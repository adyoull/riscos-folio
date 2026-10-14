/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio core: fonts from the system, through fontconfig (DESIGN.md §6).
 *
 * MuPDF asks for a font it doesn't have built in:
 * - CJK text in a PDF whose font isn't embedded (the program is built
 *   without MuPDF's own CJK fonts): fontconfig's best font for the language
 *   (ja, zh-cn, zh-tw, ko), serif or sans;
 * - characters of a script none of the document's fonts has (ebooks, and
 *   PDF text MuPDF has to draw itself): MuPDF's built-in Noto font if it has
 *   one for the script, else the system's for it;
 * - a font a PDF names but doesn't embed (beyond the standard 14): the
 *   system's font of that family, if it has it, else MuPDF's substitute.
 * Each font file is opened once and kept.
 *
 * Which fontconfig setup is used (PackMan's, or Folio's own) is the
 * program's choice, made with FONTCONFIG_FILE before the first font is
 * asked for (wimp/fontsetup.c). fontconfig starts on that first request,
 * which may build its cache. Built only with FOLIO_FONTCONFIG. */
#include "folio_int.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>

#ifdef FOLIO_FONTCONFIG
#include <fontconfig/fontconfig.h>
#include <mupdf/ucdn.h>
#ifdef __riscos__
#include <swis.h>
#endif

static FcConfig *config;
static int started;            /* 1: tried; config NULL if it failed */
static char status[200] = "fontconfig not started";

/* Fonts opened, by file and face. */
#define KEEP 24
static struct {
	char file[256];
	int index;
	fz_font *font;
} kept[KEEP];

/* ---- Folio's own fonts ----------------------------------------------------
 * The fonts in FOLIO_APPFONTS (!Folio.Fonts) are opened by MuPDF itself and
 * asked directly whether they have a character, without fontconfig: on the
 * Pi, fontconfig took none of them (test46: "0 of Folio's own"), and a
 * folder's cache never sees fonts added later on RISC OS anyway. */
#define APPFONTS 16
static struct {
	char path[300];
	fz_font *font;
	int failed;
} appf[APPFONTS];
static int appn, app_listed;
static char app_note[120] = "";

static int skip_name(const char *n)
{
	return n[0] == '.' || !strcmp(n, "fonts.conf") || !strcmp(n, "ReadMe") ||
		!strcmp(n, "FontLicence") || !strncmp(n, "fonts/", 6);
}

static void add_app_font(const char *dir, const char *leaf)
{
	if (appn >= APPFONTS || skip_name(leaf))
		return;
	snprintf(appf[appn].path, sizeof appf[appn].path, "%s/%s", dir, leaf);
	appn++;
}

static void list_app_fonts(void)
{
	const char *app = getenv("FOLIO_APPFONTS");
	if (app_listed)
		return;
	app_listed = 1;
	if (!app || !*app)
		return;
#ifdef __riscos__
	{
		/* The folder by its RISC OS name (FOLIO_APPFONTS_RO); the files are
		 * then opened by their UnixLib names, '/' in a leaf being '.'. */
		const char *ro = getenv("FOLIO_APPFONTS_RO");
		char buf[256];
		int next = 0, n;
		while (ro && next != -1) {
			char *p;
			int i;
			n = 0;
			if (_swix(OS_GBPB, _INR(0, 6) | _OUTR(3, 4), 9, ro, buf, 8, next, sizeof buf, "*",
					&n, &next)) {
				snprintf(app_note, sizeof app_note, " (can't read %.80s)", ro);
				break;
			}
			for (p = buf, i = 0; i < n; i++) {
				char leaf[100];
				size_t k;
				snprintf(leaf, sizeof leaf, "%.99s", p);
				for (k = 0; leaf[k]; k++)
					if (leaf[k] == '/')
						leaf[k] = '.';
				add_app_font(app, leaf);
				p += strlen(p) + 1;
			}
		}
	}
#else
	{
		DIR *d = opendir(app);
		struct dirent *e;
		if (!d)
			snprintf(app_note, sizeof app_note, " (can't read %.80s)", app);
		while (d && (e = readdir(d)) != NULL)
			add_app_font(app, e->d_name);
		if (d)
			closedir(d);
	}
#endif
}

/* The first of Folio's own fonts with character c, or NULL. */
static fz_font *app_font_with(fz_context *ctx, int c)
{
	int i;
	list_app_fonts();
	for (i = 0; i < appn; i++) {
		if (!appf[i].font && !appf[i].failed) {
			fz_try(ctx)
				appf[i].font = fz_new_font_from_file(ctx, NULL, appf[i].path, 0, 0);
			fz_catch(ctx) {
				appf[i].failed = 1;
				snprintf(app_note, sizeof app_note, " (couldn't open %.90s)", appf[i].path);
			}
		}
		if (appf[i].font && fz_encode_character(ctx, appf[i].font, c) > 0)
			return fz_keep_font(ctx, appf[i].font);
	}
	return NULL;
}

static FcConfig *fc(void)
{
	if (!started) {
		started = 1;
		list_app_fonts();
		config = FcInitLoadConfigAndFonts();
		if (config) {
			FcFontSet *set = FcConfigGetFonts(config, FcSetSystem);
			snprintf(status, sizeof status, "fontconfig %d.%d.%d: %d fonts; %d of Folio's own%s",
				FC_MAJOR, FC_MINOR, FC_REVISION, set ? set->nfont : 0, appn, app_note);
		} else {
			snprintf(status, sizeof status, "fontconfig couldn't start (no fonts.conf?)");
		}
	}
	return config;
}

const char *folio_fonts_status(void)
{
	return status;
}

static fz_font *new_font(fz_context *ctx, const char *file, int index)
{
	fz_font *font = NULL;
	fz_var(font);
	fz_try(ctx)
		font = fz_new_font_from_file(ctx, NULL, file, index, 0);
	fz_catch(ctx)
		font = NULL;
	return font;
}

static fz_font *open_font(fz_context *ctx, const char *file, int index)
{
	int i, slot = -1;
	fz_font *font;
	for (i = 0; i < KEEP; i++) {
		if (kept[i].font && kept[i].index == index && !strcmp(kept[i].file, file))
			return fz_keep_font(ctx, kept[i].font);
		if (!kept[i].font && slot < 0)
			slot = i;
	}
	font = new_font(ctx, file, index);
	if (!font)
		return NULL;
	if (slot >= 0) {
		snprintf(kept[slot].file, sizeof kept[slot].file, "%s", file);
		kept[slot].index = index;
		kept[slot].font = fz_keep_font(ctx, font);
	}
	return font;
}

/* The best match for pat, if accept() takes it; pat is destroyed. */
static fz_font *match(fz_context *ctx, FcPattern *pat, int (*accept)(FcPattern *m, void *data),
		void *data)
{
	FcConfig *c = fc();
	FcPattern *m;
	FcResult r;
	FcChar8 *file = NULL;
	int index = 0;
	fz_font *font = NULL;
	if (!c || !pat) {
		if (pat)
			FcPatternDestroy(pat);
		return NULL;
	}
	FcConfigSubstitute(c, pat, FcMatchPattern);
	FcDefaultSubstitute(pat);
	m = FcFontMatch(c, pat, &r);
	FcPatternDestroy(pat);
	if (!m)
		return NULL;
	if ((!accept || accept(m, data)) && FcPatternGetString(m, FC_FILE, 0, &file) == FcResultMatch) {
		FcPatternGetInteger(m, FC_INDEX, 0, &index);
		font = open_font(ctx, (const char *)file, index);
	}
	FcPatternDestroy(m);
	return font;
}

/* ---- By language: CJK ---------------------------------------------------- */

static int has_lang(FcPattern *m, void *data)
{
	FcLangSet *ls = NULL;
	if (FcPatternGetLangSet(m, FC_LANG, 0, &ls) != FcResultMatch)
		return 0;
	return FcLangSetHasLang(ls, (const FcChar8 *)data) != FcLangDifferentLang;
}

static fz_font *by_lang(fz_context *ctx, const char *lang, int serif)
{
	FcPattern *p = FcPatternCreate();
	if (!p)
		return NULL;
	FcPatternAddString(p, FC_LANG, (const FcChar8 *)lang);
	FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)(serif ? "serif" : "sans-serif"));
	return match(ctx, p, has_lang, (void *)lang);
}

/* A character each CJK language's fonts have and the others' may not. */
static int lang_sample(const char *lang)
{
	return !strcmp(lang, "ja") ? 0x3042 : !strcmp(lang, "ko") ? 0xac00 :
		!strcmp(lang, "zh-cn") ? 0x8fd9 : 0x9019;
}

static fz_font *load_cjk(fz_context *ctx, const char *name, int ordering, int serif)
{
	const char *lang = ordering == FZ_ADOBE_JAPAN ? "ja" : ordering == FZ_ADOBE_KOREA ? "ko" :
		ordering == FZ_ADOBE_GB ? "zh-cn" : "zh-tw";
	fz_font *f = by_lang(ctx, lang, serif);
	(void)name;
	if (!f)
		f = app_font_with(ctx, lang_sample(lang));
	return f;
}

/* ---- By script: fallback ------------------------------------------------- */

/* A character of each script, to find a font that has it. */
static int sample(int script)
{
	switch (script) {
	case UCDN_SCRIPT_GREEK: return 0x3b1;
	case UCDN_SCRIPT_CYRILLIC: return 0x431;
	case UCDN_SCRIPT_ARMENIAN: return 0x561;
	case UCDN_SCRIPT_HEBREW: return 0x5d0;
	case UCDN_SCRIPT_ARABIC: return 0x627;
	case UCDN_SCRIPT_SYRIAC: return 0x710;
	case UCDN_SCRIPT_THAANA: return 0x780;
	case UCDN_SCRIPT_DEVANAGARI: return 0x915;
	case UCDN_SCRIPT_BENGALI: return 0x995;
	case UCDN_SCRIPT_GURMUKHI: return 0xa15;
	case UCDN_SCRIPT_GUJARATI: return 0xa95;
	case UCDN_SCRIPT_ORIYA: return 0xb15;
	case UCDN_SCRIPT_TAMIL: return 0xb95;
	case UCDN_SCRIPT_TELUGU: return 0xc15;
	case UCDN_SCRIPT_KANNADA: return 0xc95;
	case UCDN_SCRIPT_MALAYALAM: return 0xd15;
	case UCDN_SCRIPT_SINHALA: return 0xd9a;
	case UCDN_SCRIPT_THAI: return 0xe01;
	case UCDN_SCRIPT_LAO: return 0xe81;
	case UCDN_SCRIPT_TIBETAN: return 0xf40;
	case UCDN_SCRIPT_MYANMAR: return 0x1000;
	case UCDN_SCRIPT_GEORGIAN: return 0x10d0;
	case UCDN_SCRIPT_ETHIOPIC: return 0x1200;
	case UCDN_SCRIPT_CHEROKEE: return 0x13a0;
	case UCDN_SCRIPT_KHMER: return 0x1780;
	case UCDN_SCRIPT_MONGOLIAN: return 0x1820;
	case UCDN_SCRIPT_HANGUL: return 0xac00;
	case UCDN_SCRIPT_HIRAGANA: return 0x3042;
	case UCDN_SCRIPT_KATAKANA: return 0x30a2;
	case UCDN_SCRIPT_BOPOMOFO: return 0x3105;
	case UCDN_SCRIPT_HAN: return 0x4e00;
	case UCDN_SCRIPT_YI: return 0xa000;
	default: return 0;
	}
}

static int has_char(FcPattern *m, void *data)
{
	FcCharSet *cs = NULL;
	if (FcPatternGetCharSet(m, FC_CHARSET, 0, &cs) != FcResultMatch)
		return 0;
	return FcCharSetHasChar(cs, (FcChar32)(size_t)data);
}

static fz_font *load_fallback(fz_context *ctx, int script, int language, int serif, int bold, int italic)
{
	int size = 0, subfont = 0, ch;
	FcPattern *p;
	FcCharSet *cs;
	(void)bold;
	(void)italic;
	/* MuPDF's own Noto fonts first, where it has one. */
	if (fz_lookup_noto_font(ctx, script, language, &size, &subfont))
		return NULL;
	if (script == UCDN_SCRIPT_HAN) {
		switch (language) {
		case FZ_LANG_ja: return load_cjk(ctx, NULL, FZ_ADOBE_JAPAN, serif);
		case FZ_LANG_ko: return load_cjk(ctx, NULL, FZ_ADOBE_KOREA, serif);
		case FZ_LANG_zh_Hant: return load_cjk(ctx, NULL, FZ_ADOBE_CNS, serif);
		case FZ_LANG_zh_Hans: return load_cjk(ctx, NULL, FZ_ADOBE_GB, serif);
		}
	}
	ch = sample(script);
	if (!ch)
		return NULL;
	p = FcPatternCreate();
	cs = FcCharSetCreate();
	if (!p || !cs) {
		if (p)
			FcPatternDestroy(p);
		if (cs)
			FcCharSetDestroy(cs);
		return NULL;
	}
	FcCharSetAddChar(cs, (FcChar32)ch);
	FcPatternAddCharSet(p, FC_CHARSET, cs);
	FcCharSetDestroy(cs);
	FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)(serif ? "serif" : "sans-serif"));
	{
		fz_font *f = match(ctx, p, has_char, (void *)(size_t)ch);
		return f ? f : app_font_with(ctx, ch);
	}
}

/* ---- By name ------------------------------------------------------------- */

/* "Arial,BoldItalic", "TimesNewRomanPS-BoldMT", "ABCDEF+Garamond": the
 * family part, letters and digits only, lower case. */
static void family_key(const char *name, char *out, size_t size)
{
	const char *plus = strchr(name, '+');
	size_t n = 0;
	if (plus && plus - name == 6)
		name = plus + 1;
	for (; *name && *name != ',' && *name != '-' && n + 1 < size; name++)
		if (isalnum((unsigned char)*name))
			out[n++] = (char)tolower((unsigned char)*name);
	out[n] = 0;
	/* PostScript names add "PS" or "MT" to the family. */
	if (n > 2 && (!strcmp(out + n - 2, "ps") || !strcmp(out + n - 2, "mt")))
		out[n - 2] = 0;
}

static int same_family(FcPattern *m, void *data)
{
	FcChar8 *fam;
	int i;
	for (i = 0; FcPatternGetString(m, FC_FAMILY, i, &fam) == FcResultMatch; i++) {
		char key[64];
		family_key((const char *)fam, key, sizeof key);
		if (!strcmp(key, (const char *)data))
			return 1;
	}
	return 0;
}

static fz_font *load_named(fz_context *ctx, const char *name, int bold, int italic, int exact)
{
	char key[64], fam[96];
	const char *plus = strchr(name, '+');
	size_t i, n = 0;
	FcPattern *p;
	(void)exact;
	family_key(name, key, sizeof key);
	if (strlen(key) < 3)
		return NULL;
	if (plus && plus - name == 6)
		name = plus + 1;
	/* The family as written, for fontconfig: up to ',' or '-', with spaces
	 * before capitals ("TimesNewRoman" -> "Times New Roman"). */
	for (i = 0; name[i] && name[i] != ',' && name[i] != '-' && n + 2 < sizeof fam; i++) {
		if (i && isupper((unsigned char)name[i]) && islower((unsigned char)name[i - 1]))
			fam[n++] = ' ';
		fam[n++] = name[i];
	}
	fam[n] = 0;
	p = FcPatternCreate();
	if (!p)
		return NULL;
	FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)fam);
	FcPatternAddInteger(p, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
	FcPatternAddInteger(p, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
	return match(ctx, p, same_family, key);
}

void folio_fonts_install(fz_context *ctx)
{
	fz_install_load_system_font_funcs(ctx, load_named, load_cjk, load_fallback);
}

void folio_fonts_drop(fz_context *ctx)
{
	int i;
	for (i = 0; i < KEEP; i++)
		if (kept[i].font) {
			fz_drop_font(ctx, kept[i].font);
			kept[i].font = NULL;
		}
	for (i = 0; i < appn; i++)
		if (appf[i].font) {
			fz_drop_font(ctx, appf[i].font);
			appf[i].font = NULL;
		}
	appn = 0;
	app_listed = 0;
	if (config) {
		FcConfigDestroy(config);
		config = NULL;
	}
	started = 0;
}

#else

const char *folio_fonts_status(void) { return "no fontconfig in this build"; }
void folio_fonts_install(fz_context *ctx) { (void)ctx; }
void folio_fonts_drop(fz_context *ctx) { (void)ctx; }

#endif
