/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Text recognition (core/ocr.c), run under qemu by make test:
 *   ocr_test tests/data TESSDATA
 * TESSDATA holds eng.traineddata. scan.pdf: page 1 is speech.pdf's page 1
 * as a 300 dpi grey picture ("Chapter One", then "The quick brown fox jumps
 * over the lazy dog. It is an exam-" with its baseline at page y 142);
 * page 2 is real text. */
#include "folio.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static double now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

static void *stopper(void *arg)
{
	usleep(700 * 1000);
	folio_abort(arg);
	return NULL;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/data";
	const char *data = argc > 2 ? argv[2] : "tessdata";
	char path[512], save[512];
	folio_ctx *fc = folio_new(64 << 20);
	folio_doc *d, *other;
	folio_loc p1 = { 0, 0 }, p2 = { 0, 1 };
	folio_quad q[16];
	int mark[16], n;
	folio_page_text t;
	double t0;
	uint32_t *pix;
	folio_irect tile = { 0, 0, 100, 140 };

	snprintf(path, sizeof path, "%s/book.epub", dir);
	d = folio_open(fc, path, NULL);
	CHECK(d && !folio_ocr_supported(d), "EPUBs have no scans");
	folio_close(d);

	snprintf(path, sizeof path, "%s/scan.pdf", dir);
	d = folio_open(fc, path, NULL);
	CHECK(d != NULL, "open scan.pdf: %s", folio_error(fc));
	if (!d)
		return 1;
	CHECK(folio_ocr_supported(d), "PDFs can be recognised");
	CHECK(folio_page_is_scan(d, p1) == 1, "page 1 is a scan");
	CHECK(folio_page_is_scan(d, p2) == 0, "page 2 has text");
	CHECK(folio_find_on_page(d, p1, "quick brown", q, mark, 16) == 0, "no text before OCR");
	CHECK(folio_ocr_page(d, p2, data, "eng", 1) == FOLIO_OCR_NOT_SCAN, "page 2 left alone");
	CHECK(!folio_page_has_ocr(d, p2), "page 2 not recognised");

	/* Stopped part way. */
	{
		pthread_t th;
		pthread_create(&th, NULL, stopper, d);
		n = folio_ocr_page(d, p1, data, "eng", 1);
		pthread_join(th, NULL);
		CHECK(n == -1 && folio_aborted(d), "stopped: %d %s", n, folio_error(fc));
		CHECK(!folio_page_has_ocr(d, p1), "nothing kept when stopped");
	}

	t0 = now();
	n = folio_ocr_page(d, p1, data, "eng", 1);
	printf("page 1: %d characters in %.1f s\n", n, now() - t0);
	CHECK(n > 60, "recognised: %d %s", n, folio_error(fc));
	CHECK(folio_page_has_ocr(d, p1) && folio_ocr_pages(d) == 1 && folio_ocr_changed(d), "kept");

	n = folio_find_on_page(d, p1, "quick brown fox", q, mark, 16);
	CHECK(n >= 1, "found after OCR: %d", n);
	if (n >= 1) {
		printf("hit at %.1f,%.1f - %.1f,%.1f\n", q[0].ul.x, q[0].ul.y, q[0].lr.x, q[0].lr.y);
		/* "The quick" starts at x 72; "quick" about 25 pt along; baseline 142. */
		CHECK(q[0].ul.x > 85 && q[0].ul.x < 115, "hit x %.1f", q[0].ul.x);
		CHECK(q[0].ul.y > 120 && q[0].ul.y < 140 && q[0].lr.y > 136 && q[0].lr.y < 150,
			"hit y %.1f-%.1f", q[0].ul.y, q[0].lr.y);
	}
	CHECK(folio_get_page_text(d, p1, &t) == 0, "page text");
	if (t.text) {
		printf("text: [%s]\n", t.text);
		CHECK(strstr(t.text, "Chapter One") != NULL, "heading read");
		CHECK(strstr(t.text, "lazy dog.") != NULL, "sentence read");
		folio_page_text_free(&t);
	}
	{
		folio_point a, b, at = { 100, 136 };
		char *w = folio_word_at(d, p1, at, &a, &b);
		CHECK(w && strcmp(w, "quick") == 0, "word at: %s", w ? w : "(none)");
		folio_free(d, w);
	}
	/* Drawing still uses the page itself (the picture). */
	pix = malloc(100 * 140 * 4);
	CHECK(folio_render_tile(d, p1, 0.2f, tile, FOLIO_PAGE_DARK, 1, pix, 400) == 0, "dark render");
	free(pix);

	/* Kept between sessions. */
	snprintf(save, sizeof save, "%s/../out/scan.ocr", dir);
	{
		char mk[600];
		snprintf(mk, sizeof mk, "%s/../out", dir);
		mkdir(mk, 0755);
	}
	CHECK(folio_ocr_save(d, save) == 0, "save: %s", folio_error(fc));
	CHECK(!folio_ocr_changed(d), "saved");
	folio_close(d);
	d = folio_open(fc, path, NULL);
	CHECK(folio_ocr_load(d, save) == 1, "loaded one page");
	CHECK(folio_find_on_page(d, p1, "lazy dog", q, mark, 16) >= 1, "found after loading");
	CHECK(!folio_ocr_changed(d), "unchanged after loading");
	snprintf(path, sizeof path, "%s/doc.pdf", dir);
	other = folio_open(fc, path, NULL);
	CHECK(other && folio_ocr_load(other, save) == -1, "another document's text refused");
	folio_close(other);
	folio_close(d);

	/* Two columns: down the first, then the second. */
	snprintf(path, sizeof path, "%s/columns.pdf", dir);
	d = folio_open(fc, path, NULL);
	CHECK(d != NULL, "open columns.pdf: %s", folio_error(fc));
	if (d) {
		n = folio_ocr_page(d, p1, data, "eng", 1);
		CHECK(n > 100, "columns recognised: %d", n);
		if (folio_get_page_text(d, p1, &t) == 0) {
			const char *l8 = strstr(t.text, "Left column line eight");
			const char *r1 = strstr(t.text, "Right column line one");
			printf("columns: [%.200s...]\n", t.text);
			CHECK(l8 && r1 && l8 < r1, "left column read before the right");
			folio_page_text_free(&t);
		}
		folio_close(d);
	}

	/* Forcing a page with text of its own. */
	snprintf(path, sizeof path, "%s/scan.pdf", dir);
	d = folio_open(fc, path, NULL);
	n = folio_ocr_page(d, p2, data, "eng", 0);
	CHECK(n > 20 && folio_find_on_page(d, p2, "second page", q, mark, 16) >= 1, "forced: %d", n);
	folio_close(d);
	folio_drop(fc);
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
