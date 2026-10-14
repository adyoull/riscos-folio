/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio core: text selection, words, copy, find and links. All points the
 * caller sees are relative to the page bounds' top left. */
#include "folio_int.h"
#include <stdlib.h>
#include <string.h>

fz_stext_page *folio_get_text(folio_doc *doc, folio_cpage *cp)
{
	fz_stext_page *ocr = folio_ocr_text(doc, cp);
	return ocr ? ocr : folio_page_own_text(doc, cp);
}

fz_stext_page *folio_page_own_text(folio_doc *doc, folio_cpage *cp)
{
	if (!cp->text) {
		fz_stext_options opts;
		fz_init_stext_options(doc->ctx, &opts);
		/* Images are kept as blocks: dark mode needs their positions. */
		opts.flags = FZ_STEXT_PRESERVE_IMAGES | FZ_STEXT_PRESERVE_LIGATURES |
			FZ_STEXT_PRESERVE_WHITESPACE | FZ_STEXT_DEHYPHENATE;
		cp->text = fz_new_stext_page_from_display_list(doc->ctx, cp->contents, &opts);
	}
	return cp->text;
}

static fz_point to_page(folio_cpage *cp, folio_point p)
{
	return fz_make_point(p.x + cp->bounds.x0, p.y + cp->bounds.y0);
}

static folio_point from_page(folio_cpage *cp, fz_point p)
{
	folio_point q = { p.x - cp->bounds.x0, p.y - cp->bounds.y0 };
	return q;
}

static folio_quad quad_from_page(folio_cpage *cp, fz_quad q)
{
	folio_quad r;
	r.ul = from_page(cp, q.ul);
	r.ur = from_page(cp, q.ur);
	r.ll = from_page(cp, q.ll);
	r.lr = from_page(cp, q.lr);
	return r;
}

fz_point folio_sel_point(fz_context *ctx, fz_stext_page *st, folio_cpage *cp, folio_point p)
{
	fz_stext_block *b;
	fz_stext_line *l;
	fz_stext_char *ch, *first = NULL, *last = NULL;
	if (p.x != FOLIO_TEXT_START && p.x != FOLIO_TEXT_END)
		return to_page(cp, p);
	for (b = st ? st->first_block : NULL; b; b = b->next) {
		if (b->type != FZ_STEXT_BLOCK_TEXT)
			continue;
		for (l = b->u.t.first_line; l; l = l->next)
			for (ch = l->first_char; ch; ch = ch->next) {
				if (!first)
					first = ch;
				last = ch;
			}
	}
	/* The middle of the first character's leading edge, or of the last's
	 * trailing edge: MuPDF's selection takes the character boundary nearest
	 * a point, so these are exactly before the first and after the last. */
	if (p.x == FOLIO_TEXT_START)
		return first ? fz_make_point((first->quad.ul.x + first->quad.ll.x) / 2,
				(first->quad.ul.y + first->quad.ll.y) / 2) :
			fz_make_point(cp->bounds.x0, cp->bounds.y0);
	return last ? fz_make_point((last->quad.ur.x + last->quad.lr.x) / 2,
			(last->quad.ur.y + last->quad.lr.y) / 2) :
		fz_make_point(cp->bounds.x1, cp->bounds.y1);
}

static int fz_mode(folio_select_mode m)
{
	return m == FOLIO_SELECT_WORDS ? FZ_SELECT_WORDS :
		m == FOLIO_SELECT_LINES ? FZ_SELECT_LINES : FZ_SELECT_CHARS;
}

int folio_snap(folio_doc *doc, folio_loc loc, folio_point *a, folio_point *b,
		folio_select_mode mode)
{
	fz_context *ctx = doc->ctx;
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		fz_point pa = to_page(cp, *a), pb = to_page(cp, *b);
		fz_snap_selection(ctx, folio_get_text(doc, cp), &pa, &pb, fz_mode(mode));
		*a = from_page(cp, pa);
		*b = from_page(cp, pb);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return 0;
}

int folio_selection_quads(folio_doc *doc, folio_loc loc, folio_point a, folio_point b,
		folio_quad *quads, int max)
{
	fz_context *ctx = doc->ctx;
	folio_begin(doc);
	fz_quad *q = NULL;
	int n = 0, i;
	if (max <= 0)
		return 0;
	fz_var(q);
	fz_var(n);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		q = fz_malloc_array(ctx, max, fz_quad);
		fz_stext_page *st = folio_get_text(doc, cp);
		n = fz_highlight_selection(ctx, st, folio_sel_point(ctx, st, cp, a),
			folio_sel_point(ctx, st, cp, b), q, max);
		for (i = 0; i < n; i++)
			quads[i] = quad_from_page(cp, q[i]);
	}
	fz_always(ctx)
		fz_free(ctx, q);
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return n;
}

char *folio_selection_text(folio_doc *doc, folio_loc loc, folio_point a, folio_point b)
{
	fz_context *ctx = doc->ctx;
	folio_begin(doc);
	char *s = NULL;
	fz_var(s);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		fz_stext_page *st = folio_get_text(doc, cp);
		s = fz_copy_selection(ctx, st, folio_sel_point(ctx, st, cp, a),
			folio_sel_point(ctx, st, cp, b), 0);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return NULL;
	}
	return s;
}

void folio_free(folio_doc *doc, void *p)
{
	fz_free(doc->ctx, p);
}

/* Is the UTF-8 character at s (length *len) punctuation or space to trim? */
static int trim_char(const char *s, int *len)
{
	int c;
	*len = fz_chartorune(&c, s);
	if (c < 128)
		return !((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'));
	/* Quotes, dashes, ellipsis, guillemets, inverted marks, no-break space. */
	return c == 0x2018 || c == 0x2019 || c == 0x201c || c == 0x201d || c == 0x2013 ||
		c == 0x2014 || c == 0x2026 || c == 0xab || c == 0xbb || c == 0xa1 ||
		c == 0xbf || c == 0xa0 || c == 0x2039 || c == 0x203a;
}

static char *trim_word(fz_context *ctx, char *s)
{
	char *start = s, *end;
	int len;
	while (*start && trim_char(start, &len))
		start += len;
	end = start + strlen(start);
	while (end > start) {
		/* Step back to the start of the last UTF-8 character. */
		char *p = end - 1;
		while (p > start && (*p & 0xc0) == 0x80)
			p--;
		if (!trim_char(p, &len))
			break;
		end = p;
	}
	memmove(s, start, end - start);
	s[end - start] = 0;
	return s;
}

/* The character whose box contains p, if any. */
static fz_stext_char *char_at(fz_stext_page *st, fz_point p)
{
	fz_stext_block *b;
	fz_stext_line *l;
	fz_stext_char *c;
	for (b = st->first_block; b; b = b->next) {
		if (b->type != FZ_STEXT_BLOCK_TEXT || !fz_is_point_inside_rect(p, fz_expand_rect(b->bbox, 1)))
			continue;
		for (l = b->u.t.first_line; l; l = l->next) {
			if (!fz_is_point_inside_rect(p, fz_expand_rect(l->bbox, 1)))
				continue;
			for (c = l->first_char; c; c = c->next)
				if (fz_is_point_inside_quad(p, c->quad))
					return c;
		}
	}
	return NULL;
}

char *folio_word_at(folio_doc *doc, folio_loc loc, folio_point p,
		folio_point *a, folio_point *b)
{
	fz_context *ctx = doc->ctx;
	folio_begin(doc);
	char *s = NULL;
	fz_var(s);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		fz_stext_page *st = folio_get_text(doc, cp);
		fz_point at = to_page(cp, p), pa, pb;
		fz_stext_char *hit = char_at(st, at);
		/* Only a character under the point counts, and not a space: MuPDF's
		 * snapping alone would pick the nearest word from anywhere. */
		if (!hit || hit->c == ' ' || hit->c == 0xa0)
			break;
		pa = pb = fz_make_point((hit->quad.ul.x + hit->quad.lr.x) / 2, (hit->quad.ul.y + hit->quad.lr.y) / 2);
		fz_snap_selection(ctx, st, &pa, &pb, FZ_SELECT_WORDS);
		s = fz_copy_selection(ctx, st, pa, pb, 0);
		trim_word(ctx, s);
		if (!*s) {
			fz_free(ctx, s);
			s = NULL;
			break;
		}
		if (a)
			*a = from_page(cp, pa);
		if (b)
			*b = from_page(cp, pb);
	}
	fz_catch(ctx) {
		fz_free(ctx, s);
		folio_set_error(doc->fc);
		return NULL;
	}
	return s;
}

int folio_find_on_page(folio_doc *doc, folio_loc loc, const char *needle,
		folio_quad *quads, int *mark, int max)
{
	fz_context *ctx = doc->ctx;
	folio_begin(doc);
	fz_quad *q = NULL;
	int n = 0, i;
	if (max <= 0 || !needle || !*needle)
		return 0;
	fz_var(q);
	fz_var(n);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		q = fz_malloc_array(ctx, max, fz_quad);
		n = fz_search_stext_page(ctx, folio_get_text(doc, cp), needle, mark, q, max);
		if (n > max)
			n = max;    /* it returns the total, even past max */
		/* MuPDF 1.28 numbers the hits (0, 1, 2...); ours marks where each starts. */
		for (i = n - 1; i >= 0; i--)
			mark[i] = i == 0 || mark[i] != mark[i - 1];
		for (i = 0; i < n; i++)
			quads[i] = quad_from_page(cp, q[i]);
	}
	fz_always(ctx)
		fz_free(ctx, q);
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return n;
}

int folio_link_at(folio_doc *doc, folio_loc loc, folio_point p, folio_link *link)
{
	fz_context *ctx = doc->ctx;
	folio_begin(doc);
	int found = 0;
	fz_var(found);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		fz_point at = to_page(cp, p);
		fz_link *l;
		if (!cp->links_loaded) {
			cp->links = fz_load_links(ctx, cp->page);
			cp->links_loaded = 1;
		}
		for (l = cp->links; l; l = l->next) {
			if (!fz_is_point_inside_rect(at, l->rect))
				continue;
			found = 1;
			link->rect = from_fz_rect(fz_translate_rect(l->rect, -cp->bounds.x0, -cp->bounds.y0));
			link->uri = l->uri;
			link->dest.chapter = link->dest.page = -1;
			link->dest_y = -1;
			if (l->uri && !fz_is_external_link(ctx, l->uri)) {
				float x = 0, y = 0;
				fz_location d = fz_resolve_link(ctx, doc->doc, l->uri, &x, &y);
				link->dest.chapter = d.chapter;
				link->dest.page = d.page;
				/* No position (NaN: just the page), or above the page. */
				link->dest_y = y == y ? (y > 0 ? y : 0) : -1;
			}
			break;
		}
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return found;
}

/* ---- Text for reading aloud ------------------------------------------------ */

typedef struct {
	char *text;
	int len, cap;
	int *byte_char;
	folio_quad *quads;
	int nchars, qcap;
	int bad;
} text_buf;

static void tb_put(text_buf *b, const char *s, int n, int ch)
{
	int i;
	if (b->bad)
		return;
	if (b->len + n + 1 > b->cap) {
		int cap = (b->len + n + 1) * 2 + 256;
		char *t = realloc(b->text, cap);
		int *m = realloc(b->byte_char, cap * sizeof *m);
		if (t) b->text = t;
		if (m) b->byte_char = m;
		if (!t || !m) {
			b->bad = 1;
			return;
		}
		b->cap = cap;
	}
	for (i = 0; i < n; i++) {
		b->text[b->len] = s[i];
		b->byte_char[b->len++] = ch;
	}
	b->text[b->len] = 0;
}

static int tb_char(text_buf *b, folio_quad q)
{
	if (b->bad)
		return -1;
	if (b->nchars == b->qcap) {
		int cap = b->qcap ? b->qcap * 2 : 1024;
		folio_quad *n = realloc(b->quads, cap * sizeof *n);
		if (!n) {
			b->bad = 1;
			return -1;
		}
		b->quads = n;
		b->qcap = cap;
	}
	b->quads[b->nchars] = q;
	return b->nchars++;
}

/* Ligatures spelt out, for eSpeak. */
static const char *unligature(int c)
{
	switch (c) {
	case 0xfb00: return "ff";
	case 0xfb01: return "fi";
	case 0xfb02: return "fl";
	case 0xfb03: return "ffi";
	case 0xfb04: return "ffl";
	case 0xfb05: case 0xfb06: return "st";
	}
	return NULL;
}

int folio_get_page_text(folio_doc *doc, folio_loc loc, folio_page_text *t)
{
	fz_context *ctx = doc->ctx;
	text_buf b;
	memset(&b, 0, sizeof b);
	memset(t, 0, sizeof *t);
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		fz_stext_page *sp = folio_get_text(doc, cp);
		fz_stext_block *blk;
		for (blk = sp->first_block; blk; blk = blk->next) {
			fz_stext_line *line;
			int any = 0;
			if (blk->type != FZ_STEXT_BLOCK_TEXT)
				continue;
			for (line = blk->u.t.first_line; line; line = line->next) {
				fz_stext_char *ch;
				int joined = (line->flags & FZ_STEXT_LINE_FLAGS_JOINED) != 0;
				for (ch = line->first_char; ch; ch = ch->next) {
					char u[8];
					const char *lig;
					int n, ci;
					if (joined && !ch->next && fz_is_unicode_hyphen(ch->c))
						continue;          /* "exam-" + "ple" */
					if (ch->c == 0xad)
						continue;          /* soft hyphen */
					ci = tb_char(&b, quad_from_page(cp, ch->quad));
					if ((lig = unligature(ch->c)) != NULL) {
						tb_put(&b, lig, (int)strlen(lig), ci);
					} else {
						n = fz_runetochar(u, ch->c);
						tb_put(&b, u, n, ci);
					}
					any = 1;
				}
				if (!joined)
					tb_put(&b, " ", 1, -1);
			}
			if (any) {
				if (b.len && b.text[b.len - 1] == ' ' && b.byte_char[b.len - 1] == -1)
					b.len--;          /* the last line's space */
				tb_put(&b, "\n\n", 2, -1);
			}
		}
		if (b.bad)
			fz_throw(ctx, FZ_ERROR_SYSTEM, "out of memory");
		if (!b.text)
			tb_put(&b, "", 0, -1);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		free(b.text);
		free(b.byte_char);
		free(b.quads);
		return -1;
	}
	t->text = b.text;
	t->len = b.len;
	t->byte_char = b.byte_char;
	t->quads = b.quads;
	t->nchars = b.nchars;
	return 0;
}

void folio_page_text_free(folio_page_text *t)
{
	free(t->text);
	free(t->byte_char);
	free(t->quads);
	memset(t, 0, sizeof *t);
}
