/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Folio core tests. Run under qemu-arm-aligntrap (RISC OS alignment rules,
 * Cortex-A8) by `make test`:  core_test DATA_DIR
 * Writes PPM pictures to tests/out for looking at by eye. */
#include "folio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static char path_buf[512];
static const char *data(const char *dir, const char *leaf)
{
	snprintf(path_buf, sizeof path_buf, "%s/%s", dir, leaf);
	return path_buf;
}

static void write_ppm(const char *name, const uint32_t *px, int w, int h, int stride)
{
	char p[256];
	FILE *f;
	int x, y;
	mkdir("tests/out", 0777);
	snprintf(p, sizeof p, "tests/out/%s.ppm", name);
	f = fopen(p, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			const unsigned char *b = (const unsigned char *)(px + y * (stride / 4) + x);
			fwrite(b, 1, 3, f);
		}
	fclose(f);
}

static void rgb(uint32_t px, int *r, int *g, int *b)
{
	const unsigned char *c = (const unsigned char *)&px;  /* R,G,B,0 in memory */
	*r = c[0]; *g = c[1]; *b = c[2];
}

static folio_point quad_centre(folio_quad q)
{
	folio_point p = { (q.ul.x + q.lr.x) / 2, (q.ul.y + q.lr.y) / 2 };
	return p;
}

/* Renders a whole page at zoom; returns malloc'd pixels, sets w,h. */
static uint32_t *render_page(folio_doc *d, folio_loc loc, float zoom, folio_page_mode mode,
		int annots, int *w, int *h)
{
	folio_rect b;
	folio_irect t;
	uint32_t *px;
	if (folio_page_bounds(d, loc, &b))
		return NULL;
	*w = (int)(b.x1 * zoom + 0.5f);
	*h = (int)(b.y1 * zoom + 0.5f);
	px = malloc((size_t)*w * *h * 4);
	t.x0 = 0; t.y0 = 0; t.x1 = *w; t.y1 = *h;
	if (folio_render_tile(d, loc, zoom, t, mode, annots, px, *w * 4)) {
		free(px);
		return NULL;
	}
	return px;
}

static int find_colour(const uint32_t *px, int w, int h, int r0, int g0, int b0, int *fx, int *fy)
{
	int x, y, r, g, b;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			rgb(px[y * w + x], &r, &g, &b);
			if (abs(r - r0) < 12 && abs(g - g0) < 12 && abs(b - b0) < 12) {
				*fx = x; *fy = y;
				return 1;
			}
		}
	return 0;
}

static void test_epub(folio_ctx *fc, const char *dir)
{
	folio_doc *d;
	folio_loc loc = { 0, 0 }, target = { -1, -1 };
	const folio_outline_item *it;
	int n, i, exact, total, steps, rc, chap_counts[8];
	char accel[512];

	snprintf(accel, sizeof accel, "%s/book.accel", dir);
	remove(accel);

	d = folio_open(fc, data(dir, "book.epub"), accel);
	CHECK(d, "open book.epub: %s", folio_error(fc));
	if (!d)
		return;
	CHECK(folio_is_reflowable(d), "EPUB is reflowable");
	CHECK(!strncmp(folio_format(d), "EPUB", 4), "format %s", folio_format(d));
	CHECK(!strcmp(folio_title(d), "Folio Test Book"), "title '%s'", folio_title(d));
	CHECK(!strcmp(folio_author(d), "A. Tester"), "author '%s'", folio_author(d));
	CHECK(!strcmp(folio_language(d), "en-GB"), "language '%s'", folio_language(d));
	CHECK(folio_chapters(d) == 4, "chapters %d", folio_chapters(d));

	/* Nothing counted yet: an estimate. */
	total = folio_page_count(d, &exact);
	CHECK(!exact, "not exact before counting");
	CHECK(folio_page_number(d, (folio_loc){ 2, 0 }) == -1, "page number unknown before counting");

	/* Count a chapter per step, as null polls will. */
	steps = 0;
	while ((rc = folio_count_step(d, 1)) == 0)
		steps++;
	CHECK(rc == 1, "count_step finished: %s", folio_error(fc));
	CHECK(steps == 3, "4 chapters take 3 unfinished steps + 1, got %d", steps);
	total = folio_page_count(d, &exact);
	CHECK(exact && total >= 6, "total %d exact %d", total, exact);
	for (i = 0, n = 0; i < 4; i++) {
		chap_counts[i] = folio_chapter_pages(d, i);
		n += chap_counts[i];
	}
	CHECK(n == total, "chapter sum %d == total %d", n, total);
	CHECK(chap_counts[2] > 3, "chapter 3 is long: %d pages", chap_counts[2]);
	printf("  EPUB: %d pages (%d/%d/%d/%d)\n", total, chap_counts[0], chap_counts[1], chap_counts[2], chap_counts[3]);

	/* Walk forward and back over every page. */
	for (n = 1; folio_next(d, &loc) == 1; n++)
		;
	CHECK(n == total, "next visits %d of %d pages", n, total);
	CHECK(loc.chapter == 3, "ends in chapter 4 (%d)", loc.chapter);
	for (n = 1; folio_prev(d, &loc) == 1; n++)
		;
	CHECK(n == total && loc.chapter == 0 && loc.page == 0, "prev back to start (%d)", n);
	for (i = 0; i < total; i++) {
		folio_loc l;
		if (folio_loc_from_number(d, i, &l) || folio_page_number(d, l) != i) {
			CHECK(0, "page number round trip at %d", i);
			break;
		}
	}

	/* Contents. */
	CHECK(folio_outline(d, &it, &n) == 0, "outline: %s", folio_error(fc));
	CHECK(n == 5, "5 contents entries, got %d", n);
	if (n == 5) {
		CHECK(!strcmp(it[0].title, "Chapter One") && it[0].depth == 0 && it[0].loc.chapter == 0, "entry 0");
		CHECK(!strcmp(it[2].title, "Chapter Three") && it[2].loc.chapter == 2 && it[2].loc.page == 0, "entry 2");
		CHECK(!strcmp(it[3].title, "The Target") && it[3].depth == 1 && it[3].loc.chapter == 2 && it[3].loc.page > 0,
			"entry 3 %s depth %d at %d/%d", it[3].title, it[3].depth, it[3].loc.chapter, it[3].loc.page);
		CHECK(!strcmp(it[4].title, "Chapter Four") && it[4].loc.chapter == 3, "entry 4");
		target = it[3].loc;
	}

	/* Find "zephyr": two hits in chapter 3; check the word under the first. */
	{
		folio_quad q[16], first;
		int mark[16], hits = 0, got_first = 0;
		folio_loc l = { 2, 0 }, first_loc = l;
		for (i = 0; i < chap_counts[2]; i++) {
			l.page = i;
			n = folio_find_on_page(d, l, "ZEPHYR", q, mark, 16);  /* case-insensitive */
			CHECK(n >= 0, "find: %s", folio_error(fc));
			hits += n;
			if (n > 0 && !got_first) {
				first = q[0];
				first_loc = l;
				got_first = 1;
			}
		}
		CHECK(hits == 2, "zephyr hits %d", hits);
		if (got_first) {
			folio_point a, b;
			char *w = folio_word_at(d, first_loc, quad_centre(first), &a, &b);
			CHECK(w && !strcmp(w, "zephyr"), "word at hit '%s'", w ? w : "(null)");
			folio_free(d, w);
		}
	}

	/* The link in chapter 1 leads to the target. */
	{
		folio_quad q[4];
		int mark[4];
		folio_link link;
		n = folio_find_on_page(d, (folio_loc){ 0, 0 }, "Go to the target", q, mark, 4);
		CHECK(n >= 1, "link text found");
		if (n >= 1) {
			rc = folio_link_at(d, (folio_loc){ 0, 0 }, quad_centre(q[0]), &link);
			CHECK(rc == 1, "link under the text (%d) %s", rc, folio_error(fc));
			if (rc == 1)
				CHECK(link.dest.chapter == target.chapter && link.dest.page == target.page,
					"link goes to %d/%d, target %d/%d", link.dest.chapter, link.dest.page,
					target.chapter, target.page);
		}
		/* A point in the margin has no link. */
		CHECK(folio_link_at(d, (folio_loc){ 0, 0 }, (folio_point){ 2, 2 }, &link) == 0, "no link in margin");
	}

	/* Rendering: whole page, tiles, modes. */
	{
		int w, h, r, g, b, x, y, same = 1, alpha0 = 1;
		uint32_t *whole = render_page(d, (folio_loc){ 0, 0 }, 1.5f, FOLIO_PAGE_NORMAL, 1, &w, &h);
		CHECK(whole, "render: %s", folio_error(fc));
		if (whole) {
			uint32_t *tiled = calloc((size_t)w * h, 4);
			CHECK(w == 600 && h == 900, "page 400x600pt at 1.5 = %dx%d", w, h);
			rgb(whole[0], &r, &g, &b);
			CHECK(r == 255 && g == 255 && b == 255, "white paper %d,%d,%d", r, g, b);
			for (i = 0; i < w * h; i++)
				if (whole[i] >> 24)
					alpha0 = 0;
			CHECK(alpha0, "the sprite's top byte is 0");
			/* The same page drawn as 128x128 tiles into one buffer. */
			for (y = 0; y < h; y += 128)
				for (x = 0; x < w; x += 128) {
					folio_irect t = { x, y, x + 128 > w ? w : x + 128, y + 128 > h ? h : y + 128 };
					if (folio_render_tile(d, (folio_loc){ 0, 0 }, 1.5f, t, FOLIO_PAGE_NORMAL, 1,
							tiled + y * w + x, w * 4))
						same = 0;
				}
			for (i = 0; i < w * h && same; i++) {
				int r2, g2, b2;
				rgb(whole[i], &r, &g, &b);
				rgb(tiled[i], &r2, &g2, &b2);
				if (abs(r - r2) > 2 || abs(g - g2) > 2 || abs(b - b2) > 2)
					same = 0;
			}
			CHECK(same, "tiles match the whole page");
			write_ppm("epub-c1p1", whole, w, h, w * 4);
			free(tiled);
			free(whole);
		}
		/* Chapter 2 has the red picture: kept red in dark mode. */
		whole = render_page(d, (folio_loc){ 1, 0 }, 1.0f, FOLIO_PAGE_NORMAL, 1, &w, &h);
		if (whole) {
			int fx, fy;
			uint32_t *dark = render_page(d, (folio_loc){ 1, 0 }, 1.0f, FOLIO_PAGE_DARK, 1, &w, &h);
			uint32_t *sepia = render_page(d, (folio_loc){ 1, 0 }, 1.0f, FOLIO_PAGE_SEPIA, 1, &w, &h);
			CHECK(find_colour(whole, w, h, 220, 30, 30, &fx, &fy), "red picture drawn");
			if (dark) {
				rgb(dark[0], &r, &g, &b);
				CHECK(r == 0 && g == 0 && b == 0, "dark paper is pure black: %d,%d,%d", r, g, b);
				if (find_colour(whole, w, h, 220, 30, 30, &fx, &fy)) {
					/* Look a few pixels inside the picture's edge. */
					rgb(dark[(fy + 4) * w + fx + 4], &r, &g, &b);
					CHECK(abs(r - 220) < 12 && g < 45 && b < 45, "picture kept in dark mode: %d,%d,%d", r, g, b);
				}
				write_ppm("epub-c2-dark", dark, w, h, w * 4);
			}
			if (sepia) {
				rgb(sepia[0], &r, &g, &b);
				CHECK(r == 0xf4 && g == 0xec && b == 0xd8, "sepia paper %d,%d,%d", r, g, b);
				if (find_colour(whole, w, h, 220, 30, 30, &fx, &fy)) {
					rgb(sepia[(fy + 4) * w + fx + 4], &r, &g, &b);
					CHECK(abs(r - 220) < 12 && g < 45 && b < 45, "picture kept in sepia: %d,%d,%d", r, g, b);
				}
				write_ppm("epub-c2-sepia", sepia, w, h, w * 4);
			}
			write_ppm("epub-c2", whole, w, h, w * 4);
			free(dark);
			free(sepia);
			free(whole);
		}
	}

	/* Accelerator: save, reopen, and every chapter is known at once. */
	CHECK(folio_save_accel(d, accel) == 0, "save accel: %s", folio_error(fc));
	{
		folio_mark m;
		int have_mark = target.chapter >= 0 && folio_make_mark(d, target, &m) == 0;
		CHECK(have_mark, "make mark: %s", folio_error(fc));
		if (have_mark) {
			printf("  mark: chapter %d, %.2f, '%s'\n", m.chapter, m.fraction, m.text);
			CHECK(m.chapter == 2 && m.text[0], "mark has chapter 3 and words");
		}
		/* A bigger font: more pages; the mark still finds its words. */
		{
			folio_layout lay;
			int total2;
			folio_default_layout(&lay);
			lay.em = 16;
			CHECK(folio_set_layout(d, &lay) == 0, "set layout: %s", folio_error(fc));
			while (folio_count_step(d, 4) == 0)
				;
			total2 = folio_page_count(d, &exact);
			CHECK(exact && total2 > total, "16pt: %d pages > %d", total2, total);
			if (have_mark) {
				folio_loc l;
				folio_quad q[4];
				int mk[4];
				char start[16];
				CHECK(folio_find_mark(d, &m, &l) == 0, "find mark: %s", folio_error(fc));
				CHECK(l.chapter == 2, "mark chapter %d", l.chapter);
				snprintf(start, sizeof start, "%.12s", m.text);
				n = folio_find_on_page(d, l, start, q, mk, 4);
				CHECK(n >= 1, "16pt page %d/%d holds '%s'", l.chapter, l.page, start);
				printf("  mark at 11pt %d/%d -> 16pt %d/%d\n", target.chapter, target.page, l.chapter, l.page);
				/* Back to 11pt: the same page as before. */
				folio_default_layout(&lay);
				folio_set_layout(d, &lay);
				CHECK(folio_find_mark(d, &m, &l) == 0 && l.chapter == target.chapter && l.page == target.page,
					"back at 11pt the mark is %d/%d again (got %d/%d)", target.chapter, target.page,
					l.chapter, l.page);
			}
		}
	}
	folio_close(d);

	d = folio_open(fc, data(dir, "book.epub"), accel);
	CHECK(d, "reopen with accel: %s", folio_error(fc));
	if (d) {
		/* Without layout work, all four counts come from the accelerator. */
		rc = folio_count_step(d, 4);
		CHECK(rc == 1, "accel count done in one step");
		for (i = 0; i < 4; i++)
			CHECK(folio_chapter_pages(d, i) == chap_counts[i], "accel chapter %d: %d vs %d",
				i, folio_chapter_pages(d, i), chap_counts[i]);
		folio_close(d);
	}
	printf("  EPUB key %08x\n", folio_layout_key(&(folio_layout){ 400, 600, 11, 18, 1, 1 }));
}

static void test_pdf(folio_ctx *fc, const char *dir)
{
	folio_doc *d = folio_open(fc, data(dir, "doc.pdf"), NULL);
	folio_rect b;
	folio_quad q[4];
	int mark[4], n, exact;
	char *s;
	CHECK(d, "open doc.pdf: %s", folio_error(fc));
	if (!d)
		return;
	CHECK(!folio_is_reflowable(d), "PDF is fixed layout");
	CHECK(!strncmp(folio_format(d), "PDF", 3), "format %s", folio_format(d));
	CHECK(!strcmp(folio_title(d), "Folio Test PDF"), "title '%s'", folio_title(d));
	CHECK(!strcmp(folio_language(d), "en-GB"), "language '%s'", folio_language(d));
	CHECK(folio_chapters(d) == 1, "one chapter");
	CHECK(folio_count_step(d, 1) == 1, "counted");
	CHECK(folio_page_count(d, &exact) == 3 && exact, "3 pages");
	CHECK(folio_page_bounds(d, (folio_loc){ 0, 0 }, &b) == 0 && (int)b.x1 == 595 && (int)b.y1 == 842,
		"A4 bounds %gx%g", b.x1, b.y1);

	/* Two hits of one quad each: each marked as a start. Then a max of 1. */
	n = folio_find_on_page(d, (folio_loc){ 0, 0 }, "test", q, mark, 4);
	CHECK(n == 2 && mark[0] == 1 && mark[1] == 1, "test: %d quads, marks %d %d", n, mark[0], mark[1]);
	n = folio_find_on_page(d, (folio_loc){ 0, 0 }, "test", q, mark, 1);
	CHECK(n == 1, "max 1 gave %d", n);

	n = folio_find_on_page(d, (folio_loc){ 0, 0 }, "serendipity", q, mark, 4);
	CHECK(n == 1, "serendipity found once (%d)", n);
	if (n == 1) {
		folio_point a, e;
		char *w = folio_word_at(d, (folio_loc){ 0, 0 }, quad_centre(q[0]), &a, &e);
		CHECK(w && !strcmp(w, "serendipity"), "word '%s'", w ? w : "(null)");
		CHECK(q[0].ul.y > 125 && q[0].ul.y < 142, "word sits at page y ~131: %g", q[0].ul.y);
		folio_free(d, w);
		/* Select from the word's start to the end of "the". */
		{
			folio_point s0 = { q[0].ul.x + 1, (q[0].ul.y + q[0].ll.y) / 2 };
			folio_point s1 = { q[0].ur.x + 60, s0.y };
			s = folio_selection_text(d, (folio_loc){ 0, 0 }, s0, s1);
			CHECK(s && !strncmp(s, "serendipity is", 14), "selection '%s'", s ? s : "(null)");
			folio_free(d, s);
			n = folio_selection_quads(d, (folio_loc){ 0, 0 }, s0, s1, q, 4);
			CHECK(n == 1, "one quad for a one-line selection (%d)", n);
		}
	}
	s = folio_word_at(d, (folio_loc){ 0, 0 }, (folio_point){ 500, 750 }, NULL, NULL);
	CHECK(s == NULL, "no word in blank space ('%s')", s ? s : "");
	folio_free(d, s);

	{
		folio_link link;
		int rc = folio_link_at(d, (folio_loc){ 0, 0 }, (folio_point){ 100, 70 }, &link);
		CHECK(rc == 1 && link.dest.chapter == 0 && link.dest.page == 2,
			"link to page 3 (%d: %d/%d) %s", rc, link.dest.chapter, link.dest.page, folio_error(fc));
		CHECK(rc == 1 && link.dest_y < 0, "a page with no position: top (%g)", rc == 1 ? link.dest_y : 0);
		rc = folio_link_at(d, (folio_loc){ 0, 0 }, (folio_point){ 100, 100 }, &link);
		CHECK(rc == 1 && link.dest.page == 1 && link.dest_y > 299 && link.dest_y < 301,
			"link to page 2 at y 300 (%d: page %d, y %g)", rc, link.dest.page, rc == 1 ? link.dest_y : 0);
		rc = folio_link_at(d, (folio_loc){ 0, 0 }, (folio_point){ 100, 130 }, &link);
		CHECK(rc == 1 && link.dest.page < 0 && link.uri && !strcmp(link.uri, "https://example.org/"),
			"web link (%d: %s)", rc, rc == 1 && link.uri ? link.uri : "");
		/* Links underlined in blue when asked for, and only then. */
		if (rc == 1) {
			static uint32_t px[300 * 300];
			folio_irect t = { 0, 0, 300, 300 };
			int i, blue[2] = { 0, 0 }, k;
			for (k = 0; k < 2; k++) {
				folio_render_tile(d, (folio_loc){ 0, 0 }, 1.0f, t, FOLIO_PAGE_NORMAL,
					FOLIO_RENDER_ANNOTS | (k ? FOLIO_RENDER_LINKS : 0), px, 300 * 4);
				for (i = 0; i < 300 * 300; i++) {
					const unsigned char *c = (const unsigned char *)&px[i];
					if (c[0] == 0x1a && c[1] == 0x5f && c[2] == 0xc8)
						blue[k]++;
				}
			}
			CHECK(blue[0] == 0 && blue[1] > 50, "link underlines: %d without, %d with", blue[0], blue[1]);
		}
	}

	{
		int w, h, r, g, bl, r2, g2, b2;
		uint32_t *norm = render_page(d, (folio_loc){ 0, 0 }, 1.0f, FOLIO_PAGE_NORMAL, 1, &w, &h);
		uint32_t *bare = render_page(d, (folio_loc){ 0, 0 }, 1.0f, FOLIO_PAGE_NORMAL, 0, &w, &h);
		uint32_t *dark = render_page(d, (folio_loc){ 0, 0 }, 1.0f, FOLIO_PAGE_DARK, 1, &w, &h);
		CHECK(norm && bare && dark, "PDF renders: %s", folio_error(fc));
		if (norm && bare && dark) {
			/* The photo's centre (page 192,362) is the same in both modes. */
			rgb(norm[362 * w + 192], &r, &g, &bl);
			rgb(dark[362 * w + 192], &r2, &g2, &b2);
			CHECK(abs(r - r2) < 4 && abs(g - g2) < 4 && abs(bl - b2) < 4,
				"photo kept: %d,%d,%d vs %d,%d,%d", r, g, bl, r2, g2, b2);
			rgb(dark[5 * w + 5], &r, &g, &bl);
			CHECK(r == 0 && g == 0 && bl == 0, "dark PDF paper black: %d,%d,%d", r, g, bl);
			/* The highlight (annotation) changes pixels around the word. */
			{
				int diff = 0, i;
				for (i = 0; i < w * h; i++)
					if (norm[i] != bare[i])
						diff++;
				CHECK(diff > 200, "annotations drawn (%d pixels differ)", diff);
			}
			write_ppm("pdf-p1", norm, w, h, w * 4);
			write_ppm("pdf-p1-dark", dark, w, h, w * 4);
		}
		free(norm);
		free(bare);
		free(dark);
	}
	folio_close(d);
}

/* Abort: a slow render in a thread, stopped from here. */
typedef struct { folio_doc *d; int rc; volatile int started; } abort_job;
static void *abort_worker(void *arg)
{
	abort_job *j = arg;
	folio_irect t = { 0, 0, 2400, 2400 };
	uint32_t *px = malloc((size_t)2400 * 2400 * 4);
	j->started = 1;
	j->rc = folio_render_tile(j->d, (folio_loc){ 0, 0 }, 8.0f, t, FOLIO_PAGE_DARK, 1, px, 2400 * 4);
	free(px);
	return NULL;
}

static void test_abort(folio_ctx *fc, const char *dir)
{
	folio_doc *d = folio_open(fc, data(dir, "doc.pdf"), NULL);
	abort_job j = { d, 99, 0 };
	pthread_t th;
	int w, h;
	uint32_t *px;
	if (!d)
		return;
	folio_count_step(d, 1);
	pthread_create(&th, NULL, abort_worker, &j);
	while (!j.started)
		usleep(1000);
	folio_abort(d);
	pthread_join(th, NULL);
	CHECK(j.rc == -1 && folio_aborted(d), "an aborted render fails (%d, aborted %d)", j.rc, folio_aborted(d));
	/* The next render starts afresh and works: nothing half-made was kept. */
	px = render_page(d, (folio_loc){ 0, 0 }, 1.0f, FOLIO_PAGE_NORMAL, 1, &w, &h);
	CHECK(px != NULL && !folio_aborted(d), "render after an abort works");
	if (px) {
		int r, g, b;
		rgb(px[362 * w + 192], &r, &g, &b);
		CHECK(!(r == 255 && g == 255 && b == 255), "the photo is there after the abort (%d,%d,%d)", r, g, b);
	}
	free(px);
	folio_close(d);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/data";
	folio_ctx *fc = folio_new(64 << 20);
	if (!fc) {
		printf("FAIL: no context\n");
		return 1;
	}
	CHECK(folio_open(fc, data(dir, "missing.pdf"), NULL) == NULL && *folio_error(fc),
		"missing file reports an error");
	printf("EPUB\n");
	test_epub(fc, dir);
	printf("PDF\n");
	test_pdf(fc, dir);
	printf("Abort\n");
	test_abort(fc, dir);
	printf("Text ends\n");
	{
		/* footer.pdf: each page's number is first in its text but at the
		 * bottom right, where the corner nearest the page's end is. */
		folio_doc *f = folio_open(fc, data(dir, "footer.pdf"), NULL);
		CHECK(f != NULL, "open footer.pdf: %s", folio_error(fc));
		if (f) {
			folio_point st = { FOLIO_TEXT_START, 0 }, en = { FOLIO_TEXT_END, 0 };
			folio_point corner = { 595, 842 }, mid = { 49, 105 };
			folio_quad q[16];
			char *t;
			t = folio_selection_text(f, (folio_loc){ 0, 0 }, mid, corner);
			printf("  (to the corner instead: '%s')\n", t ? t : "");
			folio_free(f, t);
			t = folio_selection_text(f, (folio_loc){ 0, 0 }, mid, en);
			CHECK(t && strstr(t, "Page 1 line 2") && strstr(t, "Page 1 line 3.") && !strstr(t, "line 1"),
				"line 2 to the end: '%s'", t ? t : "(null)");
			folio_free(f, t);
			t = folio_selection_text(f, (folio_loc){ 0, 1 }, st, en);
			CHECK(t && strstr(t, "30") && strstr(t, "Page 2 line 1") && strstr(t, "Page 2 line 3."),
				"whole page 2: '%s'", t ? t : "(null)");
			folio_free(f, t);
			CHECK(folio_selection_quads(f, (folio_loc){ 0, 1 }, st, en, q, 16) == 4,
				"whole page 2 is four lines");
			folio_close(f);
		}
	}
	printf("Page sizes\n");
	{
		/* mixed.pdf: A5 (420 x 595), then A4 (595 x 842), then A4 landscape. */
		folio_doc *m = folio_open(fc, data(dir, "mixed.pdf"), NULL);
		float w0 = 0, h0 = 0, w1 = 0, h1 = 0, w2 = 0, h2 = 0;
		CHECK(m != NULL, "open mixed.pdf: %s", folio_error(fc));
		if (m) {
			CHECK(folio_page_size(m, (folio_loc){ 0, 0 }, &w0, &h0) == 0 && (int)w0 == 420 && (int)h0 == 595,
				"page 1 %gx%g", w0, h0);
			CHECK(folio_page_size(m, (folio_loc){ 0, 1 }, &w1, &h1) == 0 && (int)w1 == 595 && (int)h1 == 842,
				"page 2 %gx%g", w1, h1);
			CHECK(folio_page_size(m, (folio_loc){ 0, 2 }, &w2, &h2) == 0 && (int)w2 == 842 && (int)h2 == 595,
				"page 3 %gx%g", w2, h2);
			CHECK(folio_page_size(m, (folio_loc){ 0, 3 }, &w2, &h2) == -1, "no page 4");
			folio_close(m);
		}
	}
	folio_drop(fc);
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
