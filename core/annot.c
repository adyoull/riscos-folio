/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio core: annotations on PDFs, with undo and saving.
 *
 * Highlight, underline and strike-out go on the text between two points
 * (the selection), as PDF text markup annotations with the selection's
 * quads. Notes are PDF Text annotations (the sticky-note icon). Every change
 * is one operation in MuPDF's journal, so undo and redo step through them.
 * A changed page is dropped from the page cache: its annotations are drawn
 * again the next time it's loaded.
 *
 * Coordinates: folio points are from the page's top left (bounds x0, y0);
 * MuPDF's annotation calls take page space, so the bounds are added back. */
#include "folio_int.h"
#include <stdio.h>
#include <string.h>

static pdf_document *pdf_of(folio_doc *doc)
{
	return pdf_specifics(doc->ctx, doc->doc);
}

int folio_can_annotate(folio_doc *doc)
{
	return pdf_of(doc) != NULL;
}

/* Called by folio_open for PDFs. */
void folio_annot_init(folio_doc *doc)
{
	pdf_document *pdf = pdf_of(doc);
	if (!pdf)
		return;
	fz_try(doc->ctx) {
		pdf_enable_journal(doc->ctx, pdf);
	}
	fz_catch(doc->ctx) {
		/* no undo then; annotations still work */
	}
}

static void forget_page(folio_doc *doc, folio_loc loc)
{
	int i;
	for (i = 0; i < FOLIO_PAGE_CACHE; i++) {
		folio_cpage *c = &doc->cache[i];
		if (c->used && c->loc.chapter == loc.chapter && c->loc.page == loc.page)
			folio_drop_cpage(doc, c);
	}
}

static fz_point to_page(folio_cpage *cp, folio_point p)
{
	fz_point q = { p.x + cp->bounds.x0, p.y + cp->bounds.y0 };
	return q;
}

static enum pdf_annot_type pdf_type(folio_annot_kind kind)
{
	switch (kind) {
	case FOLIO_ANNOT_HIGHLIGHT: return PDF_ANNOT_HIGHLIGHT;
	case FOLIO_ANNOT_UNDERLINE: return PDF_ANNOT_UNDERLINE;
	case FOLIO_ANNOT_STRIKEOUT: return PDF_ANNOT_STRIKE_OUT;
	case FOLIO_ANNOT_NOTE: return PDF_ANNOT_TEXT;
	default: return PDF_ANNOT_UNKNOWN;
	}
}

static folio_annot_kind kind_of(enum pdf_annot_type t)
{
	switch (t) {
	case PDF_ANNOT_HIGHLIGHT: return FOLIO_ANNOT_HIGHLIGHT;
	case PDF_ANNOT_UNDERLINE: return FOLIO_ANNOT_UNDERLINE;
	case PDF_ANNOT_STRIKE_OUT: return FOLIO_ANNOT_STRIKEOUT;
	case PDF_ANNOT_TEXT: return FOLIO_ANNOT_NOTE;
	default: return FOLIO_ANNOT_OTHER;
	}
}

static const char *op_name(folio_annot_kind kind)
{
	switch (kind) {
	case FOLIO_ANNOT_HIGHLIGHT: return "Highlight";
	case FOLIO_ANNOT_UNDERLINE: return "Underline";
	case FOLIO_ANNOT_STRIKEOUT: return "Strike out";
	case FOLIO_ANNOT_NOTE: return "Note";
	default: return "Annotation";
	}
}

/* Colours as the common readers use: yellow highlight, blue underline, red
 * strike-out, yellow note; or the one chosen for the kind. */
static void set_colour(folio_doc *doc, pdf_annot *a, folio_annot_kind kind)
{
	static const float yellow[3] = { 1.0f, 0.86f, 0.0f };
	static const float blue[3] = { 0.1f, 0.35f, 0.85f };
	static const float red[3] = { 0.85f, 0.1f, 0.1f };
	const float *c = kind == FOLIO_ANNOT_UNDERLINE ? blue : kind == FOLIO_ANNOT_STRIKEOUT ? red : yellow;
	int rgb = kind >= 0 && kind < 4 ? doc->fc->annot_rgb[kind] : -1;
	float own[3];
	if (rgb >= 0) {
		own[0] = ((rgb >> 16) & 255) / 255.0f;
		own[1] = ((rgb >> 8) & 255) / 255.0f;
		own[2] = (rgb & 255) / 255.0f;
		c = own;
	}
	pdf_set_annot_color(doc->ctx, a, 3, c);
}

void folio_set_annot_colour(folio_ctx *fc, folio_annot_kind kind, int rgb)
{
	if (kind >= 0 && kind < 4)
		fc->annot_rgb[kind] = rgb < 0 ? -1 : rgb & 0xffffff;
}

int folio_annot_markup(folio_doc *doc, folio_loc loc, folio_annot_kind kind,
		folio_point a, folio_point b)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_of(doc);
	pdf_annot *annot = NULL;
	fz_quad q[256];
	int n = 0, op = 0;
	if (!pdf || kind == FOLIO_ANNOT_NOTE || kind == FOLIO_ANNOT_OTHER) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "this document can't be marked up");
		return -1;
	}
	fz_var(annot);
	fz_var(op);
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		fz_stext_page *st = folio_get_text(doc, cp);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		n = fz_highlight_selection(ctx, st, folio_sel_point(ctx, st, cp, a),
			folio_sel_point(ctx, st, cp, b), q, (int)(sizeof q / sizeof q[0]));
		if (n <= 0)
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "no text selected");
		folio_allow(doc, FOLIO_CHANGE_ANNOT, NULL);
		pdf_begin_operation(ctx, pdf, op_name(kind));
		op = 1;
		annot = pdf_create_annot(ctx, page, pdf_type(kind));
		set_colour(doc, annot, kind);
		pdf_set_annot_quad_points(ctx, annot, n, q);
		pdf_update_page(ctx, page);
		pdf_end_operation(ctx, pdf);
		op = 0;
	}
	fz_always(ctx)
		pdf_drop_annot(ctx, annot);
	fz_catch(ctx) {
		if (op)
			pdf_abandon_operation(ctx, pdf);
		folio_set_error(doc->fc);
		forget_page(doc, loc);
		return -1;
	}
	forget_page(doc, loc);
	return 0;
}

int folio_annot_note(folio_doc *doc, folio_loc loc, folio_point at, const char *text)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_of(doc);
	pdf_annot *annot = NULL;
	int op = 0;
	if (!pdf) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "this document can't take notes");
		return -1;
	}
	fz_var(annot);
	fz_var(op);
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		fz_point p = to_page(cp, at);
		folio_allow(doc, FOLIO_CHANGE_ANNOT, NULL);
		pdf_begin_operation(ctx, pdf, "Note");
		op = 1;
		annot = pdf_create_annot(ctx, page, PDF_ANNOT_TEXT);
		pdf_set_annot_rect(ctx, annot, fz_make_rect(p.x, p.y, p.x + 20, p.y + 20));
		pdf_set_annot_icon_name(ctx, annot, "Note");
		set_colour(doc, annot, FOLIO_ANNOT_NOTE);
		pdf_set_annot_contents(ctx, annot, text ? text : "");
		pdf_update_page(ctx, page);
		pdf_end_operation(ctx, pdf);
		op = 0;
	}
	fz_always(ctx)
		pdf_drop_annot(ctx, annot);
	fz_catch(ctx) {
		if (op)
			pdf_abandon_operation(ctx, pdf);
		folio_set_error(doc->fc);
		forget_page(doc, loc);
		return -1;
	}
	forget_page(doc, loc);
	return 0;
}

/* The n-th annotation of the page (borrowed), or NULL. */
static pdf_annot *nth(fz_context *ctx, pdf_page *page, int n)
{
	pdf_annot *a;
	int i = 0;
	for (a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a), i++)
		if (i == n)
			return a;
	return NULL;
}

int folio_annot_at(folio_doc *doc, folio_loc loc, folio_point p, folio_annot_kind *kind,
		folio_rect *rect)
{
	fz_context *ctx = doc->ctx;
	int found = -1;
	if (!pdf_of(doc))
		return -1;
	fz_var(found);
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		fz_point q = to_page(cp, p);
		pdf_annot *a;
		int i = 0;
		/* The last one hit is the one on top. */
		for (a = pdf_first_annot(ctx, page); a; a = pdf_next_annot(ctx, a), i++) {
			enum pdf_annot_type t = pdf_annot_type(ctx, a);
			fz_rect r = pdf_bound_annot(ctx, a);
			int hit = 0;
			if (t == PDF_ANNOT_POPUP || t == PDF_ANNOT_LINK || t == PDF_ANNOT_WIDGET)
				continue;
			if (!fz_is_point_inside_rect(q, r))
				continue;
			if (t == PDF_ANNOT_HIGHLIGHT || t == PDF_ANNOT_UNDERLINE ||
					t == PDF_ANNOT_STRIKE_OUT || t == PDF_ANNOT_SQUIGGLY) {
				int k, nq = pdf_annot_quad_point_count(ctx, a);
				for (k = 0; k < nq && !hit; k++)
					hit = fz_is_point_inside_quad(q, pdf_annot_quad_point(ctx, a, k));
			} else {
				hit = 1;
			}
			if (hit) {
				found = i;
				if (kind)
					*kind = kind_of(t);
				if (rect) {
					rect->x0 = r.x0 - cp->bounds.x0;
					rect->y0 = r.y0 - cp->bounds.y0;
					rect->x1 = r.x1 - cp->bounds.x0;
					rect->y1 = r.y1 - cp->bounds.y0;
				}
			}
		}
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return found;
}

char *folio_annot_text(folio_doc *doc, folio_loc loc, int n)
{
	fz_context *ctx = doc->ctx;
	char *s = NULL;
	if (!pdf_of(doc))
		return NULL;
	fz_var(s);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_annot *a = nth(ctx, pdf_page_from_fz_page(ctx, cp->page), n);
		s = fz_strdup(ctx, a ? pdf_annot_contents(ctx, a) : "");
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return NULL;
	}
	return s;
}

int folio_annot_set_text(folio_doc *doc, folio_loc loc, int n, const char *text)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_of(doc);
	int op = 0;
	if (!pdf)
		return -1;
	fz_var(op);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		pdf_annot *a = nth(ctx, page, n);
		if (!a)
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "no such annotation");
		folio_allow(doc, FOLIO_CHANGE_ANNOT, NULL);
		pdf_begin_operation(ctx, pdf, "Edit note");
		op = 1;
		pdf_set_annot_contents(ctx, a, text ? text : "");
		pdf_update_page(ctx, page);
		pdf_end_operation(ctx, pdf);
		op = 0;
	}
	fz_catch(ctx) {
		if (op)
			pdf_abandon_operation(ctx, pdf);
		folio_set_error(doc->fc);
		forget_page(doc, loc);
		return -1;
	}
	forget_page(doc, loc);
	return 0;
}

int folio_annot_delete(folio_doc *doc, folio_loc loc, int n)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_of(doc);
	int op = 0;
	if (!pdf)
		return -1;
	fz_var(op);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		pdf_annot *a = nth(ctx, page, n);
		if (!a)
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "no such annotation");
		folio_allow(doc, FOLIO_CHANGE_ANNOT, NULL);
		pdf_begin_operation(ctx, pdf, "Delete annotation");
		op = 1;
		pdf_delete_annot(ctx, page, a);
		pdf_update_page(ctx, page);
		pdf_end_operation(ctx, pdf);
		op = 0;
	}
	fz_catch(ctx) {
		if (op)
			pdf_abandon_operation(ctx, pdf);
		folio_set_error(doc->fc);
		forget_page(doc, loc);
		return -1;
	}
	forget_page(doc, loc);
	return 0;
}

int folio_can_undo(folio_doc *doc)
{
	pdf_document *pdf = pdf_of(doc);
	int r = 0;
	if (!pdf)
		return 0;
	/* A signature waiting for the save that makes it can't be undone
	 * (MuPDF would still try to sign it); close without saving instead. */
	fz_try(doc->ctx)
		r = pdf_can_undo(doc->ctx, pdf) && !pdf_has_unsaved_sigs(doc->ctx, pdf);
	fz_catch(doc->ctx)
		r = 0;
	return r;
}

int folio_can_redo(folio_doc *doc)
{
	pdf_document *pdf = pdf_of(doc);
	int r = 0;
	if (!pdf)
		return 0;
	fz_try(doc->ctx)
		r = pdf_can_redo(doc->ctx, pdf);
	fz_catch(doc->ctx)
		r = 0;
	return r;
}

/* Undo or redo: which pages change isn't known, so every page is
 * forgotten (before, so no page MuPDF re-syncs is held, and after). */
static int step(folio_doc *doc, int redo)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_of(doc);
	int *before, nb;
	if (!pdf || !(redo ? folio_can_redo(doc) : folio_can_undo(doc)))
		return -1;
	/* Pages may turn back (or again): their recognised text goes. */
	before = folio_page_keys(doc, &nb);
	folio_forget_pages(doc);
	fz_try(ctx) {
		if (redo)
			pdf_redo(ctx, pdf);
		else
			pdf_undo(ctx, pdf);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		folio_forget_pages(doc);
		free(before);
		return -1;
	}
	folio_recount(doc);         /* pages may have been added or taken away */
	folio_ocr_follow(doc, before, nb);
	free(before);
	return 0;
}

int folio_undo(folio_doc *doc) { return step(doc, 0); }

int folio_change_count(folio_doc *doc)
{
	pdf_document *pdf = pdf_of(doc);
	int pos = 0, steps = 0;
	if (!pdf)
		return 0;
	fz_try(doc->ctx)
		pos = pdf_undoredo_state(doc->ctx, pdf, &steps);
	fz_catch(doc->ctx)
		return 0;
	return pos * 100003 + steps;
}
int folio_redo(folio_doc *doc) { return step(doc, 1); }

int folio_is_modified(folio_doc *doc)
{
	pdf_document *pdf = pdf_of(doc);
	int r = 0;
	if (!pdf)
		return 0;
	fz_try(doc->ctx)
		r = pdf_has_unsaved_changes(doc->ctx, pdf);
	fz_catch(doc->ctx)
		r = 0;
	return r || doc->pw_change != FOLIO_PW_KEEP;
}

/* Copies file from to file to (both closed when this returns). */
static int copy_file(const char *from, const char *to)
{
	FILE *in = fopen(from, "rb"), *out;
	char buf[65536];
	size_t n;
	int bad = 0;
	if (!in)
		return -1;
	out = fopen(to, "wb");
	if (!out) {
		fclose(in);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof buf, in)) > 0)
		if (fwrite(buf, 1, n, out) != n) {
			bad = 1;
			break;
		}
	if (ferror(in))
		bad = 1;
	fclose(in);
	if (fclose(out))
		bad = 1;
	return bad ? -1 : 0;
}

int folio_save_pdf(folio_doc *doc, const char *original, const char *path)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_of(doc);
	pdf_write_options opts;
	if (!pdf) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "only PDFs can be saved");
		return -1;
	}
	opts = pdf_default_write_options;
	fz_try(ctx) {
		if (doc->pw_change != FOLIO_PW_KEEP) {
			/* A password on or off: a full save, encrypted or not. */
			if (folio_is_signed(doc))
				fz_throw(ctx, FZ_ERROR_ARGUMENT, "the PDF is signed: changing its password would break the signature");
			folio_security_write_options(doc, &opts);
		} else {
			/* Incremental (the original bytes, then the changes) keeps
			 * signatures and is quick. MuPDF appends to path, so path starts
			 * as a copy of the original. */
			opts.do_incremental = original && pdf_can_be_saved_incrementally(ctx, pdf);
			if (opts.do_incremental && copy_file(original, path))
				opts.do_incremental = 0;
			/* A signed PDF is only ever added to: a full save would
			 * rewrite what was signed. */
			if (!opts.do_incremental && folio_is_signed(doc))
				fz_throw(ctx, FZ_ERROR_ARGUMENT, "the PDF is signed, and it can't be saved without "
					"breaking the signature (it can only be added to)");
		}
		pdf_save_document(ctx, pdf, path, &opts);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return 0;
}

/* ---- Listing and recolouring --------------------------------------------- */

static int rgb_of(fz_context *ctx, pdf_annot *a)
{
	float c[4];
	int n = 0;
	pdf_annot_color(ctx, a, &n, c);
	if (n == 1)
		c[1] = c[2] = c[0];
	else if (n == 4) {
		float k = c[3];
		c[0] = (1 - c[0]) * (1 - k);
		c[1] = (1 - c[1]) * (1 - k);
		c[2] = (1 - c[2]) * (1 - k);
	} else if (n != 3)
		return -1;
	return ((int)(c[0] * 255 + 0.5f) << 16) | ((int)(c[1] * 255 + 0.5f) << 8) | (int)(c[2] * 255 + 0.5f);
}

/* Copies UTF-8 s into out, spaces for line breaks, cut at a character. */
static void put_text(char *out, size_t size, const char *s)
{
	size_t n = 0;
	for (; s && *s && n + 1 < size; s++) {
		unsigned char c = (unsigned char)*s;
		if ((c & 0xc0) != 0x80 && n + 4 >= size)
			break;
		out[n++] = c < ' ' ? ' ' : (char)c;
	}
	out[n] = 0;
}

int folio_annot_list(folio_doc *doc, folio_loc loc, folio_annot_info *out, int max)
{
	fz_context *ctx = doc->ctx;
	int count = 0;
	if (!pdf_of(doc))
		return 0;
	fz_var(count);
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		pdf_annot *a;
		int i = 0;
		for (a = pdf_first_annot(ctx, page); a && count < max; a = pdf_next_annot(ctx, a), i++) {
			enum pdf_annot_type t = pdf_annot_type(ctx, a);
			folio_annot_info *f = &out[count];
			fz_rect r;
			const char *s;
			if (t == PDF_ANNOT_POPUP || t == PDF_ANNOT_LINK || t == PDF_ANNOT_WIDGET)
				continue;
			memset(f, 0, sizeof *f);
			f->n = i;
			f->kind = kind_of(t);
			snprintf(f->type, sizeof f->type, "%s", pdf_string_from_annot_type(ctx, t));
			r = pdf_bound_annot(ctx, a);
			f->rect.x0 = r.x0 - cp->bounds.x0;
			f->rect.y0 = r.y0 - cp->bounds.y0;
			f->rect.x1 = r.x1 - cp->bounds.x0;
			f->rect.y1 = r.y1 - cp->bounds.y0;
			f->rgb = rgb_of(ctx, a);
			s = pdf_dict_get_text_string(ctx, pdf_annot_obj(ctx, a), PDF_NAME(T));
			put_text(f->author, sizeof f->author, s);
			s = pdf_annot_contents(ctx, a);
			if (s && *s)
				put_text(f->text, sizeof f->text, s);
			else if (t == PDF_ANNOT_HIGHLIGHT || t == PDF_ANNOT_UNDERLINE ||
					t == PDF_ANNOT_STRIKE_OUT || t == PDF_ANNOT_SQUIGGLY) {
				/* The text it marks. */
				fz_stext_page *st = folio_get_text(doc, cp);
				int k, nq = pdf_annot_quad_point_count(ctx, a);
				size_t len = 0;
				for (k = 0; k < nq && len + 2 < sizeof f->text; k++) {
					char *piece = fz_copy_rectangle(ctx, st, fz_rect_from_quad(pdf_annot_quad_point(ctx, a, k)), 0);
					if (piece && *piece) {
						if (len)
							f->text[len++] = ' ';
						put_text(f->text + len, sizeof f->text - len, piece);
						len = strlen(f->text);
					}
					fz_free(ctx, piece);
				}
			}
			count++;
		}
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return count;
}

int folio_annot_recolour(folio_doc *doc, folio_loc loc, int n, int rgb)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_of(doc);
	int op = 0;
	if (!pdf)
		return -1;
	fz_var(op);
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		pdf_annot *a = pdf_first_annot(ctx, page);
		float c[3];
		int i;
		for (i = 0; a && i < n; i++)
			a = pdf_next_annot(ctx, a);
		if (!a)
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "no such annotation");
		c[0] = ((rgb >> 16) & 255) / 255.0f;
		c[1] = ((rgb >> 8) & 255) / 255.0f;
		c[2] = (rgb & 255) / 255.0f;
		folio_allow(doc, FOLIO_CHANGE_ANNOT, NULL);
		pdf_begin_operation(ctx, pdf, "Colour");
		op = 1;
		pdf_set_annot_color(ctx, a, 3, c);
		pdf_update_page(ctx, page);
		pdf_end_operation(ctx, pdf);
		op = 0;
	}
	fz_catch(ctx) {
		if (op)
			pdf_abandon_operation(ctx, pdf);
		folio_set_error(doc->fc);
		forget_page(doc, loc);
		return -1;
	}
	forget_page(doc, loc);
	return 0;
}

/* ---- Measuring ------------------------------------------------------------ */

int folio_page_measure(folio_doc *doc, folio_loc loc, double *per_point, char *unit, size_t unit_size)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_of(doc);
	int own = 0;
	double uu = 1;
	*per_point = 25.4 / 72;
	snprintf(unit, unit_size, "mm");
	if (!pdf)
		return 0;
	fz_var(own);
	fz_try(ctx) {
		pdf_obj *page = pdf_lookup_page_obj(ctx, pdf, folio_page_number(doc, loc));
		pdf_obj *vp = pdf_dict_gets(ctx, page, "VP"), *m, *x;
		if (pdf_is_number(ctx, pdf_dict_get(ctx, page, PDF_NAME(UserUnit))))
			uu = pdf_to_real(ctx, pdf_dict_get(ctx, page, PDF_NAME(UserUnit)));
		if (uu <= 0)
			uu = 1;
		*per_point = 25.4 / 72 * uu;
		/* The drawing's own scale: the first viewport's rectilinear measure,
		 * its X number format's conversion factor and unit. */
		m = pdf_dict_gets(ctx, pdf_array_get(ctx, vp, 0), "Measure");
		x = pdf_array_get(ctx, pdf_dict_gets(ctx, m, "X"), 0);
		if (pdf_name_eq(ctx, pdf_dict_get(ctx, m, PDF_NAME(Subtype)), PDF_NAME(RL)) &&
				pdf_is_number(ctx, pdf_dict_get(ctx, x, PDF_NAME(C)))) {
			const char *u = pdf_dict_get_text_string(ctx, x, PDF_NAME(U));
			double c = pdf_to_real(ctx, pdf_dict_get(ctx, x, PDF_NAME(C)));
			if (c > 0) {
				*per_point = c * uu;
				snprintf(unit, unit_size, "%s", u && *u ? u : "units");
				own = 1;
			}
		}
	}
	fz_catch(ctx) {
		/* paper scale */
	}
	return own;
}

int folio_annot_measure(folio_doc *doc, folio_loc loc, const folio_point *pt, int n, int closed,
		const char *text)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_of(doc);
	int op = 0, i;
	if (!pdf || n < 2) {
		snprintf(doc->fc->err, sizeof doc->fc->err, n < 2 ? "it needs two points or more" :
			"only a PDF can have measurements added");
		return -1;
	}
	fz_var(op);
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		static const float red[3] = { 0.85f, 0.1f, 0.1f };
		pdf_annot *a;
		folio_allow(doc, FOLIO_CHANGE_ANNOT, NULL);
		pdf_begin_operation(ctx, pdf, "Measurement");
		op = 1;
		if (n == 2) {
			a = pdf_create_annot(ctx, page, PDF_ANNOT_LINE);
			pdf_set_annot_line(ctx, a, to_page(cp, pt[0]), to_page(cp, pt[1]));
			pdf_set_annot_line_ending_styles(ctx, a, PDF_ANNOT_LE_BUTT, PDF_ANNOT_LE_BUTT);
		} else {
			a = pdf_create_annot(ctx, page, closed ? PDF_ANNOT_POLYGON : PDF_ANNOT_POLY_LINE);
			for (i = 0; i < n; i++)
				pdf_add_annot_vertex(ctx, a, to_page(cp, pt[i]));
		}
		pdf_set_annot_color(ctx, a, 3, red);
		pdf_set_annot_border_width(ctx, a, 1);
		if (text)
			pdf_set_annot_contents(ctx, a, text);
		pdf_update_page(ctx, page);
		pdf_end_operation(ctx, pdf);
		op = 0;
	}
	fz_catch(ctx) {
		if (op)
			pdf_abandon_operation(ctx, pdf);
		folio_set_error(doc->fc);
		forget_page(doc, loc);
		return -1;
	}
	forget_page(doc, loc);
	return 0;
}

/* ---- Drawing, stamps and pictures --------------------------------------------
 * Freehand drawing is an Ink annotation (its strokes, colour and width);
 * a stamp is a Stamp annotation with one of the standard names, which
 * MuPDF draws (and other readers know); a picture is a Stamp with the
 * picture as its appearance (pdf_set_annot_stamp_image). White can be made
 * see-through, for a scanned signature on paper. Each is one change in the
 * journal, and needs the PDF to allow notes (folio_allow). */

static void rgb_to(int rgb, float c[3])
{
	c[0] = ((rgb >> 16) & 255) / 255.0f;
	c[1] = ((rgb >> 8) & 255) / 255.0f;
	c[2] = (rgb & 255) / 255.0f;
}

int folio_annot_ink(folio_doc *doc, folio_loc loc, const folio_point *pts, const int *counts, int strokes,
		int rgb, float width)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_of(doc);
	pdf_annot *annot = NULL;
	fz_point *v = NULL;
	int op = 0, total = 0, i;
	if (!pdf) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "only a PDF can be drawn on");
		return -1;
	}
	for (i = 0; i < strokes; i++)
		total += counts[i];
	if (strokes <= 0 || total <= 0) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "nothing has been drawn");
		return -1;
	}
	fz_var(annot);
	fz_var(op);
	fz_var(v);
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		float c[3];
		v = fz_malloc_array(ctx, total, fz_point);
		for (i = 0; i < total; i++)
			v[i] = to_page(cp, pts[i]);
		folio_allow(doc, FOLIO_CHANGE_ANNOT, NULL);
		pdf_begin_operation(ctx, pdf, "Drawing");
		op = 1;
		annot = pdf_create_annot(ctx, page, PDF_ANNOT_INK);
		pdf_set_annot_ink_list(ctx, annot, strokes, counts, v);
		rgb_to(rgb, c);
		pdf_set_annot_color(ctx, annot, 3, c);
		pdf_set_annot_border_width(ctx, annot, width > 0 ? width : 1.5f);
		pdf_update_page(ctx, page);
		pdf_end_operation(ctx, pdf);
		op = 0;
	}
	fz_always(ctx) {
		pdf_drop_annot(ctx, annot);
		fz_free(ctx, v);
	}
	fz_catch(ctx) {
		if (op)
			pdf_abandon_operation(ctx, pdf);
		folio_set_error(doc->fc);
		forget_page(doc, loc);
		return -1;
	}
	forget_page(doc, loc);
	return 0;
}

static const char *const stamp_names[] = {
	"Approved", "NotApproved", "Draft", "Final", "Confidential", "ForComment", "ForPublicRelease",
	"NotForPublicRelease", "Experimental", "Expired", "AsIs", "Departmental", "Sold", "TopSecret", NULL
};

const char *folio_stamp_name(int i)
{
	return i >= 0 && i < (int)(sizeof stamp_names / sizeof stamp_names[0]) - 1 ? stamp_names[i] : NULL;
}

int folio_annot_stamp(folio_doc *doc, folio_loc loc, folio_point centre, const char *name)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_of(doc);
	pdf_annot *annot = NULL;
	int op = 0, i, known = 0;
	if (!pdf) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "only a PDF can be stamped");
		return -1;
	}
	for (i = 0; stamp_names[i]; i++)
		if (!strcmp(stamp_names[i], name))
			known = 1;
	if (!known) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "there's no stamp called '%s'", name);
		return -1;
	}
	fz_var(annot);
	fz_var(op);
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		fz_point c = to_page(cp, centre);
		folio_allow(doc, FOLIO_CHANGE_ANNOT, NULL);
		pdf_begin_operation(ctx, pdf, "Stamp");
		op = 1;
		annot = pdf_create_annot(ctx, page, PDF_ANNOT_STAMP);
		pdf_set_annot_icon_name(ctx, annot, name);
		/* Acrobat's stamps are about 2.5 inches by 0.75. */
		pdf_set_annot_rect(ctx, annot, fz_make_rect(c.x - 90, c.y - 27, c.x + 90, c.y + 27));
		pdf_update_page(ctx, page);
		pdf_end_operation(ctx, pdf);
		op = 0;
	}
	fz_always(ctx)
		pdf_drop_annot(ctx, annot);
	fz_catch(ctx) {
		if (op)
			pdf_abandon_operation(ctx, pdf);
		folio_set_error(doc->fc);
		forget_page(doc, loc);
		return -1;
	}
	forget_page(doc, loc);
	return 0;
}

/* The picture with near-white made see-through (an alpha channel). */
fz_image *folio_image_without_white(fz_context *ctx, fz_image *img)
{
	fz_pixmap *pix = NULL, *rgb = NULL, *rgba = NULL;
	fz_image *out = NULL;
	fz_var(out);
	fz_var(pix);
	fz_var(rgb);
	fz_var(rgba);
	fz_try(ctx) {
		int x, y;
		pix = fz_get_pixmap_from_image(ctx, img, NULL, NULL, NULL, NULL);
		rgb = fz_convert_pixmap(ctx, pix, fz_device_rgb(ctx), NULL, NULL, fz_default_color_params, 0);
		rgba = fz_new_pixmap(ctx, fz_device_rgb(ctx), rgb->w, rgb->h, NULL, 1);
		for (y = 0; y < rgb->h; y++) {
			const unsigned char *s = rgb->samples + (size_t)y * rgb->stride;
			unsigned char *d = rgba->samples + (size_t)y * rgba->stride;
			for (x = 0; x < rgb->w; x++, s += rgb->n, d += 4) {
				int lo = s[0] < s[1] ? (s[0] < s[2] ? s[0] : s[2]) : (s[1] < s[2] ? s[1] : s[2]);
				/* Paper (light) goes; ink stays, with soft edges. */
				int a = lo >= 230 ? 0 : lo <= 150 ? 255 : (230 - lo) * 255 / 80;
				/* Premultiplied alpha. */
				d[0] = (unsigned char)(s[0] * a / 255);
				d[1] = (unsigned char)(s[1] * a / 255);
				d[2] = (unsigned char)(s[2] * a / 255);
				d[3] = (unsigned char)a;
			}
		}
		out = fz_new_image_from_pixmap(ctx, rgba, NULL);
	}
	fz_always(ctx) {
		fz_drop_pixmap(ctx, pix);
		fz_drop_pixmap(ctx, rgb);
		fz_drop_pixmap(ctx, rgba);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
	return out;
}

static int picture(folio_doc *doc, folio_loc loc, const folio_point *at, const float *wp, const char *path,
		int clear_white)
{
	const float width = *wp;
	const folio_point centre = *at;
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_of(doc);
	pdf_annot *annot = NULL;
	fz_image *img = NULL, *see = NULL;
	int op = 0;
	if (!pdf) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "only a PDF can take a picture");
		return -1;
	}
	fz_var(annot);
	fz_var(op);
	fz_var(img);
	fz_var(see);
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		fz_point c = to_page(cp, centre);
		float h;
		img = fz_new_image_from_file(ctx, path);
		if (clear_white)
			see = folio_image_without_white(ctx, img);
		h = width * img->h / (img->w ? img->w : 1);
		folio_allow(doc, FOLIO_CHANGE_ANNOT, NULL);
		pdf_begin_operation(ctx, pdf, "Picture");
		op = 1;
		annot = pdf_create_annot(ctx, page, PDF_ANNOT_STAMP);
		pdf_set_annot_rect(ctx, annot, fz_make_rect(c.x - width / 2, c.y - h / 2, c.x + width / 2, c.y + h / 2));
		pdf_set_annot_stamp_image(ctx, annot, see ? see : img);
		pdf_update_page(ctx, page);
		pdf_end_operation(ctx, pdf);
		op = 0;
	}
	fz_always(ctx) {
		pdf_drop_annot(ctx, annot);
		fz_drop_image(ctx, img);
		fz_drop_image(ctx, see);
	}
	fz_catch(ctx) {
		if (op)
			pdf_abandon_operation(ctx, pdf);
		folio_set_error(doc->fc);
		forget_page(doc, loc);
		return -1;
	}
	forget_page(doc, loc);
	return 0;
}

int folio_annot_picture(folio_doc *doc, folio_loc loc, folio_point centre, float wanted, const char *path,
		int clear_white)
{
	float w = wanted > 0 ? wanted : 144;
	return picture(doc, loc, &centre, &w, path, clear_white);
}

/* Strokes (folio points in a box_w x box_h box) drawn into a PNG with a
 * see-through background, at 4 pixels a point: a handwritten signature as
 * a picture (for a digital signature's appearance). 0 or -1. */
int folio_strokes_png(folio_ctx *fc, const folio_point *pts, const int *counts, int strokes, float box_w,
		float box_h, int rgb, float width, const char *path)
{
	fz_context *ctx = fc->ctx;
	fz_pixmap *pix = NULL;
	fz_device *dev = NULL;
	fz_path *p = NULL;
	fz_stroke_state *ss = NULL;
	int rc = 0;
	fz_var(pix);
	fz_var(dev);
	fz_var(p);
	fz_var(ss);
	fz_var(rc);
	fz_try(ctx) {
		const float k = 4;
		fz_irect bb = { 0, 0, (int)(box_w * k + 0.5f), (int)(box_h * k + 0.5f) };
		float c[3];
		int s, j, at = 0;
		if (strokes <= 0 || bb.x1 <= 0 || bb.y1 <= 0)
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "nothing has been drawn");
		pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), bb, NULL, 1);
		fz_clear_pixmap(ctx, pix);
		dev = fz_new_draw_device(ctx, fz_scale(k, k), pix);
		p = fz_new_path(ctx);
		for (s = 0; s < strokes; s++) {
			fz_moveto(ctx, p, pts[at].x, pts[at].y);
			for (j = 1; j < counts[s]; j++)
				fz_lineto(ctx, p, pts[at + j].x, pts[at + j].y);
			if (counts[s] == 1)
				fz_lineto(ctx, p, pts[at].x + 0.1f, pts[at].y);
			at += counts[s];
		}
		ss = fz_new_stroke_state(ctx);
		ss->linewidth = width > 0 ? width : 1.5f;
		ss->start_cap = ss->end_cap = FZ_LINECAP_ROUND;
		ss->linejoin = FZ_LINEJOIN_ROUND;
		rgb_to(rgb, c);
		fz_stroke_path(ctx, dev, p, ss, fz_identity, fz_device_rgb(ctx), c, 1, fz_default_color_params);
		fz_close_device(ctx, dev);
		fz_save_pixmap_as_png(ctx, pix, path);
	}
	fz_always(ctx) {
		fz_drop_stroke_state(ctx, ss);
		fz_drop_path(ctx, p);
		fz_drop_device(ctx, dev);
		fz_drop_pixmap(ctx, pix);
	}
	fz_catch(ctx) {
		folio_set_error(fc);
		rc = -1;
	}
	return rc;
}
