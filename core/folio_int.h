/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio core internals: shared by doc.c, render.c and text.c. */
#ifndef FOLIO_INT_H
#define FOLIO_INT_H

#include "folio.h"
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>

#define FOLIO_PAGE_CACHE 16

struct folio_ctx {
	fz_context *ctx;
	char err[256];
	int code;                   /* FOLIO_ERR_* for the last folio_open */
};

typedef struct {
	int used;
	folio_loc loc;
	int gen;                    /* layout generation the page belongs to */
	unsigned age;
	fz_page *page;
	fz_rect bounds;
	fz_display_list *contents;  /* page content only */
	fz_display_list *annots;    /* annotations and widgets */
	fz_stext_page *text;        /* lazily made; images preserved */
	fz_link *links;             /* lazily loaded */
	int links_loaded;
} folio_cpage;

struct folio_doc {
	folio_ctx *fc;
	fz_context *ctx;
	fz_document *doc;
	int reflow;
	folio_layout lay;
	int gen;                    /* bumped on every layout change */
	char *css;

	int nchap;
	int *chap_pages;            /* -1 = not counted for this layout */
	int next_count;             /* first chapter not yet counted */

	char *format, *title, *author, *lang;

	folio_outline_item *outline;
	int outline_n;
	char *outline_strings;

	/* Security (PDFs; security.c). */
	char *path;                 /* the file opened, for checking an owner password */
	int encrypted;              /* the file is encrypted */
	int needs_pw;               /* opening it needs a password */
	int owner;                  /* opened with full rights */
	int pw_change;              /* FOLIO_PW_*: applied by the next folio_save_pdf */
	char new_pw[128];           /* UTF-8, for FOLIO_PW_SET */

	fz_cookie cookie;           /* folio_abort() sets cookie.abort */
	folio_cpage cache[FOLIO_PAGE_CACHE];
	unsigned clock;
};

/* Record the caught error's message in fc->err. */
void folio_set_error(folio_ctx *fc);
/* The cached page for loc (loaded on demand), or throws. */
folio_cpage *folio_get_page(folio_doc *doc, folio_loc loc);
/* The structured text of a cached page (made on demand), or throws. */
fz_stext_page *folio_get_text(folio_doc *doc, folio_cpage *cp);
void folio_drop_cpage(folio_doc *doc, folio_cpage *cp);
void folio_forget_pages(folio_doc *doc);
/* PDFs: turn on the undo journal (annot.c). */
void folio_annot_init(folio_doc *doc);
/* Security state after opening (security.c); pw is the password used. */
void folio_security_init(folio_doc *doc, const char *pw);
/* Sets up a full save that applies a password change (security.c). */
void folio_security_write_options(folio_doc *doc, pdf_write_options *opts);
/* Throws if folio_abort() was called during the work just done. */
void folio_check_abort(folio_doc *doc);
/* Start of a call that may load or draw pages: an earlier abort no longer applies. */
static inline void folio_begin(folio_doc *doc) { doc->cookie.abort = 0; }

static inline fz_point to_fz_point(folio_point p) { fz_point q = { p.x, p.y }; return q; }
static inline folio_point from_fz_point(fz_point p) { folio_point q = { p.x, p.y }; return q; }
static inline folio_quad from_fz_quad(fz_quad q)
{
	folio_quad r = { from_fz_point(q.ul), from_fz_point(q.ur), from_fz_point(q.ll), from_fz_point(q.lr) };
	return r;
}
static inline folio_rect from_fz_rect(fz_rect r) { folio_rect q = { r.x0, r.y0, r.x1, r.y1 }; return q; }

#endif
