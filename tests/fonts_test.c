/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Folio system font tests (core/fonts.c, fontconfig), run under qemu by
 * `make test`:
 *   fonts_test tests/data
 * fontconfig reads tests/out/fonts/fonts.conf, written here, which lists
 * tests/data/fonts (DroidSansFallback.ttf, from MuPDF's sources) and keeps
 * its cache in tests/out/fonts/cache. Then: CJK fonts by language, a
 * fallback by script, a font by name, and a PDF whose Japanese font isn't
 * embedded drawn with the system's. */
#include "folio.h"
#include "folio_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <mupdf/ucdn.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static int is_droid(fz_context *ctx, fz_font *f)
{
	return f && strstr(fz_font_name(ctx, f), "Droid") != NULL;
}

/* Dark pixels in the page drawn at 72 dpi. */
static int ink(fz_context *ctx, fz_document *doc)
{
	fz_pixmap *pix = NULL;
	int n = 0, i;
	fz_var(pix);
	fz_var(n);
	fz_try(ctx) {
		pix = fz_new_pixmap_from_page_number(ctx, doc, 0, fz_identity, fz_device_gray(ctx), 0);
		for (i = 0; i < pix->w * pix->h; i++)
			if (pix->samples[i] < 128)
				n++;
	}
	fz_always(ctx)
		fz_drop_pixmap(ctx, pix);
	fz_catch(ctx)
		n = -1;
	return n;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/data";
	char cwd[400], conf[600], fontdir[600];
	FILE *f;
	fz_context *ctx;
	fz_font *font;
	fz_document *doc;

	if (!getcwd(cwd, sizeof cwd))
		return 1;
	mkdir("tests/out", 0777);
	mkdir("tests/out/fonts", 0777);
	snprintf(fontdir, sizeof fontdir, "%s/%s/fonts", cwd, dir);
	snprintf(conf, sizeof conf, "%s/tests/out/fonts/fonts.conf", cwd);
	f = fopen(conf, "w");
	if (!f)
		return 1;
	fprintf(f, "<?xml version=\"1.0\"?>\n<!DOCTYPE fontconfig SYSTEM \"fonts.dtd\">\n<fontconfig>\n"
		"  <dir>%s</dir>\n  <cachedir>%s/tests/out/fonts/cache</cachedir>\n</fontconfig>\n",
		fontdir, cwd);
	fclose(f);
	setenv("FONTCONFIG_FILE", conf, 1);
	unsetenv("FONTCONFIG_PATH");

	ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
	if (!ctx)
		return 1;
	fz_register_document_handlers(ctx);
	folio_fonts_install(ctx);

	printf("CJK\n");
	font = fz_load_system_cjk_font(ctx, "MS-Mincho", FZ_ADOBE_JAPAN, 1);
	printf("  %s\n", folio_fonts_status());
	CHECK(strstr(folio_fonts_status(), "fontconfig 2.14.1: 1 fonts") != NULL || strstr(folio_fonts_status(), ": 2 fonts"),
		"status '%s'", folio_fonts_status());
	CHECK(is_droid(ctx, font), "Japanese: %s", font ? fz_font_name(ctx, font) : "none");
	fz_drop_font(ctx, font);
	font = fz_load_system_cjk_font(ctx, "Batang", FZ_ADOBE_KOREA, 0);
	CHECK(is_droid(ctx, font), "Korean: %s", font ? fz_font_name(ctx, font) : "none");
	fz_drop_font(ctx, font);

	printf("Fallback\n");
	/* (fz_load_fallback_font keeps what it finds: no drop.) */
	font = fz_load_fallback_font(ctx, UCDN_SCRIPT_HIRAGANA, 0, 0, 0, 0);
	CHECK(is_droid(ctx, font), "hiragana: %s", font ? fz_font_name(ctx, font) : "none");
	font = fz_load_fallback_font(ctx, UCDN_SCRIPT_GREEK, 0, 0, 0, 0);
	CHECK(font && !is_droid(ctx, font), "Greek: MuPDF's own (%s)", font ? fz_font_name(ctx, font) : "none");

	printf("By name\n");
	font = fz_load_system_font(ctx, "DroidSansFallback", 0, 0, 0);
	CHECK(is_droid(ctx, font), "by name: %s", font ? fz_font_name(ctx, font) : "none");
	fz_drop_font(ctx, font);
	font = fz_load_system_font(ctx, "NoSuchFontAtAll-Bold", 1, 0, 0);
	CHECK(font == NULL, "an unknown name gives none (%s)", font ? fz_font_name(ctx, font) : "");
	fz_drop_font(ctx, font);

	printf("PDF\n");
	{
		char path[600];
		snprintf(path, sizeof path, "%s/cjk.pdf", dir);
		doc = NULL;
		fz_try(ctx)
			doc = fz_open_document(ctx, path);
		fz_catch(ctx)
			doc = NULL;
		CHECK(doc != NULL, "open cjk.pdf");
		if (doc) {
			int n = ink(ctx, doc);
			CHECK(n > 200, "Japanese text drawn with the system's font (%d dark pixels)", n);
			fz_drop_document(ctx, doc);
		}
	}
	folio_fonts_drop(ctx);
	fz_drop_context(ctx);
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
