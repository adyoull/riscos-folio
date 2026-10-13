/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
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

/* Why the last folio_open failed. */
enum { FOLIO_ERR_OTHER = 0, FOLIO_ERR_PASSWORD = 1, FOLIO_ERR_WRONG_PASSWORD = 2 };
int folio_error_code(folio_ctx *fc);

/* Opens a document. accel_path, if not NULL, names the layout accelerator
 * file for re-flowed books: it is read now if it exists (stale ones are
 * ignored by MuPDF), and written by folio_save_accel(). */
folio_doc *folio_open(folio_ctx *fc, const char *path, const char *accel_path);
/* The same, for a document that may need a password (UTF-8, or NULL for
 * none). Fails with folio_error_code() FOLIO_ERR_PASSWORD if one is needed
 * and none was given, FOLIO_ERR_WRONG_PASSWORD if it isn't right. */
folio_doc *folio_open_password(folio_ctx *fc, const char *path, const char *accel_path,
		const char *password);
void folio_close(folio_doc *doc);
/* Opens a small HTML document held in memory (the dictionary pop-up), laid
 * out as one long page width_pt wide, with em the body font size. */
folio_doc *folio_open_html(folio_ctx *fc, const char *html, float width_pt, float em);
/* The height of what's drawn on the page (points from its top). */
int folio_content_height(folio_doc *doc, folio_loc loc, float *height);

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

/* A page's width and height in points, without drawing it or keeping it
 * (cheap: for laying out documents whose pages differ in size). */
int folio_page_size(folio_doc *doc, folio_loc loc, float *w, float *h);
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

/* Stops a page load or folio_render_tile running in another thread as soon
 * as MuPDF next checks (it then fails with folio_aborted() true). The only
 * call that may be made while another thread is inside the core. */
void folio_abort(folio_doc *doc);
/* Was the last failure an abort? */
int folio_aborted(folio_doc *doc);

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

/* The page's text in reading order, for reading aloud: UTF-8, with the
 * character each byte belongs to (-1 for spaces and breaks added between
 * lines and blocks) and each character's quad. Words hyphenated at line
 * ends are joined (the hyphen left out); ligatures are spelt out; each text
 * block ends with a blank line (a paragraph). Free with folio_page_text_free. */
typedef struct {
	char *text;            /* NUL-terminated */
	int len;
	int *byte_char;        /* len entries */
	folio_quad *quads;     /* nchars entries */
	int nchars;
} folio_page_text;
int folio_get_page_text(folio_doc *doc, folio_loc loc, folio_page_text *t);
void folio_page_text_free(folio_page_text *t);

/* ---- Text recognition (OCR) ------------------------------------------- */
/* Scanned pages have no text, only a picture of it. folio_ocr_page reads the
 * picture with Tesseract (300 dpi grey); afterwards the page's text, for
 * selection, Copy, Find, Define and read aloud, is the recognised text.
 * Recognising a page takes seconds: call it from the worker. */

#define FOLIO_OCR_NOT_SCAN (-2)

/* 1 if this build can recognise text and the document has fixed pages. */
int folio_ocr_supported(folio_doc *doc);
/* 1 if the page has no text of its own but has pictures; 0; -1 on error. */
int folio_page_is_scan(folio_doc *doc, folio_loc loc);
/* datadir: the folder with the language data, lang: e.g. "eng". On RISC OS
 * the data is "<datadir>.<lang>"; elsewhere "<datadir>/<lang>.traineddata".
 * only_scans: leave pages with text of their own alone (FOLIO_OCR_NOT_SCAN).
 * Returns the characters found (0 or more), or -1 (folio_error; stopped by
 * folio_abort if folio_aborted). */
int folio_ocr_page(folio_doc *doc, folio_loc loc, const char *datadir, const char *lang,
		int only_scans);
int folio_page_has_ocr(folio_doc *doc, folio_loc loc);
int folio_ocr_pages(folio_doc *doc);      /* pages recognised */
int folio_ocr_changed(folio_doc *doc);    /* recognised since loaded or saved */
/* The recognised text of every page, kept between sessions: saving with
 * none removes the file. Loading checks the file is for this document (its
 * page count, and a PDF's ID or else the file's size) and returns the pages
 * restored, or -1. */
int folio_ocr_save(folio_doc *doc, const char *path);
int folio_ocr_load(folio_doc *doc, const char *path);

/* ---- Links ------------------------------------------------------------ */

typedef struct {
	folio_rect rect;
	folio_loc dest;     /* {-1,-1} if external */
	float dest_y;       /* point on the destination page, or -1 */
	const char *uri;    /* the link's URI (valid until the page is forgotten) */
} folio_link;

/* Link at p on the page, if any (returns 1 and fills *link), else 0. */
int folio_link_at(folio_doc *doc, folio_loc loc, folio_point p, folio_link *link);

/* ---- Draw files ---------------------------------------------------------- */

/* Writes the page (with its annotations) to path as a RISC OS Draw file:
 * paths, text as outlines, each picture as a sprite of its own (core/drawfile.c).
 * The caller sets the file's type (&AFF). 0, or -1 (folio_error). */
int folio_export_draw(folio_doc *doc, folio_loc loc, const char *path);

/* ---- Annotations (PDFs) ------------------------------------------------ */

typedef enum {
	FOLIO_ANNOT_HIGHLIGHT, FOLIO_ANNOT_UNDERLINE, FOLIO_ANNOT_STRIKEOUT,
	FOLIO_ANNOT_NOTE, FOLIO_ANNOT_OTHER
} folio_annot_kind;

/* 1 if annotations can be added (PDFs). */
int folio_can_annotate(folio_doc *doc);
/* Highlights, underlines or strikes out the text from a to b (as for
 * folio_selection_quads). One undoable operation. 0 or -1. */
int folio_annot_markup(folio_doc *doc, folio_loc loc, folio_annot_kind kind,
		folio_point a, folio_point b);
/* A note (sticky-note icon) with text, its top left at 'at'. */
int folio_annot_note(folio_doc *doc, folio_loc loc, folio_point at, const char *text);
/* The annotation under p: its number on the page, kind and box; -1 if none.
 * Text markup is hit only on its quads. */
int folio_annot_at(folio_doc *doc, folio_loc loc, folio_point p, folio_annot_kind *kind,
		folio_rect *rect);
/* Its text (folio_free it), or NULL. */
char *folio_annot_text(folio_doc *doc, folio_loc loc, int n);
int folio_annot_set_text(folio_doc *doc, folio_loc loc, int n, const char *text);
int folio_annot_delete(folio_doc *doc, folio_loc loc, int n);

/* Undo and redo of annotation changes (every page is redrawn after). */
int folio_can_undo(folio_doc *doc);
int folio_can_redo(folio_doc *doc);
int folio_undo(folio_doc *doc);
int folio_redo(folio_doc *doc);
/* Unsaved changes? */
int folio_is_modified(folio_doc *doc);
/* Writes the PDF with its changes to path, which must not be the open file
 * (save to a new file, close, then replace). original is the open file's
 * path, for an incremental save (or NULL for a full one). */
int folio_save_pdf(folio_doc *doc, const char *original, const char *path);

/* ---- Passwords (PDFs) ---------------------------------------------------- */

enum { FOLIO_PW_KEEP = 0, FOLIO_PW_SET = 1, FOLIO_PW_REMOVE = 2 };

typedef struct {
	int pdf;            /* 0: not a PDF, none of this applies */
	int encrypted;      /* the file is encrypted (a password, or restrictions) */
	int needs_password; /* opening it needs a password */
	int owner;          /* opened with full rights, so its security can be changed */
	int change;         /* FOLIO_PW_*: what the next save does */
	char method[32];    /* "AES 256", "RC4 128"..., or "" */
} folio_security;

int folio_get_security(folio_doc *doc, folio_security *sec);
/* Is this the document's owner password? 1 (and the document now has full
 * rights), 0, or -1 on error. */
int folio_check_owner_password(folio_doc *doc, const char *password);
/* Sets a password (UTF-8) to open the document, or removes it (NULL or "").
 * Needs full rights. It's applied when the document is next saved (a full
 * save, encrypted with AES-256, the same password for owner and user); until
 * then the document counts as modified. Setting a password needs a source of
 * strong random numbers (on RISC OS, /dev/urandom, i.e. the CryptRandom
 * module); without one this fails. */
int folio_set_password(folio_doc *doc, const char *password);

#endif
