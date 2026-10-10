/* Folio core: MuPDF wrapped for the reader. No Wimp code here, so all of it
 * runs (and is tested) on ARM Linux under qemu-arm-aligntrap as well as on
 * RISC OS.
 *
 * Conventions
 *  - Every call that can fail returns 0 for success and -1 for failure;
 *    folio_error() then gives the message. Calls returning pointers return
 *    NULL on failure.
 *  - Page coordinates are points (1/72 inch), y down, with 0,0 at the top
 *    left of the page's bounds. Pixel coordinates are the same scaled by the
 *    zoom (pixels per point).
 *  - A page is named by a folio_loc: chapter and page within the chapter.
 *    Fixed-layout documents (PDF, XPS, CBZ) have one chapter. Re-flowed
 *    books (EPUB, FB2, MOBI, HTML) have one per spine item, and their
 *    page numbers change when the layout changes; a folio_mark survives
 *    re-layout.
 *
 * Copyright (C) 2026 Andrew Youll. AGPL-3.0-or-later (MuPDF is AGPL).
 */
#ifndef FOLIO_H
#define FOLIO_H

#include <stddef.h>
#include <stdint.h>

typedef struct folio_ctx folio_ctx;
typedef struct folio_doc folio_doc;

typedef struct { int chapter, page; } folio_loc;
typedef struct { float x0, y0, x1, y1; } folio_rect;
typedef struct { float x, y; } folio_point;
/* A quadrilateral (text runs can be rotated): ul, ur, ll, lr corners. */
typedef struct { folio_point ul, ur, ll, lr; } folio_quad;
typedef struct { int x0, y0, x1, y1; } folio_irect;

/* ---- Context ---------------------------------------------------------- */

/* store_limit: bytes MuPDF may keep in its cache (0 = MuPDF's default). */
folio_ctx *folio_new(size_t store_limit);
void folio_drop(folio_ctx *fc);
const char *folio_error(folio_ctx *fc);

/* ---- Documents -------------------------------------------------------- */

/* Opens a document. accel_path, if not NULL, names the layout accelerator
 * file for re-flowed books: it is read now if it exists (stale ones are
 * ignored by MuPDF), and written by folio_save_accel(). */
folio_doc *folio_open(folio_ctx *fc, const char *path, const char *accel_path);
void folio_close(folio_doc *doc);

int folio_is_reflowable(folio_doc *doc);
/* "PDF 1.7", "EPUB", "CBZ"... */
const char *folio_format(folio_doc *doc);
/* Title and author, or "" if none. Returned strings live until close. */
const char *folio_title(folio_doc *doc);
const char *folio_author(folio_doc *doc);
/* BCP 47 tag such as "en", "fr-CA", or "" if the document doesn't say:
 * EPUB dc:language, PDF /Lang. */
const char *folio_language(folio_doc *doc);

/* ---- Layout (re-flowed books only; ignored for fixed layout) ---------- */

typedef struct {
	float page_w, page_h;  /* page size in points */
	float em;              /* base font size in points */
	float margin;          /* page margin in points */
	float line_spacing;    /* 1.0 = the book's own */
	int publisher_css;     /* 1 = use the book's styles */
} folio_layout;
/* Dark and sepia pages are a rendering mode (folio_render_tile), not a
 * layout, so switching them never re-lays out a book. */

void folio_default_layout(folio_layout *lay);
/* A short hex key for the layout, for naming accelerator files: "%08x". */
unsigned folio_layout_key(const folio_layout *lay);
/* Changes the layout. Page counts restart from scratch (unless the
 * accelerator matches); use a folio_mark to keep the reading position. */
int folio_set_layout(folio_doc *doc, const folio_layout *lay);

/* ---- Counting pages, a chapter at a time (for null polls) ------------- */

int folio_chapters(folio_doc *doc);
/* Lays out (if needed) and counts up to max_chapters more chapters.
 * Returns 1 when every chapter is counted, 0 if more remain, -1 on error. */
int folio_count_step(folio_doc *doc, int max_chapters);
/* Pages in a chapter, or -1 if not counted yet. */
int folio_chapter_pages(folio_doc *doc, int chapter);
/* Total pages: exact once counting is complete (*exact = 1); before that an
 * estimate from the chapters counted so far (*exact = 0). */
int folio_page_count(folio_doc *doc, int *exact);
/* Page number (0-based) of a location; -1 if earlier chapters aren't
 * counted yet. */
int folio_page_number(folio_doc *doc, folio_loc loc);
/* Location of a page number; only valid when counting is complete or the
 * page lies in counted chapters. Returns -1 if not known yet. */
int folio_loc_from_number(folio_doc *doc, int number, folio_loc *loc);
/* Next/previous page; returns 0 and leaves *loc unchanged at either end.
 * Lays out the neighbouring chapter if needed. */
int folio_next(folio_doc *doc, folio_loc *loc);
int folio_prev(folio_doc *doc, folio_loc *loc);
/* Writes the accelerator (re-flowed books, after counting). */
int folio_save_accel(folio_doc *doc, const char *accel_path);

/* A reading position that survives re-layout and can be saved (e.g. in
 * Choices, to reopen a book where it was left): the chapter, how far through
 * it, and the first words of the page. (MuPDF's own EPUB bookmarks are
 * pointers into the laid-out chapter, so they go stale on re-layout.) */
typedef struct {
	int chapter;
	float fraction;     /* page / pages in chapter, 0..1 */
	char text[64];      /* UTF-8, first words of the page; may be "" */
} folio_mark;
int folio_make_mark(folio_doc *doc, folio_loc loc, folio_mark *mark);
/* Finds the page holding the mark's words, searching outwards from where
 * the fraction says it should be. Falls back to the fraction. */
int folio_find_mark(folio_doc *doc, const folio_mark *mark, folio_loc *loc);

/* ---- Contents --------------------------------------------------------- */

typedef struct {
	const char *title;
	int depth;
	folio_loc loc;      /* {-1,-1} if it points nowhere */
	const char *uri;    /* external link, or NULL */
} folio_outline_item;

/* Flattened contents list, in order. Valid until the next call, a layout
 * change, or close. Re-flowed books: locations follow the current layout. */
int folio_outline(folio_doc *doc, const folio_outline_item **items, int *count);

/* ---- Pages ------------------------------------------------------------ */

int folio_page_bounds(folio_doc *doc, folio_loc loc, folio_rect *bounds);

typedef enum {
	FOLIO_PAGE_NORMAL = 0,
	FOLIO_PAGE_DARK = 1,   /* black page, light text, pictures kept */
	FOLIO_PAGE_SEPIA = 2
} folio_page_mode;

/* Draws part of a page. zoom is pixels per point. tile is in pixels in the
 * page's zoomed space (page origin at 0,0). The output is RISC OS 32bpp
 * sprite pixels: one word per pixel, &00BBGGRR (bytes R, G, B, 0), rows
 * 'stride' bytes apart (a multiple of 4). annots: 1 to draw annotations. */
int folio_render_tile(folio_doc *doc, folio_loc loc, float zoom,
		folio_irect tile, folio_page_mode mode, int annots,
		uint32_t *pixels, int stride);

/* Throws away cached pages (display lists, text); call after an edit or to
 * free memory. */
void folio_forget_pages(folio_doc *doc);

/* ---- Text ------------------------------------------------------------- */

typedef enum { FOLIO_SELECT_CHARS, FOLIO_SELECT_WORDS, FOLIO_SELECT_LINES } folio_select_mode;

/* Snaps a selection from a to b (page points) to whole characters, words or
 * lines; a and b are updated to the snapped ends. */
int folio_snap(folio_doc *doc, folio_loc loc, folio_point *a, folio_point *b,
		folio_select_mode mode);
/* Quads covering the selection from a to b, for drawing it. Returns the
 * number written (up to max), or -1. */
int folio_selection_quads(folio_doc *doc, folio_loc loc, folio_point a, folio_point b,
		folio_quad *quads, int max);
/* The selected text as UTF-8 (lines joined with '\n'); free with folio_free. */
char *folio_selection_text(folio_doc *doc, folio_loc loc, folio_point a, folio_point b);
/* The word under p (for double-click and the dictionary). Returns UTF-8 text
 * (free with folio_free) and sets *a and *b to its ends; NULL if none. Leading
 * and trailing punctuation is trimmed from the returned text. */
char *folio_word_at(folio_doc *doc, folio_loc loc, folio_point p,
		folio_point *a, folio_point *b);
void folio_free(folio_doc *doc, void *p);

/* Find: case-insensitive. Hit quads on one page; returns the number of quads
 * (up to max), with mark[i] = 1 where a new hit starts; -1 on error. */
int folio_find_on_page(folio_doc *doc, folio_loc loc, const char *needle,
		folio_quad *quads, int *mark, int max);

/* ---- Links ------------------------------------------------------------ */

typedef struct {
	folio_rect rect;
	folio_loc dest;     /* {-1,-1} if external */
	float dest_y;       /* point on the destination page, or -1 */
	const char *uri;    /* the link's URI (valid until the page is forgotten) */
} folio_link;

/* Link at p on the page, if any (returns 1 and fills *link), else 0. */
int folio_link_at(folio_doc *doc, folio_loc loc, folio_point p, folio_link *link);

#endif
