/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Folio page and picture tests (core/pages.c), run under qemu by `make test`:
 *   pages_test tests/data
 * Makes its own pictures and documents in tests/out/pages: JPEGs and a PNG,
 * a CBZ of the JPEGs, and "scanned" PDFs (a JPEG per page, one with hidden
 * text as recognised scans have, one with visible text on top). Then:
 * page lists; pictures to a PDF; each page's picture back out (the JPEG
 * byte for byte); chosen pages saved as a new PDF, from a PDF and from the
 * CBZ, with the JPEGs still byte for byte; a text page drawn as a PNG. */
#include "folio.h"
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

#define OUT "tests/out/pages"

static char pbuf[8][512];
static const char *P(int slot, const char *fmt, const char *leaf)
{
	snprintf(pbuf[slot], sizeof pbuf[slot], fmt, leaf);
	return pbuf[slot];
}

/* A file's bytes (malloc'd), and its length. */
static unsigned char *slurp(const char *path, long *len)
{
	FILE *f = fopen(path, "rb");
	unsigned char *b;
	*len = 0;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	*len = ftell(f);
	fseek(f, 0, SEEK_SET);
	b = malloc(*len ? *len : 1);
	if (b && fread(b, 1, *len, f) != (size_t)*len) {
		free(b);
		b = NULL;
	}
	fclose(f);
	return b;
}

static int same_file(const char *a, const char *b)
{
	long la, lb;
	unsigned char *x = slurp(a, &la), *y = slurp(b, &lb);
	int same = x && y && la == lb && !memcmp(x, y, la);
	free(x);
	free(y);
	return same;
}

static int is_png(const char *path, int *w, int *h)
{
	long n;
	unsigned char *b = slurp(path, &n);
	int ok = b && n > 24 && !memcmp(b, "\x89PNG\r\n\x1a\n", 8);
	if (ok) {
		*w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
		*h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
	}
	free(b);
	return ok;
}

/* A w x h picture with a pattern that differs with 'seed', 150 dpi. */
static fz_pixmap *picture(fz_context *ctx, int w, int h, int seed)
{
	fz_pixmap *pix = fz_new_pixmap(ctx, fz_device_rgb(ctx), w, h, NULL, 0);
	int x, y;
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			unsigned char *p = pix->samples + y * pix->stride + x * 3;
			p[0] = (unsigned char)(x * 255 / w);
			p[1] = (unsigned char)(y * 255 / h);
			p[2] = (unsigned char)(((x / 16 + y / 16 + seed) & 1) ? 230 : 40);
		}
	fz_set_pixmap_resolution(ctx, pix, 150, 150);
	return pix;
}

/* A one-page PDF: the JPEG covering the page, then 'text' (if not NULL),
 * drawn in mode 3 (not drawn: a recognised scan's text) or 0 (seen). */
static void scan_pdf(fz_context *ctx, const char *jpeg, const char *text, int mode, const char *out)
{
	pdf_document *pdf = pdf_create_document(ctx);
	fz_image *img = fz_new_image_from_file(ctx, jpeg);
	fz_font *font = fz_new_base14_font(ctx, "Helvetica");
	pdf_obj *res = pdf_new_dict(ctx, pdf, 2), *xo, *fo, *page;
	fz_buffer *c = fz_new_buffer(ctx, 128);
	int xr, yr;
	float w, h;
	pdf_write_options opts = pdf_default_write_options;
	fz_image_resolution(img, &xr, &yr);
	w = img->w * 72.0f / xr;
	h = img->h * 72.0f / yr;
	xo = pdf_dict_put_dict(ctx, res, PDF_NAME(XObject), 1);
	pdf_dict_puts_drop(ctx, xo, "I0", pdf_add_image(ctx, pdf, img));
	fo = pdf_dict_put_dict(ctx, res, PDF_NAME(Font), 1);
	pdf_dict_puts_drop(ctx, fo, "F1", pdf_add_simple_font(ctx, pdf, font, PDF_SIMPLE_ENCODING_LATIN));
	fz_append_printf(ctx, c, "q %g 0 0 %g 0 0 cm /I0 Do Q\n", w, h);
	if (text)
		fz_append_printf(ctx, c, "BT %d Tr /F1 24 Tf 20 40 Td (%s) Tj ET\n", mode, text);
	page = pdf_add_page(ctx, pdf, fz_make_rect(0, 0, w, h), 0, res, c);
	pdf_insert_page(ctx, pdf, -1, page);
	pdf_save_document(ctx, pdf, out, &opts);
	pdf_drop_obj(ctx, page);
	pdf_drop_obj(ctx, res);
	fz_drop_buffer(ctx, c);
	fz_drop_font(ctx, font);
	fz_drop_image(ctx, img);
	pdf_drop_document(ctx, pdf);
}

static void make_data(void)
{
	fz_context *ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	fz_zip_writer *zip;
	int i;
	fz_register_document_handlers(ctx);
	for (i = 0; i < 3; i++) {
		char name[32];
		fz_pixmap *pix = picture(ctx, 300 + 20 * i, 420, i);
		snprintf(name, sizeof name, OUT "/scan%d.jpg", i + 1);
		fz_save_pixmap_as_jpeg(ctx, pix, name, 80);
		if (i == 2)
			fz_save_pixmap_as_png(ctx, pix, OUT "/scan3.png");
		fz_drop_pixmap(ctx, pix);
	}
	/* A CBZ of the three JPEGs. */
	zip = fz_new_zip_writer(ctx, OUT "/book.cbz");
	for (i = 0; i < 3; i++) {
		char name[32], leaf[16];
		fz_buffer *b;
		snprintf(name, sizeof name, OUT "/scan%d.jpg", i + 1);
		snprintf(leaf, sizeof leaf, "p%d.jpg", i + 1);
		b = fz_read_file(ctx, name);
		fz_write_zip_entry(ctx, zip, leaf, b, 0);
		fz_drop_buffer(ctx, b);
	}
	fz_close_zip_writer(ctx, zip);
	fz_drop_zip_writer(ctx, zip);
	/* A JPEG that doesn't say its resolution, as a scanner's may not. */
	{
		fz_pixmap *pix = picture(ctx, 400, 200, 5);
		fz_set_pixmap_resolution(ctx, pix, 0, 0);
		fz_save_pixmap_as_jpeg(ctx, pix, OUT "/nores.jpg", 80);
		fz_drop_pixmap(ctx, pix);
	}
	scan_pdf(ctx, OUT "/scan1.jpg", "hidden words", 3, OUT "/hidden.pdf");
	scan_pdf(ctx, OUT "/scan1.jpg", "VISIBLE", 0, OUT "/ontop.pdf");
	fz_drop_context(ctx);
}

static int pages(folio_doc *d)
{
	int exact = 0;
	while (folio_count_step(d, 100) == 0)
		;
	return folio_page_count(d, &exact);
}

static folio_loc L(int page)
{
	folio_loc l = { 0, page };
	return l;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/data";
	folio_ctx *fc;
	folio_doc *d;
	unsigned char sel[20];
	int k, w, h;
	float pw, ph;

	mkdir("tests/out", 0777);
	mkdir(OUT, 0777);
	make_data();
	fc = folio_new(0);

	printf("Page lists\n");
	CHECK(folio_parse_pages("1-3, 5", 10, sel) == 4 && sel[0] && sel[2] && !sel[3] && sel[4], "1-3, 5");
	CHECK(folio_parse_pages("", 10, sel) == 10 && sel[9], "blank = all");
	CHECK(folio_parse_pages("All", 10, sel) == 10, "all");
	CHECK(folio_parse_pages("8-", 10, sel) == 3 && sel[7] && sel[9] && !sel[6], "8-");
	CHECK(folio_parse_pages("-2", 10, sel) == 2 && sel[0] && sel[1], "-2");
	CHECK(folio_parse_pages("9-20", 10, sel) == 2, "9-20 stops at the end");
	CHECK(folio_parse_pages("3,3,3", 10, sel) == 1, "repeats once");
	CHECK(folio_parse_pages("11", 10, sel) == -1, "past the end");
	CHECK(folio_parse_pages("4-2", 10, sel) == -1, "backwards");
	CHECK(folio_parse_pages("1-2x", 10, sel) == -1, "junk");
	CHECK(folio_parse_pages("0", 10, sel) == -1, "page 0");

	printf("Pictures to a PDF\n");
	{
		const char *files[3] = { OUT "/scan1.jpg", OUT "/scan2.jpg", OUT "/scan3.png" };
		CHECK(folio_images_to_pdf(fc, files, 3, OUT "/made.pdf") == 0, "made: %s", folio_error(fc));
		CHECK(folio_images_to_pdf(fc, files, 0, OUT "/none.pdf") == -1, "no pictures");
		{
			const char *bad[2] = { OUT "/scan1.jpg", OUT "/nosuch.jpg" };
			CHECK(folio_images_to_pdf(fc, bad, 2, OUT "/bad.pdf") == -1 &&
				strstr(folio_error(fc), "nosuch.jpg") != NULL, "a missing file is named: %s", folio_error(fc));
		}
	}
	d = folio_open(fc, OUT "/made.pdf", NULL);
	CHECK(d != NULL, "open made.pdf: %s", folio_error(fc));
	if (d) {
		CHECK(pages(d) == 3, "3 pages");
		/* 300 x 420 pixels at 150 dpi = 144 x 201.6 points. */
		CHECK(folio_page_size(d, L(0), &pw, &ph) == 0 && pw > 143.9f && pw < 144.1f &&
			ph > 201.5f && ph < 201.7f, "page 1 is the picture's size: %g x %g", pw, ph);

		{
		/* 400 pixels at the 200 dpi given: 144 points. */
		const char *nores[1] = { OUT "/nores.jpg" };
		folio_doc *dn;
		CHECK(folio_images_to_pdf_res(fc, nores, 1, 200, OUT "/nores.pdf") == 0, "nores: %s", folio_error(fc));
		dn = folio_open(fc, OUT "/nores.pdf", NULL);
		CHECK(dn && folio_page_size(dn, L(0), &pw, &ph) == 0 && pw > 143.9f && pw < 144.1f,
			"pictures taken at the resolution given (%g)", pw);
		if (dn)
			folio_close(dn);
	}

	printf("Page pictures\n");
		k = folio_save_page_image(d, L(0), 150, OUT "/out1");
		CHECK(k == FOLIO_PAGEIMG_JPEG, "page 1: the JPEG (%d)", k);
		CHECK(same_file(OUT "/out1", OUT "/scan1.jpg"), "page 1: byte for byte");
		k = folio_save_page_image(d, L(1), 150, OUT "/out2");
		CHECK(k == FOLIO_PAGEIMG_JPEG && same_file(OUT "/out2", OUT "/scan2.jpg"), "page 2 (%d)", k);
		k = folio_save_page_image(d, L(2), 150, OUT "/out3");
		CHECK(k == FOLIO_PAGEIMG_IMAGE_PNG, "page 3: the picture as PNG (%d)", k);
		CHECK(is_png(OUT "/out3", &w, &h) && w == 340 && h == 420, "page 3: 340 x 420 (%d x %d)", w, h);

		printf("Saving pages\n");
		CHECK(folio_parse_pages("2-3", 3, sel) == 2, "2-3");
		CHECK(folio_save_pages(d, sel, 3, OUT "/two.pdf") == 0, "save 2-3: %s", folio_error(fc));
		folio_close(d);
	}
	d = folio_open(fc, OUT "/two.pdf", NULL);
	CHECK(d != NULL, "open two.pdf: %s", folio_error(fc));
	if (d) {
		CHECK(pages(d) == 2, "2 pages");
		k = folio_save_page_image(d, L(0), 150, OUT "/two1");
		CHECK(k == FOLIO_PAGEIMG_JPEG && same_file(OUT "/two1", OUT "/scan2.jpg"),
			"its page 1 is page 2's JPEG, byte for byte (%d)", k);
		folio_close(d);
	}

	printf("Scans with text\n");
	d = folio_open(fc, OUT "/hidden.pdf", NULL);
	CHECK(d != NULL, "open hidden.pdf");
	if (d) {
		k = folio_save_page_image(d, L(0), 150, OUT "/hidden1");
		CHECK(k == FOLIO_PAGEIMG_JPEG && same_file(OUT "/hidden1", OUT "/scan1.jpg"),
			"hidden text doesn't count (%d)", k);
		folio_close(d);
	}
	d = folio_open(fc, OUT "/ontop.pdf", NULL);
	CHECK(d != NULL, "open ontop.pdf");
	if (d) {
		k = folio_save_page_image(d, L(0), 100, OUT "/ontop1");
		CHECK(k == FOLIO_PAGEIMG_PAGE_PNG, "text on top: the page drawn (%d)", k);
		/* 144 x 201.6 points at 100 dpi: 200 x 280 pixels. */
		CHECK(is_png(OUT "/ontop1", &w, &h) && w == 200 && h == 280, "drawn at 100 dpi (%d x %d)", w, h);
		folio_close(d);
	}

	printf("From a CBZ\n");
	d = folio_open(fc, OUT "/book.cbz", NULL);
	CHECK(d != NULL, "open book.cbz: %s", folio_error(fc));
	if (d) {
		CHECK(pages(d) == 3, "3 pages");
		CHECK(folio_parse_pages("all", 3, sel) == 3, "all");
		CHECK(folio_save_pages(d, sel, 3, OUT "/cbz.pdf") == 0, "CBZ as PDF: %s", folio_error(fc));
		folio_close(d);
	}
	d = folio_open(fc, OUT "/cbz.pdf", NULL);
	CHECK(d != NULL, "open cbz.pdf: %s", folio_error(fc));
	if (d) {
		CHECK(pages(d) == 3, "3 pages");
		k = folio_save_page_image(d, L(2), 150, OUT "/cbz3");
		CHECK(k == FOLIO_PAGEIMG_JPEG && same_file(OUT "/cbz3", OUT "/scan3.jpg"),
			"the CBZ's JPEGs kept byte for byte (%d)", k);
		folio_close(d);
	}

	printf("Joining files\n");
	/* Pages as text. */
	{
		char path[512], *t;
		unsigned char sel[2] = { 1, 1 };
		int empty;
		folio_doc *td;
		snprintf(path, sizeof path, "%s/speech.pdf", dir);
		td = folio_open(fc, path, NULL);
		CHECK(td != NULL, "open speech.pdf");
		if (td) {
			while (folio_count_step(td, 100) == 0)
				;
			t = folio_pages_text(td, sel, 2, &empty, NULL, NULL);
			CHECK(t && empty == 0, "speech.pdf text (%d empty)", empty);
			if (t) {
				CHECK(!strncmp(t, "Chapter One\n\nThe quick brown fox", strlen("Chapter One\n\nThe quick brown fox")), "heading, blank line, paragraph: '%.40s'", t);
				CHECK(strstr(t, "It is an example of a word split") != NULL, "hyphenated word joined");
				CHECK(strstr(t, "line.\n\nThe second page has one more sentence.\n") != NULL,
					"a blank line between pages, ends with a newline");
				CHECK(!strstr(t, "\n\n\n"), "never two blank lines");
				free(t);
			}
			sel[0] = 0;
			t = folio_pages_text(td, sel, 2, &empty, NULL, NULL);
			CHECK(t && !strcmp(t, "The second page has one more sentence.\n"), "page 2 only: '%s'", t ? t : "(null)");
			free(t);
			folio_close(td);
		}
		snprintf(path, sizeof path, "%s/scan.pdf", dir);
		td = folio_open(fc, path, NULL);
		if (td) {
			while (folio_count_step(td, 100) == 0)
				;
			sel[0] = 1;
			t = folio_pages_text(td, sel, 2, &empty, NULL, NULL);
			CHECK(t && empty == 1 && !strcmp(t, "The second page has one more sentence.\n"),
				"a scan with no recognised text is an empty page (%d): '%s'", empty, t ? t : "(null)");
			free(t);
			sel[1] = 0;
			t = folio_pages_text(td, sel, 2, &empty, NULL, NULL);
			CHECK(t && empty == 1 && !t[0], "only the scan: no text");
			free(t);
			folio_close(td);
		}
		snprintf(path, sizeof path, "%s/book.epub", dir);
		td = folio_open(fc, path, NULL);
		if (td) {
			int exact = 0, n;
			unsigned char *all;
			while (folio_count_step(td, 100) == 0)
				;
			n = folio_page_count(td, &exact);
			all = malloc((size_t)n);
			memset(all, 1, (size_t)n);
			t = folio_pages_text(td, all, n, &empty, NULL, NULL);
			CHECK(t && strlen(t) > 1000, "the book's text (%d pages, %d chars)", n, t ? (int)strlen(t) : -1);
			free(t);
			free(all);
			folio_close(td);
		}
	}
	/* Netpbm pictures with no name ending, as on RISC OS (type &69E):
	 * plain and raw PBM (1 bit), PGM (grey) and PPM (colour). */
	{
		static const char *const pnm[4] = { OUT "/pbmplain", OUT "/pbmraw", OUT "/pgm", OUT "/ppm" };
		FILE *f;
		int y;
		if ((f = fopen(pnm[0], "wb"))) {
			fprintf(f, "P1\n# plain\n8 4\n");
			for (y = 0; y < 4; y++)
				fprintf(f, "1 0 1 0 1 0 1 0\n");
			fclose(f);
		}
		if ((f = fopen(pnm[1], "wb"))) {
			fprintf(f, "P4\n16 8\n");
			for (y = 0; y < 16; y++)
				fputc(y & 1 ? 0xf0 : 0x0f, f);
			fclose(f);
		}
		if ((f = fopen(pnm[2], "wb"))) {
			fprintf(f, "P5\n4 4\n255\n");
			for (y = 0; y < 16; y++)
				fputc(y * 16, f);
			fclose(f);
		}
		if ((f = fopen(pnm[3], "wb"))) {
			fprintf(f, "P6\n4 2\n255\n");
			for (y = 0; y < 24; y++)
				fputc(y * 10, f);
			fclose(f);
		}
		CHECK(folio_images_to_pdf(fc, pnm, 4, OUT "/pnm.pdf") == 0, "PBM/PGM/PPM to a PDF: %s", folio_error(fc));
		CHECK(folio_combine(fc, pnm, 4, OUT "/pnm2.pdf") == 0, "PBM/PGM/PPM combined: %s", folio_error(fc));
		d = folio_open(fc, OUT "/pnm.pdf", NULL);
		CHECK(d && pages(d) == 4, "4 Netpbm pages (%d)", d ? pages(d) : -1);
		if (d)
			folio_close(d);
		d = folio_open(fc, OUT "/pnm2.pdf", NULL);
		CHECK(d && pages(d) == 4, "4 Netpbm pages combined (%d)", d ? pages(d) : -1);
		if (d)
			folio_close(d);
		CHECK(folio_paper_dpi(16, 8) == 96, "small: 96 dpi");
		CHECK(folio_paper_dpi(2480, 3508) == 300, "A4 at 300 dpi (%d)", folio_paper_dpi(2480, 3508));
		CHECK(folio_paper_dpi(3508, 2480) == 300, "A4 landscape at 300 dpi");
		CHECK(folio_paper_dpi(2550, 3300) == 300, "Letter at 300 dpi");
		CHECK(folio_paper_dpi(4960, 7016) == 600, "A4 at 600 dpi (%d)", folio_paper_dpi(4960, 7016));
		CHECK(folio_paper_dpi(1654, 2339) == 200, "A4 at 200 dpi (%d)", folio_paper_dpi(1654, 2339));
		/* A 300 dpi A4 PBM comes out A4, joined or made. */
		if ((f = fopen(OUT "/a4pbm", "wb"))) {
			fprintf(f, "P4\n2480 3508\n");
			for (y = 0; y < 310 * 3508; y++)
				fputc(y % 310 < 2 ? 0xff : 0, f);
			fclose(f);
		}
		{
			const char *one[1] = { OUT "/a4pbm" };
			float pw2, ph2;
			CHECK(folio_combine(fc, one, 1, OUT "/a4pbm.pdf") == 0, "A4 PBM: %s", folio_error(fc));
			d = folio_open(fc, OUT "/a4pbm.pdf", NULL);
			CHECK(d && folio_page_size(d, L(0), &pw2, &ph2) == 0 && pw2 > 594 && pw2 < 596 &&
				ph2 > 841 && ph2 < 843, "A4 PBM is an A4 page (%g x %g)", d ? pw2 : 0, d ? ph2 : 0);
			if (d)
				folio_close(d);
		}
	}
	{
		/* made.pdf (scan1, scan2, scan3.png), scan2.jpg alone, the CBZ
		 * (scan1-3), hidden.pdf (scan1 + hidden text): 8 pages. */
		const char *files[4] = { OUT "/made.pdf", OUT "/scan2.jpg", OUT "/book.cbz", OUT "/hidden.pdf" };
		const char *bad[2] = { OUT "/made.pdf", OUT "/nosuch.pdf" };
		static const struct { int page; const char *jpeg; } want[] = {
			{ 0, OUT "/scan1.jpg" }, { 1, OUT "/scan2.jpg" }, { 3, OUT "/scan2.jpg" },
			{ 4, OUT "/scan1.jpg" }, { 6, OUT "/scan3.jpg" }, { 7, OUT "/scan1.jpg" } };
		int i;
		CHECK(folio_combine(fc, files, 4, OUT "/joined.pdf") == 0, "joined: %s", folio_error(fc));
		CHECK(folio_combine(fc, bad, 2, OUT "/bad2.pdf") == -1 &&
			strstr(folio_error(fc), "nosuch.pdf") != NULL, "a missing file is named: %s", folio_error(fc));
		CHECK(folio_combine(fc, files, 0, OUT "/none2.pdf") == -1, "no files");
		d = folio_open(fc, OUT "/joined.pdf", NULL);
		CHECK(d != NULL, "open joined.pdf: %s", folio_error(fc));
		if (d) {
			CHECK(pages(d) == 8, "8 pages (%d)", pages(d));
			for (i = 0; i < (int)(sizeof want / sizeof want[0]); i++) {
				char out[64];
				snprintf(out, sizeof out, OUT "/joined%d", want[i].page + 1);
				k = folio_save_page_image(d, L(want[i].page), 150, out);
				CHECK(k == FOLIO_PAGEIMG_JPEG && same_file(out, want[i].jpeg),
					"joined page %d is %s byte for byte (%d)", want[i].page + 1, want[i].jpeg, k);
			}
			k = folio_save_page_image(d, L(2), 150, OUT "/joined3");
			CHECK(k == FOLIO_PAGEIMG_IMAGE_PNG, "page 3 is still the PNG picture (%d)", k);
			CHECK(folio_page_size(d, L(3), &pw, &ph) == 0 && pw > 153.5f && pw < 154.5f,
				"the lone JPEG's page is its size at 150 dpi (%g)", pw);
			folio_close(d);
		}
	}

	printf("Turning pages\n");
	d = folio_open(fc, OUT "/made.pdf", NULL);
	if (d) {
		float w0, h0, w1, h1;
		unsigned char one[3] = { 0, 1, 0 }, all[3] = { 1, 1, 1 };
		int i;
		folio_page_size(d, L(1), &w0, &h0);
		CHECK(folio_page_rotation(d, L(1)) == 0, "page 2 upright");
		CHECK(folio_rotate_pages(d, one, 3, 90) == 0, "turn page 2: %s", folio_error(fc));
		CHECK(folio_page_rotation(d, L(1)) == 90 && folio_page_rotation(d, L(0)) == 0, "only page 2 turned");
		CHECK(folio_page_size(d, L(1), &w1, &h1) == 0 && w1 > h0 - 0.1f && w1 < h0 + 0.1f &&
			h1 > w0 - 0.1f && h1 < w0 + 0.1f, "its width and height swap (%g x %g, was %g x %g)", w1, h1, w0, h0);
		CHECK(folio_is_modified(d) && folio_can_undo(d), "an undoable change");
		CHECK(folio_rotate_pages(d, one, 3, -90) == 0 && folio_page_rotation(d, L(1)) == 0, "and back");
		for (i = 0; i < 4; i++)
			folio_rotate_pages(d, all, 3, 90);
		CHECK(folio_page_rotation(d, L(0)) == 0 && folio_page_rotation(d, L(2)) == 0, "four quarter turns: upright");
		folio_rotate_pages(d, all, 3, 180);
		CHECK(folio_page_rotation(d, L(2)) == 180, "a half turn");
		CHECK(folio_undo(d) == 0 && folio_page_rotation(d, L(2)) == 0, "undone");
		CHECK(folio_redo(d) == 0 && folio_page_rotation(d, L(2)) == 180, "redone");
		CHECK(folio_save_pdf(d, OUT "/made.pdf", OUT "/turned.pdf") == 0, "saved: %s", folio_error(fc));
		folio_close(d);
	}
	printf("Adding pages\n");
	d = folio_open(fc, OUT "/made.pdf", NULL);
	if (d) {
		const char *more[2] = { OUT "/scan2.jpg", OUT "/scan3.jpg" };
		CHECK(pages(d) == 3, "3 pages before");
		CHECK(folio_append_pictures(d, more, 2, 150) == 0, "added: %s", folio_error(fc));
		CHECK(pages(d) == 5, "5 after (%d)", pages(d));
		k = folio_save_page_image(d, L(4), 150, OUT "/added5");
		CHECK(k == FOLIO_PAGEIMG_JPEG && same_file(OUT "/added5", OUT "/scan3.jpg"), "page 5 is the JPEG (%d)", k);
		CHECK(folio_undo(d) == 0 && pages(d) == 3, "undone: 3 pages (%d)", pages(d));
		CHECK(folio_redo(d) == 0 && pages(d) == 5, "redone: 5 pages (%d)", pages(d));
		CHECK(folio_save_pdf(d, OUT "/made.pdf", OUT "/added.pdf") == 0, "saved: %s", folio_error(fc));
		folio_close(d);
	}
	d = folio_open(fc, OUT "/added.pdf", NULL);
	CHECK(d && pages(d) == 5, "kept when saved");
	if (d)
		folio_close(d);
	d = folio_open(fc, OUT "/turned.pdf", NULL);
	CHECK(d && folio_page_rotation(d, L(1)) == 180, "kept when saved");
	if (d)
		folio_close(d);
	d = folio_open(fc, OUT "/book.cbz", NULL);
	if (d) {
		unsigned char one[1] = { 1 };
		CHECK(folio_rotate_pages(d, one, 1, 90) == -1, "a CBZ's pages can't be turned");
		folio_close(d);
	}

	printf("A text page\n");
	snprintf(pbuf[0], sizeof pbuf[0], "%s/doc.pdf", dir);
	d = folio_open(fc, pbuf[0], NULL);
	CHECK(d != NULL, "open doc.pdf: %s", folio_error(fc));
	if (d) {
		k = folio_save_page_image(d, L(0), 72, OUT "/doc1");
		CHECK(k == FOLIO_PAGEIMG_PAGE_PNG, "drawn (%d)", k);
		CHECK(folio_page_size(d, L(0), &pw, &ph) == 0 && is_png(OUT "/doc1", &w, &h) &&
			abs(w - (int)(pw + 0.5f)) <= 1 && abs(h - (int)(ph + 0.5f)) <= 1,
			"72 dpi: the page's size in pixels (%d x %d for %g x %g)", w, h, pw, ph);
		CHECK(folio_parse_pages("1", pages(d), sel) == 1 && folio_save_pages(d, sel, pages(d),
			OUT "/doc-p1.pdf") == 0, "one page of a text PDF: %s", folio_error(fc));
		folio_close(d);
	}
	d = folio_open(fc, OUT "/doc-p1.pdf", NULL);
	CHECK(d != NULL && pages(d) == 1, "it has 1 page");
	if (d)
		folio_close(d);

	folio_drop(fc);
	(void)P;
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
