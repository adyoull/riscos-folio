/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Folio document info tests (core/info.c), run under qemu by `make test`:
 *   info_test tests/data
 * Writes a PDF by hand with one of each kind of font: a standard font not
 * embedded, a TrueType subset (twice, under two subset prefixes), a CID
 * TrueType subset, a Type 3 font whose glyphs use a font of their own, a
 * font inside a form inside a form (which also holds itself), a note's
 * appearance and the form's default font; one picture used twice; three
 * bookmarks and two layers. Then checks the info and the fonts list. */
#include "folio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

#define OUT "tests/out/info"

static const char *obj[] = {
	/* 1 */ "<< /Type /Catalog /Pages 2 0 R /Outlines 20 0 R /AcroForm << /Fields [] /DR << /Font << /ZaDb 19 0 R >> >> >>"
		" /OCProperties << /OCGs [24 0 R 25 0 R] /D << /Order [24 0 R 25 0 R] >> >> >>",
	/* 2 */ "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 /Resources 5 0 R >>",
	/* 3 */ "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 16 0 R >>",
	/* 4 */ "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Contents 16 0 R"
		" /Resources << /Font << /F2 13 0 R >> /XObject << /Im1 12 0 R >> >> /Annots [17 0 R] >>",
	/* 5 */ "<< /Font << /F1 6 0 R /F2 7 0 R /F3 8 0 R >> /XObject << /Fm1 10 0 R /Im1 12 0 R >> >>",
	/* 6 */ "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
	/* 7 */ "<< /Type /Font /Subtype /TrueType /BaseFont /ABCDEF+Arial-BoldMT /FontDescriptor 9 0 R >>",
	/* 8 */ "<< /Type /Font /Subtype /Type0 /BaseFont /GHIJKL+MSGothic /Encoding /Identity-H"
		" /DescendantFonts [<< /Type /Font /Subtype /CIDFontType2 /BaseFont /GHIJKL+MSGothic /FontDescriptor 9 0 R >>] >>",
	/* 9 */ "<< /Type /FontDescriptor /FontName /X /FontFile2 15 0 R >>",
	/* 10 */ "STREAM << /Type /XObject /Subtype /Form /BBox [0 0 10 10]"
		" /Resources << /XObject << /Fm1 10 0 R /Fm2 11 0 R >> /Font << /T 14 0 R >> >> >>",
	/* 11 */ "STREAM << /Type /XObject /Subtype /Form /BBox [0 0 10 10] /Resources << /Font << /F 18 0 R >> >> >>",
	/* 12 */ "STREAM << /Type /XObject /Subtype /Image /Width 1 /Height 1 /ColorSpace /DeviceGray /BitsPerComponent 8 >>",
	/* 13 */ "<< /Type /Font /Subtype /TrueType /BaseFont /BCDEFG+Arial-BoldMT /FontDescriptor 9 0 R >>",
	/* 14 */ "<< /Type /Font /Subtype /Type3 /Name /Stamp /FontBBox [0 0 1 1] /FontMatrix [1 0 0 1 0 0]"
		" /CharProcs << >> /Encoding << /Differences [] >> /FirstChar 0 /LastChar 0 /Widths [0]"
		" /Resources << /Font << /G 23 0 R >> >> >>",
	/* 15 */ "STREAM << >>",
	/* 16 */ "STREAM << >>",
	/* 17 */ "<< /Type /Annot /Subtype /FreeText /Rect [0 0 10 10] /AP << /N 22 0 R >> >>",
	/* 18 */ "<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman >>",
	/* 19 */ "<< /Type /Font /Subtype /Type1 /BaseFont /ZapfDingbats >>",
	/* 20 */ "<< /Type /Outlines /First 21 0 R /Last 26 0 R /Count 3 >>",
	/* 21 */ "<< /Title (One) /Parent 20 0 R /Next 26 0 R /First 27 0 R /Last 27 0 R >>",
	/* 22 */ "STREAM << /Type /XObject /Subtype /Form /BBox [0 0 10 10] /Resources << /Font << /C 28 0 R >> >> >>",
	/* 23 */ "<< /Type /Font /Subtype /Type1 /BaseFont /Symbol >>",
	/* 24 */ "<< /Type /OCG /Name (Back) >>",
	/* 25 */ "<< /Type /OCG /Name (Front) >>",
	/* 26 */ "<< /Title (Two) /Parent 20 0 R /Prev 21 0 R >>",
	/* 27 */ "<< /Title (One.One) /Parent 21 0 R >>",
	/* 28 */ "<< /Type /Font /Subtype /Type1 /BaseFont /Courier /FontDescriptor << /FontFile3 15 0 R >> >>",
};

static void write_pdf(const char *path)
{
	int n = (int)(sizeof obj / sizeof obj[0]), i;
	long off[64], xref;
	FILE *f = fopen(path, "wb");
	fprintf(f, "%%PDF-1.7\n");
	for (i = 0; i < n; i++) {
		off[i] = ftell(f);
		if (!strncmp(obj[i], "STREAM ", 7)) {
			const char *d = obj[i] + 7;
			fprintf(f, "%d 0 obj\n%.*s /Length 0 >>\nstream\n\nendstream\nendobj\n", i + 1,
				(int)strlen(d) - 2, d);
		} else
			fprintf(f, "%d 0 obj\n%s\nendobj\n", i + 1, obj[i]);
	}
	xref = ftell(f);
	fprintf(f, "xref\n0 %d\n0000000000 65535 f \n", n + 1);
	for (i = 0; i < n; i++)
		fprintf(f, "%010ld 00000 n \n", off[i]);
	fprintf(f, "trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%ld\n%%%%EOF\n", n + 1, xref);
	fclose(f);
}

static const char *info(folio_info *in, const char *label)
{
	int i;
	for (i = 0; i < in->n; i++)
		if (!strcmp(in->item[i].label, label))
			return in->item[i].value;
	return "";
}

static const folio_font *font(folio_fonts *f, const char *name)
{
	int i;
	for (i = 0; i < f->n; i++)
		if (!strcmp(f->font[i].name, name))
			return &f->font[i];
	return NULL;
}

int main(int argc, char **argv)
{
	folio_ctx *core;
	folio_doc *doc;
	folio_info in;
	folio_fonts *f = malloc(sizeof *f);
	const folio_font *x;
	char path[256];
	int i;
	/* info_test <data> <a.pdf>: just list a PDF's fonts. */
	if (argc > 2) {
		core = folio_new(0);
		doc = folio_open(core, argv[2], NULL);
		if (!doc || folio_get_fonts(doc, f, 0) < 0)
			return 1;
		printf("%d pictures, %d of %d pages\n", f->pictures, f->pages_done, f->pages);
		for (i = 0; i < f->n; i++)
			printf("%s (%s) %d\n", f->font[i].name, f->font[i].type, f->font[i].embedded);
		return 0;
	}
	mkdir("tests/out", 0777);
	mkdir(OUT, 0777);
	snprintf(path, sizeof path, OUT "/fonts.pdf");
	write_pdf(path);
	core = folio_new(0);
	doc = folio_open(core, path, NULL);
	CHECK(doc != NULL, "open %s", path);
	if (!doc)
		return 1;

	folio_get_info(doc, &in);
	CHECK(!strcmp(info(&in, "Bookmarks"), "3"), "bookmarks '%s'", info(&in, "Bookmarks"));
	CHECK(!strcmp(info(&in, "Layers"), "2"), "layers '%s'", info(&in, "Layers"));

	CHECK(folio_get_fonts(doc, f, 0) == 8, "fonts %d", f->n);
	for (i = 0; i < f->n; i++)
		printf("  font %s (%s) %d\n", f->font[i].name, f->font[i].type, f->font[i].embedded);
	CHECK(f->pictures == 1, "pictures %d", f->pictures);
	CHECK(f->pages == 2 && f->pages_done == 2, "pages %d/%d", f->pages_done, f->pages);
	CHECK(f->more == 0, "more %d", f->more);
	CHECK(f->n >= 1 && !strcmp(f->font[0].name, "Arial-BoldMT"), "sorted, first '%s'", f->n ? f->font[0].name : "");
	x = font(f, "Arial-BoldMT");
	CHECK(x && !strcmp(x->type, "TrueType") && x->embedded == 2, "Arial subset, once");
	x = font(f, "Helvetica");
	CHECK(x && !strcmp(x->type, "Type 1") && x->embedded == 0, "Helvetica not embedded");
	x = font(f, "MSGothic");
	CHECK(x && !strcmp(x->type, "TrueType (CID)") && x->embedded == 2, "CID TrueType subset");
	x = font(f, "Stamp");
	CHECK(x && !strcmp(x->type, "Type 3") && x->embedded == 3, "Type 3 by /Name");
	CHECK(font(f, "Symbol") != NULL, "a Type 3 glyph's own font");
	CHECK(font(f, "Times-Roman") != NULL, "font in a form in a form");
	x = font(f, "Courier");
	CHECK(x && x->embedded == 1 && !strcmp(x->type, "Type 1 (CFF)"), "a note's appearance, embedded (FontFile3)");
	CHECK(font(f, "ZapfDingbats") != NULL, "the form's default font");

	/* Not a PDF: no list. */
	{
		folio_doc *epub;
		snprintf(path, sizeof path, "%s/book.epub", argc > 1 ? argv[1] : "tests/data");
		epub = folio_open(core, path, NULL);
		if (epub) {
			CHECK(folio_get_fonts(epub, f, 0) == -1, "epub: not a PDF");
			folio_close(epub);
		}
	}
	folio_close(doc);
	folio_drop(core);
	free(f);
	printf("info_test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
