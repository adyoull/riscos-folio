/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Draw file export (core/drawfile.c), run under qemu by `make test`:
 *   draw_test tests/data
 * Exports page 1 of doc.pdf (text, a photo, a highlight, a link) and of
 * speech.pdf, and of the EPUB, to tests/out/ (.aff files), then reads each file
 * back: the header, every object's size and box, and what kinds there
 * are. tests/draw_view.py draws one as a PNG, to look at. */
#include "folio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static unsigned rd(const unsigned char *p)
{
	return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24;
}

typedef struct { int paths, sprites, tsprites, other, bad; int sprite_w, sprite_h, points; } census;

/* Walks objects from p to end. */
static void walk(const unsigned char *p, const unsigned char *end, census *c)
{
	while (p < end) {
		unsigned type, size;
		int x0, y0, x1, y1;
		if (end - p < 8) {
			c->bad++;
			return;
		}
		type = rd(p);
		size = rd(p + 4);
		if (size < 24 || size > (unsigned)(end - p) || (size & 3)) {
			c->bad++;
			return;
		}
		x0 = (int)rd(p + 8); y0 = (int)rd(p + 12); x1 = (int)rd(p + 16); y1 = (int)rd(p + 20);
		if (x1 < x0 || y1 < y0)
			c->bad++;
		if (type == 2) {
			/* fill, outline, width, style, [dash], elements to tag 0 */
			const unsigned char *q = p + 24 + 16, *qe = p + size;
			unsigned style = rd(p + 36);
			if (style & 0x80)
				q += 8 + 4 * rd(q + 4);
			while (q < qe) {
				unsigned tag = rd(q) & 255;
				q += 4;
				if (tag == 0)
					break;
				if (tag == 2 || tag == 8) { q += 8; c->points++; }
				else if (tag == 6) { q += 24; c->points += 3; }
				else if (tag != 5) { c->bad++; break; }
			}
			if (q != qe)
				c->bad++;
			c->paths++;
		} else if (type == 5 || type == 13) {
			const unsigned char *s = p + 24 + (type == 13 ? 24 : 0);
			unsigned ssize = rd(s), mode = rd(s + 40);
			if (s + ssize != p + size || (mode >> 27) != 6)
				c->bad++;
			c->sprite_w = (int)rd(s + 16) + 1;
			c->sprite_h = (int)rd(s + 20) + 1;
			if (type == 5) c->sprites++; else c->tsprites++;
		} else if (type == 6) {
			walk(p + 36, p + size, c);
		} else {
			c->other++;
		}
		p += size;
	}
}

static census check_file(const char *path, const char *what)
{
	census c;
	FILE *f = fopen(path, "rb");
	unsigned char *buf;
	long n;
	memset(&c, 0, sizeof c);
	CHECK(f != NULL, "%s: written", what);
	if (!f)
		return c;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	buf = malloc(n);
	if (fread(buf, 1, n, f) != (size_t)n)
		n = 0;
	fclose(f);
	CHECK(n > 40 && !memcmp(buf, "Draw", 4) && rd(buf + 4) == 201, "%s: Draw header", what);
	CHECK(n > 40 && !memcmp(buf + 12, "Folio       ", 12), "%s: creator", what);
	if (n > 40) {
		CHECK((int)rd(buf + 32) > (int)rd(buf + 24) && (int)rd(buf + 36) > (int)rd(buf + 28), "%s: box", what);
		walk(buf + 40, buf + n, &c);
	}
	CHECK(c.bad == 0, "%s: %d bad objects", what, c.bad);
	printf("%s: %ld bytes, %d paths (%d points), %d sprites, %d turned sprites\n", what, n, c.paths,
		c.points, c.sprites, c.tsprites);
	free(buf);
	return c;
}

static char *data(const char *dir, const char *name)
{
	static char p[512];
	snprintf(p, sizeof p, "%s/%s", dir, name);
	return p;
}

int main(int argc, char **argv)
{
	folio_ctx *fc;
	folio_doc *d;
	census c;
	const char *dir = argc > 1 ? argv[1] : "tests/data";
	fc = folio_new(64 << 20);
	/* draw_test --export FILE PAGE OUT: just export (a page of any document). */
	if (argc == 5 && !strcmp(argv[1], "--export")) {
		folio_loc loc;
		d = folio_open(fc, argv[2], NULL);
		if (!d) {
			printf("%s\n", folio_error(fc));
			return 1;
		}
		while (folio_count_step(d, 1) == 0)
			;
		if (folio_loc_from_number(d, atoi(argv[3]) - 1, &loc) || folio_export_draw(d, loc, argv[4])) {
			printf("%s\n", folio_error(fc));
			return 1;
		}
		check_file(argv[4], argv[2]);
		return fails != 0;
	}
	mkdir("tests/out", 0777);

	d = folio_open(fc, data(dir, "doc.pdf"), NULL);
	CHECK(d != NULL, "open doc.pdf: %s", folio_error(fc));
	if (d) {
		CHECK(folio_export_draw(d, (folio_loc){ 0, 0 }, "tests/out/doc-p1.aff") == 0, "export: %s", folio_error(fc));
		c = check_file("tests/out/doc-p1.aff", "doc.pdf page 1");
		/* Text (title, two lines) as outlines; the highlight; the photo
		 * as a 64 x 40 sprite of its own. */
		CHECK(c.paths >= 2 && c.points > 100, "text and highlight as paths (%d, %d points)", c.paths, c.points);
		CHECK(c.sprites + c.tsprites == 1 && c.sprite_w == 64 && c.sprite_h == 40,
			"the photo as one 64x40 sprite (%d+%d, %dx%d)", c.sprites, c.tsprites, c.sprite_w, c.sprite_h);
		CHECK(folio_export_draw(d, (folio_loc){ 0, 1 }, "tests/out/doc-p2.aff") == 0, "export page 2");
		c = check_file("tests/out/doc-p2.aff", "doc.pdf page 2");
		CHECK(c.paths >= 1 && c.sprites == 0, "page 2: text only");
		folio_close(d);
	}
	d = folio_open(fc, data(dir, "book.epub"), NULL);
	CHECK(d != NULL, "open book.epub: %s", folio_error(fc));
	if (d) {
		folio_count_step(d, 1);
		CHECK(folio_export_draw(d, (folio_loc){ 0, 0 }, "tests/out/book-p1.aff") == 0, "export EPUB: %s",
			folio_error(fc));
		c = check_file("tests/out/book-p1.aff", "book.epub page 1");
		CHECK(c.paths >= 1, "EPUB text as paths");
		folio_close(d);
	}
	folio_drop(fc);
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
