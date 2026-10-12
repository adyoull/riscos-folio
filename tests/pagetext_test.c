/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Text for reading aloud (folio_get_page_text), run under qemu by make test:
 *   pagetext_test tests/data
 * speech.pdf: a heading, then a paragraph with "exam-" / "ple" at a line
 * end; page 2 one sentence. book.epub: a chapter's text. */
#include "folio.h"
#include <stdio.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/data";
	char path[512];
	folio_ctx *fc = folio_new(64 << 20);
	folio_doc *d;
	folio_page_text t;
	const char *p;
	int i, ok;

	snprintf(path, sizeof path, "%s/speech.pdf", dir);
	d = folio_open(fc, path, NULL);
	CHECK(d != NULL, "open speech.pdf: %s", folio_error(fc));
	if (!d)
		return 1;
	CHECK(folio_get_page_text(d, (folio_loc){ 0, 0 }, &t) == 0, "page 1: %s", folio_error(fc));
	printf("page 1: [%s]\n", t.text);
	CHECK(strstr(t.text, "Chapter One") != NULL, "heading");
	CHECK(strstr(t.text, "Chapter One\n\n") != NULL, "heading is its own paragraph");
	CHECK(strstr(t.text, "an example of a word") != NULL, "hyphenated word joined");
	CHECK(strstr(t.text, "exam-") == NULL, "hyphen left in");
	CHECK(t.len == (int)strlen(t.text), "len %d", t.len);
	for (ok = 1, i = 0; i < t.len; i++)
		if (t.byte_char[i] < -1 || t.byte_char[i] >= t.nchars)
			ok = 0;
	CHECK(ok, "byte_char out of range");
	/* Every letter has a character; added spaces and breaks don't. */
	p = strstr(t.text, "quick");
	CHECK(p && t.byte_char[p - t.text] >= 0, "letters have characters");
	p = strstr(t.text, "\n\n");
	CHECK(p && t.byte_char[p - t.text] == -1, "breaks have none");
	/* "q" before "u" on the line; "ple" on the line below "exam". */
	p = strstr(t.text, "quick");
	if (p) {
		folio_quad q0 = t.quads[t.byte_char[p - t.text]], q1 = t.quads[t.byte_char[p + 1 - t.text]];
		CHECK(q0.ul.x < q1.ul.x && q0.ul.y == q1.ul.y, "q then u");
	}
	p = strstr(t.text, "example");
	if (p) {
		folio_quad qe = t.quads[t.byte_char[p - t.text]], qp = t.quads[t.byte_char[p + 4 - t.text]];
		CHECK(qp.ul.y > qe.ul.y + 5, "ple below exam (%.1f vs %.1f)", qp.ul.y, qe.ul.y);
	}
	folio_page_text_free(&t);
	CHECK(folio_get_page_text(d, (folio_loc){ 0, 1 }, &t) == 0 && strstr(t.text, "one more sentence."),
		"page 2");
	folio_page_text_free(&t);
	folio_close(d);

	snprintf(path, sizeof path, "%s/book.epub", dir);
	d = folio_open(fc, path, NULL);
	CHECK(d != NULL, "open book.epub");
	if (d) {
		folio_count_step(d, 1 << 20);
		CHECK(folio_get_page_text(d, (folio_loc){ 1, 0 }, &t) == 0 && t.len > 20 && t.nchars > 20,
			"epub text: %d bytes", t.len);
		printf("epub: [%.80s...]\n", t.text);
		folio_page_text_free(&t);
		folio_close(d);
	}
	folio_drop(fc);
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
