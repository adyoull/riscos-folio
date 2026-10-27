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
/* folio_render_tile's flags: annotations and form fields drawn; the page's
 * links underlined (in blue, light blue on a dark page). */
#define FOLIO_RENDER_ANNOTS 1
#define FOLIO_RENDER_LINKS  2
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
/* A selection end can be the first or last character of the page's text
 * (in reading order), whatever is nearest the page's corners: a point whose
 * x is FOLIO_TEXT_START or FOLIO_TEXT_END (y is ignored). For selections
 * over several pages. */
#define FOLIO_TEXT_START (-100000.0f)
#define FOLIO_TEXT_END   (-200000.0f)
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
/* A searchable copy: every recognised page of a PDF gets its words as
 * invisible text over their pictures, so other readers can find and copy
 * them too. One change in the journal; Save keeps it. Pages done before
 * are left alone. Returns the pages done (0: none to do), or -1. */
int folio_ocr_embed(folio_doc *doc);
int folio_ocr_pages(folio_doc *doc);      /* pages recognised */
int folio_ocr_changed(folio_doc *doc);    /* recognised since loaded or saved */
/* The recognised text of every page, kept between sessions: saving with
 * none removes the file. Loading checks the file is for this document (its
 * page count, and a PDF's ID or else the file's size) and returns the pages
 * restored, or -1. */
int folio_ocr_save(folio_doc *doc, const char *path);
int folio_ocr_load(folio_doc *doc, const char *path);

/* ---- Document info ------------------------------------------------------ */

#define FOLIO_INFO_MAX 40
typedef struct {
	char label[32];
	char value[256];      /* UTF-8 */
} folio_info_item;
typedef struct {
	int n;
	folio_info_item item[FOLIO_INFO_MAX];
} folio_info;
/* The document's properties, in order, for showing (label: value). */
int folio_get_info(folio_doc *doc, folio_info *in);
/* The fonts a PDF uses, each once, sorted by name (no subset prefix), and
 * how many pictures it holds. Looks for at most `seconds` (0: no limit);
 * pages_done < pages says it stopped early. -1 if not a PDF. */
#define FOLIO_FONTS_MAX 400
typedef struct {
	char name[96];
	char type[28];        /* "Type 1", "TrueType", "Type 1 (CID)", "Type 3"... */
	int embedded;         /* 0 not, 1 embedded, 2 embedded subset, 3 drawn in the PDF (Type 3) */
} folio_font;
typedef struct {
	int n, more;          /* fonts listed, fonts beyond FOLIO_FONTS_MAX */
	int pages, pages_done;
	int pictures;
	folio_font font[FOLIO_FONTS_MAX];
} folio_fonts;
int folio_get_fonts(folio_doc *doc, folio_fonts *out, int seconds);
/* A PDF date ("D:20230115093000+01'00'") for people: "15 Jan 2023, 09:30". */
void folio_pdf_date(const char *d, char *out, size_t size);

/* What a PDF holds that a careful reader may want to know of (counts). */
typedef struct {
	int pdf;            /* 0: not a PDF, the rest are 0 */
	int javascript;     /* scripts: document, page, field and link actions */
	int files;          /* attached files */
	int fields;         /* form fields */
	int xfa;            /* an XFA form (Folio shows its ordinary form, if any) */
	int launch;         /* actions that start another program or open a file */
	int submit;         /* actions that send the form, or read data in */
	int other_files;    /* links into other PDF files */
	int uri;            /* links to web pages and email */
} folio_content;
int folio_get_content(folio_doc *doc, folio_content *c);

/* ---- Signatures (sign.c) ---------------------------------------------------- */

enum { FOLIO_SIG_VALID = 1,      /* the bytes are as signed, and the signature is the signer's */
	FOLIO_SIG_CHANGED,           /* the signed bytes have been changed */
	FOLIO_SIG_UNREADABLE };      /* the signature can't be checked (why says) */
enum { FOLIO_CERT_TRUSTED = 1,   /* leads to a trusted root */
	FOLIO_CERT_SELF_SIGNED,      /* the signer vouches for itself */
	FOLIO_CERT_UNTRUSTED,        /* doesn't lead to a root Folio trusts */
	FOLIO_CERT_NONE,             /* no certificate in the signature */
	FOLIO_CERT_OTHER };          /* something else wrong with it (cert_why says) */
typedef struct {
	char field[64];              /* the field's name */
	int page;                    /* 0-based */
	folio_rect rect;             /* its box on the page, folio points */
	int is_signed;               /* 0: an empty signature field */
	int digest;                  /* FOLIO_SIG_* */
	int cert;                    /* FOLIO_CERT_* */
	int expired;                 /* the certificate is out of date now */
	int changed_after;           /* the file has had changes since this signature */
	char signer[128], organisation[128], email[96];   /* UTF-8 */
	char date[40];               /* the PDF date it gives (folio_pdf_date makes it readable) */
	char reason[160], location[96];
	char why[200], cert_why[160];
	int certified;               /* a certification's P: 1 no changes, 2 filling in and signing,
	                              * 3 and notes; 0 not certified */
	int changes;                 /* what's been changed since: FOLIO_AFTER_* */
	char disallowed[160];        /* "" or why those changes break it (as Acrobat judges) */
} folio_signature;
enum { FOLIO_AFTER_FORM = 1, FOLIO_AFTER_SIGNING = 2, FOLIO_AFTER_ANNOT = 4, FOLIO_AFTER_PAGE = 8,
	FOLIO_AFTER_OTHER = 16 };
/* The document's signature fields, signed or not, with each signature
 * checked. Returns how many (up to max), or -1. */
int folio_signatures(folio_doc *doc, folio_signature *out, int max);
/* How many signature fields a PDF has (signed or not), cheaply. */
int folio_has_signatures(folio_doc *doc);
/* Certificates to trust as roots: a PEM bundle (such as CaCertificates'
 * ca-certificates.crt) or one DER certificate; added to Adobe's root, which
 * is always trusted. Returns how many were added, or -1. */
int folio_set_trust_roots(folio_ctx *fc, const unsigned char *data, size_t len);
int folio_trust_root_count(void);
/* A signed (or certified) PDF: changes that would break or alter a
 * signature are refused (pages, page content, its password, locked
 * fields, and what a certification forbids), and it's only saved
 * incrementally. */
int folio_is_signed(folio_doc *doc);
/* Signing with a certificate from a .p12 / .pfx file (PKCS#12: a private
 * key and its certificates, under a password). NULL, with folio_error
 * saying why ("the password isn't right"...), if it can't be used. */
typedef struct folio_signer folio_signer;
folio_signer *folio_signer_open(folio_ctx *fc, const unsigned char *p12, size_t len, const char *password);
void folio_signer_drop(folio_ctx *fc, folio_signer *s);
void folio_signer_name(folio_signer *s, char *out, size_t size);   /* its common name, UTF-8 */
/* Signs the empty signature field called field on the page, or, with field
 * NULL, a new one at where (folio points). The appearance shows the name,
 * date and reason. One change in the journal; the signature itself is made
 * when the PDF is next saved (folio_save_pdf). 0 or -1. */
enum {
	FOLIO_SIGN_APPROVE = 0,     /* an ordinary signature */
	FOLIO_SIGN_LOCK,            /* and lock the document: no changes after it (fields read-only) */
	FOLIO_CERTIFY_NO_CHANGES,   /* certify (the first signature only): no changes allowed */
	FOLIO_CERTIFY_FORMS,        /* certify: filling in and signing allowed */
	FOLIO_CERTIFY_ANNOTS        /* certify: those, and notes and markings */
};
int folio_sign(folio_doc *doc, folio_loc loc, const char *field, folio_rect where, folio_signer *s,
		const char *reason, const char *location, int mode);

/* ---- System fonts (fonts.c) ---------------------------------------------- */
/* What fontconfig found when it started (on the first font asked for), for
 * the log: "fontconfig 2.14.1: 61 fonts", or why not. */
const char *folio_fonts_status(void);

/* ---- Forms (PDFs; form.c) ------------------------------------------------ */

typedef enum {
	FOLIO_FIELD_NONE, FOLIO_FIELD_TEXT, FOLIO_FIELD_CHECK, FOLIO_FIELD_RADIO,
	FOLIO_FIELD_COMBO, FOLIO_FIELD_LIST, FOLIO_FIELD_BUTTON, FOLIO_FIELD_SIGNATURE
} folio_field_kind;

typedef struct {
	folio_field_kind kind;
	int index;          /* its number on the page (the order Tab goes in) */
	folio_rect rect;    /* page points from the top left */
	char name[128];     /* UTF-8, the full field name */
	char value[1024];   /* UTF-8: the text, the chosen option, or the on state */
	int on;             /* check box or radio button: this one is on */
	int read_only;
	int multiline, password, max_len;   /* text fields (max_len 0: none) */
	int options;        /* combo and list boxes: how many */
} folio_field;

int folio_field_count(folio_doc *doc, folio_loc loc);
/* 1 and *f for the page's field 'index' (or the one at p); 0 if none; -1. */
int folio_field_get(folio_doc *doc, folio_loc loc, int index, folio_field *f);
int folio_field_at(folio_doc *doc, folio_loc loc, folio_point p, folio_field *f);
/* A combo or list box's option i, as shown. 0 or -1. */
int folio_field_option(folio_doc *doc, folio_loc loc, int index, int i, char *buf, size_t size);
/* Changes (one undoable operation each). set_text: 0 taken, 1 turned down by
 * the form's script, -1 an error. */
int folio_field_set_text(folio_doc *doc, folio_loc loc, int index, const char *utf8);
int folio_field_toggle(folio_doc *doc, folio_loc loc, int index);
int folio_field_choose(folio_doc *doc, folio_loc loc, int index, int option);
/* A push button: its script runs (only with JavaScript on). 1 ran, 0 not. */
int folio_field_press(folio_doc *doc, folio_loc loc, int index);
/* Turns on the form's JavaScript (it can't be turned off again for this
 * document). 1 if it's on. What the scripts ask for is kept to be taken. */
int folio_form_js(folio_doc *doc, int on);
int folio_form_take_alert(folio_doc *doc, char *buf, size_t size);
int folio_form_take_url(folio_doc *doc, char *buf, size_t size);

/* ---- XFA forms (Adobe LiveCycle; xfa.c) ------------------------------------
 * Filled in as a list of fields: the values are kept in the form's XFA
 * datasets, by path (form1.Page1.Name). A static ("hybrid") form also has
 * ordinary fields on its pages, kept in step; a dynamic one has only a
 * "Please wait..." page, as MuPDF can't lay out XFA. */
enum { FOLIO_XFA_NONE, FOLIO_XFA_HYBRID, FOLIO_XFA_DYNAMIC };
int folio_xfa_kind(folio_doc *doc);

typedef enum {
	FOLIO_XFA_TEXT, FOLIO_XFA_MULTILINE, FOLIO_XFA_NUMBER, FOLIO_XFA_DATE,
	FOLIO_XFA_CHECK, FOLIO_XFA_CHOICE, FOLIO_XFA_PASSWORD
} folio_xfa_type;

#define FOLIO_XFA_OPTS 16
typedef struct {
	char path[256];         /* where the value is kept: form1.Item[2].Price */
	char label[160];        /* the caption (UTF-8), or the field's name */
	char value[512];        /* UTF-8 */
	folio_xfa_type type;
	int read_only;
	int nopts;              /* choices (and radio groups); check: [0] on, [1] off */
	char opt[FOLIO_XFA_OPTS][64];    /* as shown */
	char save[FOLIO_XFA_OPTS][64];   /* as kept */
	int chosen;             /* the value's place in save[], or -1 */
} folio_xfa_field;

/* The fields, in the form's order (data first, then fields with no value
 * yet): how many there are (more than max: only max filled), or -1. */
int folio_xfa_fields(folio_doc *doc, folio_xfa_field *out, int max);
/* Sets a field's value (for a choice or check box, one of save[]). One
 * change in the journal. 0 or -1 (folio_error). */
int folio_xfa_set(folio_doc *doc, const char *path, const char *value);

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

/* ---- Pages out, pictures in (pages.c) ------------------------------------ */

/* A list of pages ("1-10, 15, 20-", "-5", "all"; blank = all) into sel[]:
 * sel[i] = 1 for page i (0-based) of count. Returns the pages chosen, or -1
 * if it can't be read or names a page past the end. */
int folio_parse_pages(const char *spec, int count, unsigned char *sel);
/* The chosen pages as a new PDF. A PDF's pages are copied as they are
 * (contents, pictures, fonts, annotations); other documents' pages are
 * drawn into PDF pages, JPEG pictures kept as they are. 0 or -1. */
int folio_save_pages(folio_doc *doc, const unsigned char *sel, int count, const char *path);
/* The page's picture, written to path. Returns what was written, or -1:
 *  FOLIO_PAGEIMG_JPEG: the page is one JPEG picture: those bytes, as stored;
 *  FOLIO_PAGEIMG_IMAGE_PNG: the page is one picture of another kind: it, at
 *    its own size, as a PNG;
 *  FOLIO_PAGEIMG_PAGE_PNG: the whole page drawn at dpi, as a PNG.
 * "One picture": it covers the page and nothing visible is drawn on top
 * (a scan's hidden recognised text is allowed). */
enum { FOLIO_PAGEIMG_JPEG = 1, FOLIO_PAGEIMG_IMAGE_PNG = 2, FOLIO_PAGEIMG_PAGE_PNG = 3 };
int folio_save_page_image(folio_doc *doc, folio_loc loc, int dpi, const char *path);
/* Picture files (JPEG, PNG, GIF, BMP, TIFF...) as the pages of a new PDF, in
 * the order given: each page the picture's size at its resolution, a JPEG
 * put in as it is. 0 or -1 (folio_error, naming the file that failed). */
int folio_images_to_pdf(folio_ctx *fc, const char *const *paths, int n, const char *path);
/* The same, every picture taken as dpi (a scan's pages: the resolution
 * scanned at, which their JPEGs don't always say); 0 = their own. */
int folio_images_to_pdf_res(folio_ctx *fc, const char *const *paths, int n, int dpi, const char *path);
/* Files joined into one new PDF, in the order given: a PDF's pages copied
 * as they are; a picture as a page of its size; any other document Folio
 * reads (CBZ, XPS, an ebook) drawn into PDF pages, JPEGs kept as they are.
 * 0 or -1 (folio_error names the file that failed). */
int folio_combine(folio_ctx *fc, const char *const *paths, int n, const char *path);
/* The resolution given to a w x h pixel picture that doesn't say (Netpbm):
 * the usual scanning resolution (96, 150, 200, 300, 400, 600, 1200) that
 * makes it no bigger than 8.5 x 14 inches. */
int folio_paper_dpi(int w, int h);
/* Organising a PDF's pages, each one change in the journal (Undo puts it
 * back); recognised text goes with its page. Deleting every page is refused.
 * folio_move_pages: the chosen pages, in order, to stand before page `to`
 * (0-based, as numbered before the move; the page count: at the end).
 * folio_insert_file: the pages of a PDF, picture or other document Folio
 * reads, before page `at` (-1: at the end). 0 or -1 (folio_error). */
int folio_delete_pages(folio_doc *doc, const unsigned char *sel, int count);
int folio_move_pages(folio_doc *doc, const unsigned char *sel, int count, int to);
int folio_insert_file(folio_doc *doc, const char *path, int at);
/* The chosen pages' text (UTF-8, malloc'd; free()), in reading order: each
 * paragraph one line, a blank line between paragraphs and between pages; a
 * re-flowed book's sentence carried over a page break is joined. Scanned
 * pages give their recognised text if they have it. *empty counts pages
 * with no text at all. progress (or NULL) is called before each page with
 * the pages done. NULL on error (folio_error). */
char *folio_pages_text(folio_doc *doc, const unsigned char *sel, int count, int *empty,
		void (*progress)(int done, void *arg), void *arg);
/* Turning PDF pages: the chosen pages turned by degrees (90 clockwise,
 * -90 anticlockwise, 180), one undoable operation; their recognised text
 * (OCR) is dropped. And a page's turn now (0, 90, 180, 270). */
int folio_rotate_pages(folio_doc *doc, const unsigned char *sel, int count, int degrees);
/* Picture files added as new pages at the end of a PDF (a scan's next
 * pages), each its size at dpi (0: the picture's own); one undoable
 * operation. 0 or -1. */
int folio_append_pictures(folio_doc *doc, const char *const *paths, int n, int dpi);
int folio_page_rotation(folio_doc *doc, folio_loc loc);

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
/* The colour new highlights, underlines, strike-outs or notes get:
 * 0xRRGGBB, or -1 for the kind's own (yellow, blue, red, yellow). */
void folio_set_annot_colour(folio_ctx *fc, folio_annot_kind kind, int rgb);
/* The page's annotations (not links, form fields or pop-ups), in order, up
 * to max: their numbers (as folio_annot_at's), kinds, colours, and the note's
 * text or, for markup with none, the text it marks. Returns how many, or -1. */
typedef struct {
	int n;
	folio_annot_kind kind;
	char type[24];         /* MuPDF's name: "Highlight", "Text", "Ink", "FreeText"... */
	folio_rect rect;       /* folio points */
	int rgb;               /* 0xRRGGBB, or -1 if it has none */
	char text[200];        /* UTF-8, line breaks as spaces, cut */
	char author[64];
} folio_annot_info;
int folio_annot_list(folio_doc *doc, folio_loc loc, folio_annot_info *out, int max);
/* Annotation n's colour, as one change in the journal. 0 or -1. */
int folio_annot_recolour(folio_doc *doc, folio_loc loc, int n, int rgb);
/* Measuring: how much one point on this page is (*per_point), in unit. By
 * default millimetres (allowing for the page's UserUnit); if the PDF gives
 * the page a scale of its own (a /VP viewport with a /Measure, as plans and
 * drawings do), that scale and its unit, and it returns 1. */
int folio_page_measure(folio_doc *doc, folio_loc loc, double *per_point, char *unit, size_t unit_size);
/* A measurement kept on the page: a line (two points), lines or (closed) a
 * polygon through the points (folio points), red, with text as its note.
 * One change in the journal. 0 or -1. */
int folio_annot_measure(folio_doc *doc, folio_loc loc, const folio_point *pt, int n, int closed,
		const char *text);
/* Freehand drawing (an Ink annotation): strokes of folio points, counts[i]
 * points in stroke i, all in pts one after another; rgb 0xRRGGBB; width in
 * points. 0 or -1. */
int folio_annot_ink(folio_doc *doc, folio_loc loc, const folio_point *pts, const int *counts, int strokes,
		int rgb, float width);
/* The standard stamps, by number from 0 ("Approved", "NotApproved", "Draft",
 * "Final", ...): their PDF names, NULL past the last. */
const char *folio_stamp_name(int i);
/* A standard stamp centred at centre. 0 or -1. */
int folio_annot_stamp(folio_doc *doc, folio_loc loc, folio_point centre, const char *name);
/* A picture (JPEG, PNG and the rest) as a stamp, centred at centre, width
 * points wide (0: two inches), its shape kept; clear_white makes white
 * see-through (a signature on paper). 0 or -1. */
int folio_annot_picture(folio_doc *doc, folio_loc loc, folio_point centre, float width, const char *path,
		int clear_white);

/* Undo and redo of annotation changes (every page is redrawn after). */
int folio_can_undo(folio_doc *doc);
int folio_can_redo(folio_doc *doc);
int folio_undo(folio_doc *doc);
/* A number that changes whenever a change is made, undone or redone. */
int folio_change_count(folio_doc *doc);
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

/* What the document's author allows (a PDF's permission flags; anything
 * else allows everything). Opening with the owner password allows all. */
typedef enum {
	FOLIO_PERM_PRINT, FOLIO_PERM_COPY,
	FOLIO_PERM_ACCESSIBILITY,   /* text for screen readers (reading aloud) */
	FOLIO_PERM_ANNOTATE,
	FOLIO_PERM_FORM,            /* filling in its form */
	FOLIO_PERM_ASSEMBLE         /* turning, deleting, moving or inserting pages */
} folio_permission;
int folio_permitted(folio_doc *doc, folio_permission what);
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
