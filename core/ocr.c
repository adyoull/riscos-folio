/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio core: text recognition (OCR) for scanned pages, with MuPDF's OCR
 * device (Tesseract).
 *
 * The page is drawn in grey at 300 dpi and handed to Tesseract, which gives
 * words with their boxes. A word is kept as its box (in page points) and its
 * UTF-8 text: one line "x0 y0 x1 y1 text" each, which is also how a page is
 * saved (folio_ocr_save). From the words a structured-text page is built the
 * way MuPDF's OCR device does it (Courier glyphs spread evenly across the
 * word's box), and folio_get_text returns that page in place of the page's
 * own text: so selecting, Copy, Find, Define and reading aloud all work on a
 * scan once it is recognised. Drawing still uses the page's own structured
 * text (the pictures, for dark mode). */
#include "folio_int.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OCR_DPI 300.0f
#define OCR_MAX_PX (12 << 20)      /* grey pixels for one page (12 MB) */

struct folio_ocr_store {
	char *words;                   /* "x0 y0 x1 y1 text\n"... (malloc) */
	size_t len;
	fz_stext_page *text;           /* built from words when first wanted */
};

/* ---- Storage ------------------------------------------------------------- */

static int page_key(folio_doc *doc, folio_loc loc)
{
	return doc->reflow ? -1 : folio_page_number(doc, loc);
}

static folio_ocr_store *ocr_slot(folio_doc *doc, int n, int make)
{
	if (n < 0)
		return NULL;
	if (n >= doc->ocr_n) {
		int m = doc->ocr_n ? doc->ocr_n : 16;
		folio_ocr_store *p;
		if (!make)
			return NULL;
		while (m <= n)
			m *= 2;
		p = realloc(doc->ocr, m * sizeof *p);
		if (!p)
			return NULL;
		memset(p + doc->ocr_n, 0, (m - doc->ocr_n) * sizeof *p);
		doc->ocr = p;
		doc->ocr_n = m;
	}
	return &doc->ocr[n];
}

static void ocr_clear(folio_doc *doc, folio_ocr_store *p)
{
	free(p->words);
	fz_drop_stext_page(doc->ctx, p->text);
	memset(p, 0, sizeof *p);
}

void folio_ocr_free(folio_doc *doc)
{
	int i;
	for (i = 0; i < doc->ocr_n; i++)
		ocr_clear(doc, &doc->ocr[i]);
	free(doc->ocr);
	doc->ocr = NULL;
	doc->ocr_n = 0;
	fz_drop_font(doc->ctx, doc->ocr_font);
	doc->ocr_font = NULL;
}

/* ---- Words to structured text -------------------------------------------- */

static int utf8_next(const char **s, const char *end)
{
	int c;
	int n = fz_chartorune(&c, *s);
	if (n < 1 || *s + n > end)
		return -1;
	*s += n;
	return c;
}

/* One word, much as MuPDF's OCR device writes it: the box divided evenly
 * between the characters, each a Courier glyph scaled to its share. Unlike
 * the OCR device the glyphs are upright and sized so that each character's
 * quad is its share of the box exactly (the device's are upside down). */
static void add_word(fz_context *ctx, folio_doc *doc, fz_device *dev, fz_rect box,
		const char *s, const char *end)
{
	int chars[256], n = 0, i, c;
	fz_text *text = NULL;
	float step, x, asc, dsc, size;
	float grey = 0;
	while (s < end && n < 256 && (c = utf8_next(&s, end)) > 0)
		chars[n++] = c;
	if (n == 0)
		return;
	fz_var(text);
	fz_try(ctx) {
		text = fz_new_text(ctx);
		step = (box.x1 - box.x0) / n;
		x = box.x0;
		asc = fz_font_ascender(ctx, doc->ocr_font);
		dsc = fz_font_descender(ctx, doc->ocr_font);
		if (asc - dsc < 0.1f) {
			asc = 0.8f;
			dsc = -0.2f;
		}
		size = (box.y1 - box.y0) / (asc - dsc);
		for (i = 0; i < n; i++) {
			fz_matrix trm;
			/* Courier's characters are 0.6 em wide. y grows down the
			 * page, so d is negative for upright glyphs. */
			trm.a = 10.0f / 6 * step;
			trm.b = 0;
			trm.c = 0;
			trm.d = -size;
			trm.e = x;
			trm.f = box.y0 + asc * size;
			fz_show_glyph(ctx, text, doc->ocr_font, trm,
				fz_encode_character(ctx, doc->ocr_font, chars[i]), chars[i],
				0, 0, FZ_BIDI_LTR, 0);
			x += step;
		}
		fz_fill_text(ctx, dev, text, fz_identity, fz_device_gray(ctx), &grey, 1,
			fz_default_color_params);
	}
	fz_always(ctx)
		fz_drop_text(ctx, text);
	fz_catch(ctx)
		fz_rethrow(ctx);
}

typedef struct {
	fz_rect box;
	const char *s, *end;
} ocr_word;

/* Feeds the word lines to dev. Words on one line of text all get the line's
 * height, so the characters on a line are one size and the structured-text
 * device joins them into lines and paragraphs as it would printed text. */
static void add_words(fz_context *ctx, folio_doc *doc, fz_device *dev, const char *start, const char *end)
{
	ocr_word *w = NULL;
	int n = 0, cap = 0;
	fz_var(w);
	fz_var(n);
	fz_var(cap);
	fz_try(ctx) {
		const char *s = start;
		int i, j, k;
		while (s < end) {
			const char *nl = memchr(s, '\n', end - s);
			const char *eol = nl ? nl : end;
			char num[160];
			size_t len = eol - s < (long)sizeof num - 1 ? (size_t)(eol - s) : sizeof num - 1;
			fz_rect box;
			int used = 0;
			memcpy(num, s, len);
			num[len] = 0;
			if (sscanf(num, "%f %f %f %f %n", &box.x0, &box.y0, &box.x1, &box.y1, &used) == 4 &&
					used > 0 && box.x1 > box.x0 && box.y1 > box.y0) {
				if (n == cap) {
					cap = cap ? cap * 2 : 256;
					w = fz_realloc_array(ctx, w, cap, ocr_word);
				}
				w[n].box = box;
				w[n].s = s + used;
				w[n].end = eol;
				n++;
			}
			s = nl ? nl + 1 : end;
		}
		for (i = 0; i < n; i = j) {
			fz_rect line = w[i].box;
			for (j = i + 1; j < n; j++) {
				fz_rect b = w[j].box;
				float h = line.y1 - line.y0;
				/* Same line: overlapping heights, and further right. */
				if (b.y0 >= line.y1 || b.y1 <= line.y0 || b.x0 < w[j - 1].box.x1 - h / 2)
					break;
				line.y0 = fz_min(line.y0, b.y0);
				line.y1 = fz_max(line.y1, b.y1);
			}
			for (k = i; k < j; k++) {
				fz_rect b = w[k].box;
				b.y0 = line.y0;
				b.y1 = line.y1;
				add_word(ctx, doc, dev, b, w[k].s, w[k].end);
			}
		}
	}
	fz_always(ctx)
		fz_free(ctx, w);
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static fz_stext_page *build_text(folio_doc *doc, folio_ocr_store *p, fz_rect bounds)
{
	fz_context *ctx = doc->ctx;
	fz_stext_page *st = NULL;
	fz_device *dev = NULL;
	fz_stext_options opts;
	fz_var(st);
	fz_var(dev);
	if (!doc->ocr_font)
		doc->ocr_font = fz_new_base14_font(ctx, "Courier");
	fz_init_stext_options(ctx, &opts);
	opts.flags = FZ_STEXT_PRESERVE_LIGATURES | FZ_STEXT_PRESERVE_WHITESPACE | FZ_STEXT_DEHYPHENATE;
	fz_try(ctx) {
		st = fz_new_stext_page(ctx, bounds);
		dev = fz_new_stext_device(ctx, st, &opts);
		add_words(ctx, doc, dev, p->words, p->words + p->len);
		fz_close_device(ctx, dev);
	}
	fz_always(ctx)
		fz_drop_device(ctx, dev);
	fz_catch(ctx) {
		fz_drop_stext_page(ctx, st);
		fz_rethrow(ctx);
	}
	return st;
}

fz_stext_page *folio_ocr_text(folio_doc *doc, folio_cpage *cp)
{
	folio_ocr_store *p = ocr_slot(doc, page_key(doc, cp->loc), 0);
	if (!p || !p->words)
		return NULL;
	if (!p->text)
		p->text = build_text(doc, p, cp->bounds);
	return p->text;
}

/* ---- Recognising --------------------------------------------------------- */

/* Takes MuPDF's OCR device's output: each fill_text is one word, its glyphs
 * spread over the word's box in the rendered image's pixels (see add_word).
 * The box goes back to page points and is written out as a line. */
typedef struct {
	fz_device super;
	fz_buffer *out;
	float scale;                   /* pixels per point */
	fz_point origin;               /* the page's top left, points */
	int chars;
} word_device;

static void word_fill_text(fz_context *ctx, fz_device *dev_, const fz_text *text, fz_matrix ctm,
		fz_colorspace *cs, const float *color, float alpha, fz_color_params cp)
{
	word_device *dev = (word_device *)dev_;
	const fz_text_span *span;
	(void)ctm; (void)cs; (void)color; (void)alpha; (void)cp;
	/* One span per word, every glyph the same size: span->trm holds the
	 * size (a, d), each item its position. */
	for (span = text->head; span; span = span->next) {
		const fz_text_item *a, *z;
		float x0, y0, x1, y1;
		int i;
		if (span->len < 1)
			continue;
		a = &span->items[0];
		z = &span->items[span->len - 1];
		/* Undo add_word's matrix: trm.a = 10/6 of a character's width. */
		x0 = a->x;
		y0 = a->y;
		x1 = z->x + span->trm.a * 0.6f;
		y1 = a->y + span->trm.d * 0.6f;
		if (span->trm.a == 0 || span->trm.d == 0)
			continue;
		fz_append_printf(ctx, dev->out, "%.2f %.2f %.2f %.2f ",
			x0 / dev->scale + dev->origin.x, y0 / dev->scale + dev->origin.y,
			x1 / dev->scale + dev->origin.x, y1 / dev->scale + dev->origin.y);
		for (i = 0; i < span->len; i++) {
			int c = span->items[i].ucs;
			if (c <= 32 || c == 0x7f)
				c = '?';
			fz_append_rune(ctx, dev->out, c);
			dev->chars++;
		}
		fz_append_byte(ctx, dev->out, '\n');
	}
}

static int ocr_progress(fz_context *ctx, void *arg, int percent)
{
	folio_doc *doc = arg;
	(void)ctx; (void)percent;
	return doc->cookie.abort ? 1 : 0;
}

/* A scan: no text of its own (spaces aside), and at least one picture. */
static int is_scan(folio_doc *doc, folio_cpage *cp)
{
	fz_stext_page *st = folio_page_own_text(doc, cp);
	fz_stext_block *b;
	int images = 0;
	for (b = st->first_block; b; b = b->next) {
		if (b->type == FZ_STEXT_BLOCK_IMAGE)
			images++;
		else if (b->type == FZ_STEXT_BLOCK_TEXT) {
			fz_stext_line *l;
			fz_stext_char *ch;
			for (l = b->u.t.first_line; l; l = l->next)
				for (ch = l->first_char; ch; ch = ch->next)
					if (ch->c > 32 && ch->c != 0xa0)
						return 0;
		}
	}
	return images > 0;
}

int folio_ocr_supported(folio_doc *doc)
{
#ifdef FOLIO_NO_OCR
	(void)doc;
	return 0;
#else
	return !doc->reflow;
#endif
}

int folio_page_is_scan(folio_doc *doc, folio_loc loc)
{
	fz_context *ctx = doc->ctx;
	int r = -1;
	fz_var(r);
	folio_begin(doc);
	fz_try(ctx)
		r = is_scan(doc, folio_get_page(doc, loc));
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		r = -1;
	}
	return r;
}

int folio_page_has_ocr(folio_doc *doc, folio_loc loc)
{
	folio_ocr_store *p = ocr_slot(doc, page_key(doc, loc), 0);
	return p && p->words;
}

int folio_ocr_page(folio_doc *doc, folio_loc loc, const char *datadir, const char *lang,
		int only_scans)
{
	fz_context *ctx = doc->ctx;
	fz_device *ocr = NULL;
	word_device *wd = NULL;
	fz_buffer *buf = NULL;
	int result = -1;
	fz_var(ocr);
	fz_var(wd);
	fz_var(buf);
	fz_var(result);
	folio_begin(doc);
	if (!folio_ocr_supported(doc)) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "Text can't be recognised in this document");
		return -1;
	}
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		folio_ocr_store *p;
		fz_rect b = cp->bounds;
		float w = b.x1 - b.x0, h = b.y1 - b.y0;
		float scale = OCR_DPI / 72;
		fz_matrix ctm;
		if (only_scans && !is_scan(doc, cp)) {
			result = FOLIO_OCR_NOT_SCAN;
			break;
		}
		if (w <= 0 || h <= 0)
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "The page is empty");
		/* Big pages: fewer dots, so the image stays within OCR_MAX_PX. */
		if (w * scale * h * scale > OCR_MAX_PX)
			scale = sqrtf(OCR_MAX_PX / (w * h));
		/* The page's top left at the image's (0, 0): Tesseract's boxes are
		 * in the image's pixels. */
		ctm = fz_concat(fz_translate(-b.x0, -b.y0), fz_scale(scale, scale));
		buf = fz_new_buffer(ctx, 4096);
		wd = fz_new_derived_device(ctx, word_device);
		wd->super.fill_text = word_fill_text;
		wd->out = buf;
		wd->scale = scale;
		wd->origin = fz_make_point(b.x0, b.y0);
		ocr = fz_new_ocr_device(ctx, &wd->super, ctm, b, 0, lang, datadir, ocr_progress, doc);
		fz_run_display_list(ctx, cp->contents, ocr, ctm, fz_infinite_rect, &doc->cookie);
		folio_check_abort(doc);
		fz_close_device(ctx, ocr);
		folio_check_abort(doc);
		fz_close_device(ctx, &wd->super);
		p = ocr_slot(doc, page_key(doc, loc), 1);
		if (!p)
			fz_throw(ctx, FZ_ERROR_SYSTEM, "Out of memory");
		ocr_clear(doc, p);
		{
			unsigned char *data;
			size_t n = fz_buffer_storage(ctx, buf, &data);
			p->words = malloc(n + 1);
			if (!p->words)
				fz_throw(ctx, FZ_ERROR_SYSTEM, "Out of memory");
			memcpy(p->words, data, n);
			p->words[n] = 0;
			p->len = n;
		}
		doc->ocr_changed = 1;
		result = wd->chars;
	}
	fz_always(ctx) {
		fz_drop_device(ctx, ocr);
		fz_drop_device(ctx, wd ? &wd->super : NULL);
		fz_drop_buffer(ctx, buf);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		result = -1;
	}
	return result;
}

int folio_ocr_pages(folio_doc *doc)
{
	int i, n = 0;
	for (i = 0; i < doc->ocr_n; i++)
		n += doc->ocr[i].words != NULL;
	return n;
}

int folio_ocr_changed(folio_doc *doc)
{
	return doc->ocr_changed;
}

/* ---- Saving and loading -------------------------------------------------- */
/* "folio-ocr 1", a line naming the document, then for each page
 * "page N L" and L bytes of word lines. */

static void fingerprint(folio_doc *doc, char *out, size_t size)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_specifics(ctx, doc->doc);
	int pages = 0;
	char id[80] = "";
	fz_try(ctx) {
		pages = fz_count_pages(ctx, doc->doc);
		if (pdf) {
			pdf_obj *ids = pdf_dict_get(ctx, pdf_trailer(ctx, pdf), PDF_NAME(ID));
			pdf_obj *first = pdf_array_get(ctx, ids, 0);
			const char *s = pdf_to_str_buf(ctx, first);
			size_t n = pdf_to_str_len(ctx, first), i;
			for (i = 0; i < n && i < 32; i++)
				sprintf(id + 2 * i, "%02x", (unsigned char)s[i]);
		}
	}
	fz_catch(ctx) {
		/* fall through with what we have */
	}
	if (!id[0] && doc->path) {
		FILE *f = fopen(doc->path, "rb");
		long len = -1;
		if (f) {
			fseek(f, 0, SEEK_END);
			len = ftell(f);
			fclose(f);
		}
		snprintf(id, sizeof id, "size%ld", len);
	}
	snprintf(out, size, "%d %s", pages, id);
}

int folio_ocr_save(folio_doc *doc, const char *path)
{
	char fp[128];
	FILE *f;
	int i, ok;
	if (folio_ocr_pages(doc) == 0) {
		remove(path);
		doc->ocr_changed = 0;
		return 0;
	}
	fingerprint(doc, fp, sizeof fp);
	f = fopen(path, "wb");
	if (!f) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "Can't write %s", path);
		return -1;
	}
	fprintf(f, "folio-ocr 1\n%s\n", fp);
	for (i = 0; i < doc->ocr_n; i++)
		if (doc->ocr[i].words) {
			fprintf(f, "page %d %lu\n", i, (unsigned long)doc->ocr[i].len);
			fwrite(doc->ocr[i].words, 1, doc->ocr[i].len, f);
		}
	ok = !ferror(f);
	if (fclose(f) || !ok) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "Can't write %s", path);
		return -1;
	}
	doc->ocr_changed = 0;
	return 0;
}

int folio_ocr_load(folio_doc *doc, const char *path)
{
	char line[200], fp[128];
	FILE *f;
	int pages = 0;
	if (!folio_ocr_supported(doc))
		return 0;
	f = fopen(path, "rb");
	if (!f)
		return -1;
	fingerprint(doc, fp, sizeof fp);
	if (!fgets(line, sizeof line, f) || strcmp(line, "folio-ocr 1\n") != 0 ||
			!fgets(line, sizeof line, f) || strncmp(line, fp, strlen(fp)) != 0 ||
			line[strlen(fp)] != '\n') {
		fclose(f);
		return -1;       /* another document, or changed */
	}
	while (fgets(line, sizeof line, f)) {
		int n;
		unsigned long len;
		folio_ocr_store *p;
		if (sscanf(line, "page %d %lu", &n, &len) != 2 || n < 0 || n > 100000 || len > (64ul << 20))
			break;
		p = ocr_slot(doc, n, 1);
		if (!p)
			break;
		ocr_clear(doc, p);
		p->words = malloc(len + 1);
		if (!p->words)
			break;
		if (fread(p->words, 1, len, f) != len) {
			ocr_clear(doc, p);
			break;
		}
		p->words[len] = 0;
		p->len = len;
		pages++;
	}
	fclose(f);
	doc->ocr_changed = 0;
	return pages;
}
