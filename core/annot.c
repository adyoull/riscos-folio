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
 * strike-out, yellow note. */
static void set_colour(fz_context *ctx, pdf_annot *a, folio_annot_kind kind)
{
	static const float yellow[3] = { 1.0f, 0.86f, 0.0f };
	static const float blue[3] = { 0.1f, 0.35f, 0.85f };
	static const float red[3] = { 0.85f, 0.1f, 0.1f };
	const float *c = kind == FOLIO_ANNOT_UNDERLINE ? blue : kind == FOLIO_ANNOT_STRIKEOUT ? red : yellow;
	pdf_set_annot_color(ctx, a, 3, c);
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
		pdf_begin_operation(ctx, pdf, op_name(kind));
		op = 1;
		annot = pdf_create_annot(ctx, page, pdf_type(kind));
		set_colour(ctx, annot, kind);
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
		pdf_begin_operation(ctx, pdf, "Note");
		op = 1;
		annot = pdf_create_annot(ctx, page, PDF_ANNOT_TEXT);
		pdf_set_annot_rect(ctx, annot, fz_make_rect(p.x, p.y, p.x + 20, p.y + 20));
		pdf_set_annot_icon_name(ctx, annot, "Note");
		set_colour(ctx, annot, FOLIO_ANNOT_NOTE);
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
	fz_try(doc->ctx)
		r = pdf_can_undo(doc->ctx, pdf);
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
	if (!pdf || !(redo ? folio_can_redo(doc) : folio_can_undo(doc)))
		return -1;
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
		return -1;
	}
	folio_forget_pages(doc);
	return 0;
}

int folio_undo(folio_doc *doc) { return step(doc, 0); }
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
			folio_security_write_options(doc, &opts);
		} else {
			/* Incremental (the original bytes, then the changes) keeps
			 * signatures and is quick. MuPDF appends to path, so path starts
			 * as a copy of the original. */
			opts.do_incremental = original && pdf_can_be_saved_incrementally(ctx, pdf);
			if (opts.do_incremental && copy_file(original, path))
				opts.do_incremental = 0;
		}
		pdf_save_document(ctx, pdf, path, &opts);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return 0;
}
