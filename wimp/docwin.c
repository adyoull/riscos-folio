/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio document windows: pages stacked for continuous scrolling, drawn
 * from a cache of page sprites. Missing pages are loaded and drawn by the
 * worker thread (worker.c), given time after each poll; re-flowed books are
 * counted a chapter at a time; so the desktop never waits (Style Guide p103;
 * DESIGN.md §7). Pages ahead in the direction of reading are drawn early.
 *
 * Work area: x from 0 to ext_w, y from 0 down to -ext_h (OS units). Pages
 * are laid out in rows of one, or two side by side (View > Two pages), the
 * first page optionally alone on the right as in a book. Each page has its
 * own size (measured in the background: sizes_next; until then page 1's),
 * a row is as tall as its tallest page, and page i occupies
 * [left(i), left(i) + slot_w(i)] x [top(i) - slot_h(i), top(i)]. */
#include "app.h"
#include "worker.h"
#include "dict.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define GAP 32                    /* OS units around and between pages */
#define CACHE_BYTES (48 << 20)    /* page sprites (more for pages on screen) */
#define CACHE_SLOTS 32
#define MIN_ZOOM 0.25f
#define MAX_ZOOM 4.0f
#define SEL_QUADS 256
#define BACK_MAX  32        /* places remembered for Back */
#define SPK_BOXES 64

struct docwin {
	docwin *next;
	ObjectId obj;
	int wimp;
	folio_doc *doc;
	char path[256];
	char title[256];
	char accel[256];
	int reflow;
	folio_page_mode mode;
	float zoom;               /* 1.0 = actual size */
	int gen;                  /* bumped when zoom or mode changes */
	folio_rect page;          /* first page's size in points */
	/* Page sizes in points (0: not measured yet, page 1's is used), and
	 * where each row starts below the first (OS units; rows + 1 entries). */
	float *pw, *ph;
	int nsize, sizes_next;
	int *row_y, nrow_y;
	float max_pw;             /* the widest page measured */
	int ext_w, ext_h;
	int pages;                /* pages in the extent */
	int counting;             /* 1 until every chapter is counted */
	int want_render;          /* a visible page was drawn as a placeholder */
	int dir;                  /* +1 reading forwards, -1 backwards */
	int last_page;
	int fail_page, fail_gen;  /* a page that failed, not to be retried */
	int pending_mark;         /* mark to go to once its chapter is counted */
	int goto_page;            /* page asked for on opening, once counted; -1 none */
	folio_mark mark;
	char place[256];          /* where the reading position is kept */
	/* Selection: from sel_a on page sel_page (where it was started) to
	 * sel_b on page sel_end; it can run over several pages. Its boxes on
	 * each page are kept in selslot[] below. */
	int sel, sel_page, sel_end, dragging;
	int sel_stale;            /* a page showing still needs its boxes drawn */
	unsigned drag_cs;         /* when the drag last scrolled */
	folio_point sel_a, sel_b;
	char sel_word[64];        /* set by a double-click on a word */
	/* Toolbar and thumbnail drawer. */
	ObjectId toolbar;
	int show_toolbar, show_thumbs;
	thumbs *thumbs;
	int top, left;            /* work area kept clear for them, OS units */
	/* The annotation clicked on (PDFs): page -1 if none. */
	int asel_page, asel_index;
	folio_rect asel_rect;
	int can_annotate;
	int xfa;                    /* FOLIO_XFA_*: an XFA form */
	int modified;             /* as last shown in the title */
	int unsaved_new;          /* made by Folio (pictures to PDF), not saved yet */
	int menu_page;            /* the page Menu was pressed over (Rotate), or -1 */
	int fit_polls;            /* polls left to keep checking the toolbar's width */
	int grew_left, grew_right;  /* how much the window was widened for the drawer */
	/* The sentence being read aloud: boxes and their pages (n 0: none). */
	int spk_n;
	int spk_page[SPK_BOXES];
	folio_quad spk_q[SPK_BOXES];
	int cols;                 /* pages side by side: 1 or 2 */
	int cover;                /* two pages: the first one alone, on the right */
	int grew_two;             /* how much the window was widened for two pages */
	/* PDF passwords: the one it was opened with (UTF-8), for opening it again
	 * after saving, and a new one waiting to be saved. */
	char password[128], new_password[128];
	char note[48];            /* after the title, e.g. while printing */
	/* Links: the one Select went down on (followed when the button comes up
	 * without a drag), and the places to go Back to. */
	int link_down;
	folio_loc link_dest;
	float link_y;
	char link_uri[512];
	struct { int page, y, cols; float zoom; } back[BACK_MAX];
	int back_n;
	ocr_win *ocr;             /* text recognition (NULL without the plugin) */
	/* What the author allows (a PDF's permissions; read with the core lock
	 * after opening, saving and the owner password). */
	int perm_print, perm_copy, perm_access, perm_form, perm_assemble;
	int meas_on, meas_page, meas_n;   /* measuring: the points, on one page */
	folio_point meas_pt[MEASURE_MAX];
	int pen_on, pen_page, pen_n, pen_strokes, pen_drag;   /* drawing: the strokes, on one page */
	folio_point *pen_pt;
	int pen_count[PEN_STROKES];
	int pen_rgb;
	float pen_width;
	char stamp_pending[24];          /* a stamp waiting for its click on the page */
	char pic_pending[256];           /* or a picture */
	int pic_clear;
	/* Full screen: the window fills the screen with no title bar or scroll
	 * bars; what it was is kept to go back to. */
	int full;
	int fs_box[4];
	unsigned fs_flags;
	int fs_toolbar, fs_thumbs;
	/* The window's place as it was left (load_place), screen OS units. */
	int have_view, view[4];
};

typedef struct {
	docwin *w;
	int page, gen;
	folio_page_mode mode;
	unsigned age;
	int *area;                /* sprite area (malloc) */
	int w_px, h_px;
	size_t bytes;
} cache_entry;

static docwin *windows;
static cache_entry cache[CACHE_SLOTS];
static size_t cache_bytes;
static unsigned cache_clock;
static int open_count;        /* for placing new windows */

/* ---- Helpers ---------------------------------------------------------- */

static float px_per_pt(const docwin *w)
{
	/* 1 pt = 180/72 = 2.5 OS units; one pixel = 1 << xeig OS units. */
	return w->zoom * 2.5f / (float)(1 << app.xeig);
}

/* Empty places before page 0 in the first row (1 when the first page is
 * alone on the right). */
static int lead(const docwin *w)
{
	return w->cols == 2 && w->cover ? 1 : 0;
}

static int row_of(const docwin *w, int i)
{
	return (i + lead(w)) / w->cols;
}

static int rows(const docwin *w)
{
	return (w->pages + lead(w) + w->cols - 1) / w->cols;
}

/* The first and last pages in row r. */
static int first_in_row(const docwin *w, int r)
{
	int i = r * w->cols - lead(w);
	return i < 0 ? 0 : i;
}

static int last_in_row(const docwin *w, int r)
{
	int i = (r + 1) * w->cols - 1 - lead(w);
	return i >= w->pages ? w->pages - 1 : i;
}

/* Page i's size in points, and in pixels and OS units at the zoom. */
static float pt_w(const docwin *w, int i)
{
	return i >= 0 && i < w->nsize && w->pw[i] > 0 ? w->pw[i] : w->page.x1;
}

static float pt_h(const docwin *w, int i)
{
	return i >= 0 && i < w->nsize && w->ph[i] > 0 ? w->ph[i] : w->page.y1;
}

static int px_w(const docwin *w, int i)
{
	int v = (int)(pt_w(w, i) * px_per_pt(w) + 0.5f);
	return v < 1 ? 1 : v;
}

static int px_h(const docwin *w, int i)
{
	int v = (int)(pt_h(w, i) * px_per_pt(w) + 0.5f);
	return v < 1 ? 1 : v;
}

static int slot_w(const docwin *w, int i) { return px_w(w, i) << app.xeig; }
static int slot_h(const docwin *w, int i) { return px_h(w, i) << app.xeig; }

/* Where row r starts, below the top of the first row. */
static int row_start(const docwin *w, int r)
{
	if (r < 0)
		return 0;
	if (r >= w->nrow_y)
		r = w->nrow_y - 1;
	return r >= 0 ? w->row_y[r] : 0;
}

/* The row at distance off below the top of the first row (the last row
 * starting at or above it). */
static int row_at(const docwin *w, int off)
{
	int lo = 0, hi = rows(w) - 1;
	if (hi < 0 || !w->row_y)
		return 0;
	if (off <= 0)
		return 0;
	while (lo < hi) {
		int mid = (lo + hi + 1) / 2;
		if (w->row_y[mid] <= off)
			lo = mid;
		else
			hi = mid - 1;
	}
	return lo;
}

static int page_top(const docwin *w, int i)
{
	return -(w->top + GAP + row_start(w, row_of(w, i)));
}

static int page_left(const docwin *w, int i)
{
	int middle = w->left + (w->ext_w - w->left) / 2;
	if (w->cols == 1)
		return middle - slot_w(w, i) / 2;
	/* Two across: the left-hand page ends, and the right-hand one starts,
	 * half a gap from the middle. */
	return (i + lead(w)) % 2 == 0 ? middle - GAP / 2 - slot_w(w, i) : middle + GAP / 2;
}

static const char *leafname(const char *path)
{
	const char *dot = strrchr(path, '.');
	const char *colon = strrchr(path, ':');
	const char *p = dot > colon ? dot : colon;
	return p ? p + 1 : path;
}

static void fill_rect(unsigned int bbggrr00, int x0, int y0, int x1, int y1)
{
	_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), bbggrr00, 0, 0);
	_swix(OS_Plot, _INR(0, 2), 4, x0, y0);
	_swix(OS_Plot, _INR(0, 2), 96 + 5, x1 - 1, y1 - 1);
}

static unsigned int page_colour(const docwin *w)
{
	return w->mode == FOLIO_PAGE_DARK ? 0x00000000u :
		w->mode == FOLIO_PAGE_SEPIA ? 0xd8ecf400u : 0xffffff00u;
}

static unsigned int gutter_colour(const docwin *w)
{
	return w->mode == FOLIO_PAGE_DARK ? 0x30303000u : 0xb0b0b000u;
}

/* ---- Page sprite cache ------------------------------------------------- */

static void cache_drop(cache_entry *c)
{
	if (c->area) {
		cache_bytes -= c->bytes;
		free(c->area);
	}
	memset(c, 0, sizeof *c);
}

static void cache_forget(docwin *w)
{
	int i;
	for (i = 0; i < CACHE_SLOTS; i++)
		if (cache[i].w == w)
			cache_drop(&cache[i]);
}

static cache_entry *cache_find(docwin *w, int page)
{
	int i;
	for (i = 0; i < CACHE_SLOTS; i++)
		if (cache[i].w == w && cache[i].page == page && cache[i].gen == w->gen && cache[i].mode == w->mode && cache[i].area &&
				cache[i].w_px == px_w(w, page) && cache[i].h_px == px_h(w, page)) {
			cache[i].age = ++cache_clock;
			return &cache[i];
		}
	return NULL;
}

static int on_screen(cache_entry *c);

/* Makes room for 'bytes' more and returns a free slot. Pages on screen are
 * never thrown out to make room: when they alone are more than the budget,
 * it goes over it, rather than drawing them in turn for ever (each new one
 * pushing out another that's showing: the flicker of test26). */
static cache_entry *cache_slot(size_t bytes)
{
	int i;
	cache_entry *free_slot = NULL;
	for (;;) {
		cache_entry *oldest = NULL;
		for (i = 0; i < CACHE_SLOTS; i++) {
			if (!cache[i].area) {
				if (!free_slot)
					free_slot = &cache[i];
			} else if ((!oldest || cache[i].age < oldest->age) && !on_screen(&cache[i]))
				oldest = &cache[i];
		}
		if (free_slot && cache_bytes + bytes <= CACHE_BYTES)
			return free_slot;
		if (!oldest)
			return free_slot;
		cache_drop(oldest);
		if (!free_slot)
			free_slot = oldest;
	}
}

/* A sprite area for one page at w's size (PRM p1177-1178): the worker
 * draws into its pixels. */
static int *new_area(int px_w, int px_h, size_t *bytes_out)
{
	size_t img = (size_t)px_w * 4 * px_h;
	size_t bytes = 16 + 44 + img;
	int dpi = 180 >> app.xeig;
	int *area = malloc(bytes), *spr;
	if (!area)
		return NULL;
	area[0] = (int)bytes;
	area[1] = 1;
	area[2] = 16;
	area[3] = (int)bytes;
	spr = area + 4;
	spr[0] = (int)(44 + img);
	memset(spr + 1, 0, 12);
	strcpy((char *)(spr + 1), "page");
	spr[4] = px_w - 1;
	spr[5] = px_h - 1;
	spr[6] = 0;
	spr[7] = 31;
	spr[8] = 44;
	spr[9] = 44;
	/* RISC OS 3.5 mode word: type 6 (32bpp), dpi matching the screen. */
	spr[10] = (6 << 27) | (dpi << 14) | (dpi << 1) | 1;
	*bytes_out = bytes;
	return area;
}

static int *new_page_area(docwin *w, int page, size_t *bytes_out)
{
	return new_area(px_w(w, page), px_h(w, page), bytes_out);
}

/* Hands thumbnail 'page' of w to the worker. */
static int submit_thumb(docwin *w, int page, int px_w, int px_h, float ppp, int gen)
{
	render_job j;
	memset(&j, 0, sizeof j);
	if (folio_loc_from_number(w->doc, page, &j.loc))
		return 0;
	j.area = new_area(px_w, px_h, &j.bytes);
	if (!j.area)
		return 0;
	j.kind = JOB_THUMB;
	j.owner = w;
	j.fc = app.fc;
	j.doc = w->doc;
	j.page = page;
	j.gen = gen;
	j.mode = w->mode;
	j.ppp = ppp;
	j.px_w = px_w;
	j.px_h = px_h;
	if (worker_submit(&j)) {
		free(j.area);
		return 0;
	}
	return 1;
}

/* Hands page 'page' of w to the worker. */
static int submit_page(docwin *w, int page)
{
	render_job j;
	memset(&j, 0, sizeof j);
	if (folio_loc_from_number(w->doc, page, &j.loc))
		return 0;      /* not counted yet */
	j.area = new_page_area(w, page, &j.bytes);
	if (!j.area) {
		/* Too big to draw whole: don't keep trying. */
		app_log("page %d: no memory for a %dx%d sprite", page + 1, px_w(w, page), px_h(w, page));
		w->fail_page = page;
		w->fail_gen = w->gen;
		return 0;
	}
	j.owner = w;
	j.fc = app.fc;
	j.doc = w->doc;
	j.page = page;
	j.gen = w->gen;
	j.mode = w->mode;
	j.ppp = px_per_pt(w);
	j.links = app.ch.underline_links;
	j.px_w = px_w(w, page);
	j.px_h = px_h(w, page);
	if (worker_submit(&j)) {
		free(j.area);
		return 0;
	}
	return 1;
}

static int is_window(docwin *w);
static void tb_update(docwin *w);
static void abort_for(docwin *w);
static void sel_draw(docwin *w, int i, int x0, int y1);
static void spk_draw(docwin *w, int i, int x0, int y1);
static void asel_draw(docwin *w, int i, int x0, int y1);
static void sel_eor_change(docwin *w, int page, const folio_quad *oldq, int n_old,
		const folio_quad *newq, int n_new);
static folio_quad old_q[SEL_QUADS];   /* a page's selection as last drawn */
static void markup(docwin *w, folio_annot_kind kind);
static void new_note(docwin *w);
static void undo_redo(docwin *w, int redo);
static void sel_forget(docwin *w);
static void security_on_open(docwin *w);
/* A link out of the document waiting for the reader's answer. */
typedef struct {
	docwin *w;
	char uri[512];
	char path[512];          /* a document to open, or "" */
	int page;                /* its page (from #page=N), or -1 */
} pending_link;
static pending_link pend;
static int sel_drag_tick(docwin *w);
static void drag_start(docwin *w);
static void drag_stop(docwin *w);
static int current_page(docwin *w);
static void go_to_page(docwin *w, int i);
static void rotate(docwin *w, int code);
static void colour_chosen(docwin *w, int code);
static void meas_draw(docwin *w, int i, int x0, int y1);
static void pen_draw(docwin *w, int i, int x0, int y1);
static int pen_tick(docwin *w);
static void pen_commit(docwin *w);
static void place_pending(docwin *w, int page, folio_point pt);
static void ocr_embed(docwin *w);
static void remeasure(docwin *w);
static void form_changed(docwin *w);
static void say_if_signed(const char *what);
static void page_changed(docwin *w, int page);
static void link_press(docwin *w, int *b);
static int link_tick(docwin *w);
static void go_back(docwin *w);

/* A finished page from the worker: into the cache, and onto the screen. */
static void take_result(render_job *r)
{
	docwin *w = r->owner;
	cache_entry *c;
	if (bookshelf_owns_job(r)) {
		bookshelf_take(r);
		return;
	}
	if (r->kind == JOB_OCR) {
		if (is_window(w))
			ocr_win_take(w->ocr, r);
		return;
	}
	if (r->kind == JOB_THUMB) {
		if (is_window(w) && r->mode == w->mode && !r->aborted)
			thumbs_take(w->thumbs, r->page, r->gen, r->area, r->ok);
		else
			free(r->area);
		return;
	}
	if (!is_window(w) || r->gen != w->gen || r->mode != w->mode || !r->ok) {
		if (is_window(w) && !r->ok && !r->aborted) {
			app_log("page %d: %s", r->page, r->err);
			w->fail_page = r->page;
			w->fail_gen = r->gen;
		}
		free(r->area);
		return;
	}
	app_log("page %d (%d/%d) mode %d zoom %.2f: %dx%d px in %d cs", r->page, r->loc.chapter,
		r->loc.page, r->mode, w->zoom, r->px_w, r->px_h, r->cs);
	c = cache_slot(r->bytes);
	if (!c) {
		free(r->area);
		return;
	}
	c->w = w;
	c->page = r->page;
	c->gen = r->gen;
	c->mode = r->mode;
	c->age = ++cache_clock;
	c->area = r->area;
	c->w_px = r->px_w;
	c->h_px = r->px_h;
	c->bytes = r->bytes;
	cache_bytes += r->bytes;
	{
		int y1 = page_top(w, r->page), x0 = page_left(w, r->page);
		_swix(Wimp_ForceRedraw, _INR(0, 4), w->wimp, x0, y1 - slot_h(w, r->page), x0 + slot_w(w, r->page), y1);
	}
}

/* ---- What the author allows ------------------------------------------- */

/* Core lock held. */
static void read_perms(docwin *w)
{
	w->perm_print = folio_permitted(w->doc, FOLIO_PERM_PRINT);
	w->perm_copy = folio_permitted(w->doc, FOLIO_PERM_COPY);
	w->perm_access = folio_permitted(w->doc, FOLIO_PERM_ACCESSIBILITY) || w->perm_copy;
	w->perm_form = folio_permitted(w->doc, FOLIO_PERM_FORM);
	w->perm_assemble = folio_permitted(w->doc, FOLIO_PERM_ASSEMBLE);
	w->xfa = folio_xfa_kind(w->doc);
	if (!w->perm_print || !w->perm_copy)
		app_log("%s: the author allows%s%s%s", leafname(w->path), w->perm_print ? " printing" : "",
			w->perm_copy ? " copying" : "", w->perm_access ? " reading aloud" : "");
}

/* Whether w may do what event asks (printing, copying, exporting, reading
 * aloud): with Choices > Obey document restrictions, a PDF's permissions
 * decide. If not, says so (offering the owner password) and returns 0. */
static int allowed(docwin *w, int event)
{
	const char *what = NULL;
	if (!app.ch.obey_restrictions)
		return 1;
	switch (event) {
	case EV_PRINT:
		if (!w->perm_print)
			what = "printing";
		break;
	case EV_COPY:
	case EV_EXPORT_DRAW:
	case EV_SAVE_PAGES:
	case EV_SAVE_PICTURES:
	case EV_SAVE_TEXT:
		if (!w->perm_copy)
			what = "copying its text and pictures";
		break;
	case EV_READ_ALOUD:
	case EV_SPEAK_SEL:
		if (!w->perm_access)
			what = "its text to be read aloud";
		break;
	case EV_FILL_FORM:
		if (!w->perm_form)
			what = "its form to be filled in";
		break;
	case EV_PG_DELETE: case EV_PG_MOVE: case EV_PG_INSERT:
	case EV_ROT_CW: case EV_ROT_ACW: case EV_ROT_180: case EV_ROTALL_CW: case EV_ROTALL_ACW:
		if (!w->perm_assemble)
			what = "its pages to be turned, deleted, moved or added to";
		break;
	}
	if (!what)
		return 1;
	app_log("%s: not allowed: %s", leafname(w->path), what);
	passui_restricted(w, what, event);
	return 0;
}

int docwin_allows(docwin *w, int event)
{
	if (!app.ch.obey_restrictions)
		return 1;
	switch (event) {
	case EV_PRINT: return w->perm_print;
	case EV_COPY: case EV_EXPORT_DRAW: case EV_SAVE_PAGES: case EV_SAVE_PICTURES: case EV_SAVE_TEXT:
		return w->perm_copy;
	case EV_READ_ALOUD: case EV_SPEAK_SEL: return w->perm_access;
	case EV_FILL_FORM: return w->perm_form;
	case EV_PG_DELETE: case EV_PG_MOVE: case EV_PG_INSERT:
	case EV_ROT_CW: case EV_ROT_ACW: case EV_ROT_180: case EV_ROTALL_CW: case EV_ROTALL_ACW:
		return w->perm_assemble;
	}
	return 1;
}

/* ---- Layout ------------------------------------------------------------ */

static void set_extent(docwin *w)
{
	int box[4];
	int exact;
	w->pages = folio_page_count(w->doc, &exact);
	if (w->pages < 1)
		w->pages = 1;
	/* Each row as tall as its tallest page. */
	{
		int r, n = rows(w), y = 0;
		if (w->nrow_y < n + 1) {
			int *p = realloc(w->row_y, (n + 1 + 64) * sizeof *p);
			if (p) {
				w->row_y = p;
				w->nrow_y = n + 1 + 64;
			}
		}
		for (r = 0; r < n && r + 1 < w->nrow_y; r++) {
			int i, h = 0;
			w->row_y[r] = y;
			for (i = first_in_row(w, r); i <= last_in_row(w, r); i++)
				if (slot_h(w, i) > h)
					h = slot_h(w, i);
			y += h + GAP;
		}
		if (r < w->nrow_y)
			w->row_y[r] = y;
		w->ext_h = w->top + GAP + y;
	}
	thumbs_layout(w->thumbs, w->page.x1, w->page.y1, w->pages, w->mode);
	box[0] = 0;
	box[1] = -w->ext_h;
	box[2] = w->ext_w;
	box[3] = 0;
	_swix(Wimp_SetExtent, _INR(0, 1), w->wimp, box);
}

static void layout(docwin *w)
{
	formui_commit(w);         /* the field being typed in moves */
	float widest = w->max_pw > w->page.x1 ? w->max_pw : w->page.x1;
	int sw = (int)(widest * px_per_pt(w) + 0.5f) << app.xeig;
	w->ext_w = w->cols * sw + (w->cols + 1) * GAP;
	if (w->full) {
		/* The pages in the middle of the screen, the work area at least as
		 * wide as it (the Wimp won't make a window wider than that). */
		w->left = w->ext_w < app.screen_w ? (app.screen_w - w->ext_w) / 2 : 0;
		w->ext_w += w->left;
		if (w->ext_w < app.screen_w)
			w->ext_w = app.screen_w;
	} else {
		if (w->ext_w < 640)
			w->ext_w = 640;
		w->ext_w += w->left;
	}
	set_extent(w);
	w->gen++;
}

/* Window state: block[0] handle, [1..4] visible area, [5,6] scroll. */
static void get_state(docwin *w, int *st)
{
	st[0] = w->wimp;
	_swix(Wimp_GetWindowState, _IN(1), st);
}

/* The page under the top of the visible area (the first in its row). */
static int current_page(docwin *w)
{
	int st[9], r;
	get_state(w, st);
	r = row_at(w, -st[6] - GAP / 2);
	if (r >= rows(w)) r = rows(w) - 1;
	return first_in_row(w, r);
}

/* The first page of the row after (or before) the one being read. */
static int row_step(docwin *w, int by)
{
	int r = row_of(w, current_page(w)) + by;
	if (r < 0) r = 0;
	if (r >= rows(w)) r = rows(w) - 1;
	return first_in_row(w, r);
}

static void scroll_to(docwin *w, int y)
{
	int st[9];
	get_state(w, st);
	if (y != st[6])
		formui_commit(w);     /* the page moves from under the field */
	st[6] = y;
	_swix(Wimp_OpenWindow, _IN(1), st);
}

static void go_to_page(docwin *w, int i)
{
	if (i < 0) i = 0;
	if (i >= w->pages) i = w->pages - 1;
	scroll_to(w, page_top(w, i) + GAP / 2 + w->top);
}

static void redraw_all(docwin *w)
{
	_swix(Wimp_ForceRedraw, _INR(0, 4), w->wimp, 0, -w->ext_h, w->ext_w, 0);
}

/* Zoom about the current page. */
static void set_zoom(docwin *w, float z)
{
	int page;
	if (z < MIN_ZOOM) z = MIN_ZOOM;
	if (z > MAX_ZOOM) z = MAX_ZOOM;
	if (z == w->zoom)
		return;
	page = current_page(w);
	w->zoom = z;
	abort_for(w);
	cache_forget(w);
	layout(w);
	go_to_page(w, page);
	redraw_all(w);
	tb_update(w);
}

/* ---- Accelerator files ------------------------------------------------- */

static unsigned hash_string(const char *s)
{
	unsigned h = 2166136261u;
	while (*s)
		h = (h ^ (unsigned char)*s++) * 16777619u;
	return h;
}

/* ---- Choices ------------------------------------------------------------ */

static void choices_set_zoom(float z)
{
	app.ch.zoom = z;
	choices_save();
	app_log("choices: new documents at %d%%", (int)(z * 100 + 0.5f));
}

static void make_accel_path(docwin *w)
{
	const char *cw = getenv("Choices$Write");
	folio_layout lay;
	char dir[200];
	w->accel[0] = 0;
	if (!cw || !*cw)
		return;
	snprintf(dir, sizeof dir, "%s.Folio", cw);
	_swix(OS_File, _INR(0, 1) | _IN(4), 8, dir, 0);
	snprintf(dir, sizeof dir, "%s.Folio.Layouts", cw);
	_swix(OS_File, _INR(0, 1) | _IN(4), 8, dir, 0);
	folio_default_layout(&lay);
	snprintf(w->accel, sizeof w->accel, "%s.L%08x%08x", dir, hash_string(w->path),
		folio_layout_key(&lay));
}

/* ---- Reading positions ------------------------------------------------- */
/* <Choices$Write>.Folio.Places.P<hash of the path>: a small text file,
 * "folio-place 2", then chapter and fraction, then the page's first words,
 * then "view <zoom> <cols> <cover> <x0> <y0> <x1> <y1>": the scale, pages
 * across and the window's place on the screen, so a document (from the
 * Bookshelf or anywhere) opens as it was left (Chris, test34). "folio-place
 * 1" files have no view line. Read back through Choices: (Style Guide p92). */

static void make_place_path(docwin *w, char *out, size_t size, int for_writing)
{
	if (for_writing) {
		const char *cw = getenv("Choices$Write");
		char dir[200];
		out[0] = 0;
		if (!cw || !*cw)
			return;
		snprintf(dir, sizeof dir, "%s.Folio", cw);
		_swix(OS_File, _INR(0, 1) | _IN(4), 8, dir, 0);
		snprintf(dir, sizeof dir, "%s.Folio.Places", cw);
		_swix(OS_File, _INR(0, 1) | _IN(4), 8, dir, 0);
		snprintf(out, size, "%s.P%08x", dir, hash_string(w->path));
	} else {
		snprintf(out, size, "Choices:Folio.Places.P%08x", hash_string(w->path));
	}
}

static void load_place(docwin *w)
{
	char path[256], line[128];
	FILE *f;
	make_place_path(w, path, sizeof path, 0);
	f = fopen(path, "r");
	if (!f)
		return;
	if (fgets(line, sizeof line, f) && (!strncmp(line, "folio-place 1", 13) ||
			!strncmp(line, "folio-place 2", 13)) &&
			fscanf(f, "%d %f\n", &w->mark.chapter, &w->mark.fraction) == 2) {
		float z;
		int c, cv, r[4];
		if (!fgets(w->mark.text, sizeof w->mark.text, f))
			w->mark.text[0] = 0;
		w->mark.text[strcspn(w->mark.text, "\n")] = 0;
		w->pending_mark = 1;
		if (fgets(line, sizeof line, f) &&
				sscanf(line, "view %f %d %d %d %d %d %d", &z, &c, &cv, &r[0], &r[1], &r[2], &r[3]) == 7 &&
				z >= MIN_ZOOM && z <= MAX_ZOOM && (c == 1 || c == 2) && r[2] > r[0] && r[3] > r[1]) {
			w->zoom = z;
			w->cols = c;
			w->cover = cv != 0;
			memcpy(w->view, r, sizeof w->view);
			w->have_view = 1;
		}
	}
	fclose(f);
}

/* Called with the core lock held. */
static void save_place(docwin *w)
{
	char path[256];
	folio_loc loc;
	folio_mark m;
	FILE *f;
	int page = current_page(w);
	if (w->pending_mark || !w->doc)
		return;          /* never got there: keep the old position */
	if (folio_loc_from_number(w->doc, page, &loc) || folio_make_mark(w->doc, loc, &m))
		return;
	make_place_path(w, path, sizeof path, 1);
	if (!path[0] || !(f = fopen(path, "w")))
		return;
	{
		int st[9];
		get_state(w, st);
		if (w->full) {
			/* Kept as the window was before full screen. */
			st[1] = w->fs_box[0];
			st[2] = w->fs_box[1];
			st[3] = w->fs_box[2];
			st[4] = w->fs_box[3];
		}
		fprintf(f, "folio-place 2\n%d %.5f\n%s\nview %.4f %d %d %d %d %d %d\n", m.chapter, m.fraction,
			m.text, w->zoom, w->cols, w->cover, st[1], st[2], st[3], st[4]);
	}
	fclose(f);
	_swix(OS_File, _INR(0, 2), 18, path, 0xfff);
}

/* If the worker is on a page of w, stop it: it's out of date. */
static void abort_for(docwin *w)
{
	const render_job *cur = worker_current();
	/* Recognising text doesn't depend on the zoom or the colours. */
	if (cur && cur->owner == w && cur->kind != JOB_OCR)
		folio_abort(w->doc);
}

/* ---- Window template --------------------------------------------------- */

static tbx_shortcut shortcuts[] = {
	{ 0, 14, EV_DARK, 0 },          /* ^N */
	{ 0, 0x19e, EV_NEXT, 0 },       /* Page Down */
	{ 0, 0x19f, EV_PREV, 0 },       /* Page Up */
	{ 0, 0x1e, EV_FIRST, 0 },       /* Home */
	{ 0, 0x18b, EV_LAST, 0 },       /* End (Copy) */
	{ 0, 0x18e, EV_LINE_DOWN, 0 },  /* Down */
	{ 0, 0x18f, EV_LINE_UP, 0 },    /* Up */
	{ 0, 0x1a2, EV_CLOSE, 0 },      /* ^F2 */
	{ 0, 3, EV_COPY, 0 },           /* ^C */
	{ 0, 1, EV_SELECT_ALL, 0 },     /* ^A */
	{ 0, 27, EV_CLEAR_SEL, 0 },     /* Escape: clear the selection (DESIGN §3.8) */
	{ 0, 0x184, EV_FIND, 0 },       /* F4: Find (as in Edit, SG p60) */
	{ 0, 26, EV_UNDO, 0 },          /* ^Z: undo (DESIGN §3.8) */
	{ 0, 25, EV_REDO, 0 },          /* ^Y: redo */
	{ 0, 0x7f, EV_DEL_ANNOT, 0 },   /* Delete: the selected annotation */
	{ 0, 0x183, EV_SAVE, 0 },       /* F3: Save (SG p84) */
	{ 0, 0x1ac, EV_BACK, 0 },       /* Ctrl-Left: back from a link */
	{ 0, 0x180, EV_PRINT, 0 },      /* Print (SG p84) */
	{ 0, 0x185, EV_GOTO, 0 },       /* F5: Go to page */
	{ 0, 0x1cb, EV_FULL_SCREEN, 0 },/* F11: full screen (F12 is the OS's) */
	{ 0, 18, EV_ROT_CW, 0 },        /* ^R: turn the page under the pointer clockwise */
	{ 0, 12, EV_ROT_ACW, 0 },       /* ^L: and anticlockwise */
};

static ObjectId create_window(docwin *w)
{
	static tbx_window t;
	ObjectId id = 0;
	_kernel_oserror *e;
	memset(&t, 0, sizeof t);
	/* Opening and closing are ours: opening to keep the toolbar as wide as
	 * the window, closing because Adjust also opens the directory. */
	t.flags = 0;
	t.help_message = "This window shows the document. Scroll to move through it; use Page Up and Page Down to move a page at a time.|MPress Menu for view and page options.";
	t.max_help = 200;
	t.num_shortcuts = sizeof shortcuts / sizeof shortcuts[0];
	t.shortcuts = shortcuts;
	t.default_focus = -2;  /* the window takes the input focus, for the keys */
	t.window.vis_x0 = 0;
	t.window.vis_y0 = 0;
	t.window.vis_x1 = 800;
	t.window.vis_y1 = 600;
	t.window.behind = -1;
	/* Moveable; back, close, title, toggle size, scroll bars, adjust size;
	 * new-format flags. Not auto-redraw: the pages are ours to draw. */
	t.window.window_flags = 0x80000002u | (1u << 24) | (1u << 25) | (1u << 26) |
		(1u << 27) | (1u << 28) | (1u << 29) | (1u << 30);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 255;   /* no Wimp fill: the redraw covers everything */
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_x0 = 0;
	t.window.work_y0 = -600;
	t.window.work_x1 = 800;
	t.window.work_y1 = 0;
	t.window.title_flags = 0x00000119;  /* text, centred, indirected */
	t.window.work_flags = 10 << 12;     /* button type: click, double-click, drag */
	t.window.sprite_area = (void *)1;
	t.window.title_text = w->title;
	t.window.title_validation = "";
	t.window.title_buflen = sizeof w->title;
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	return id;
}

/* ---- Open and close ---------------------------------------------------- */

static void wipe(char *p, size_t n)
{
	volatile char *v = p;
	while (n--)
		*v++ = 0;
}

docwin *docwin_open(const char *path)
{
	return docwin_open_password(path, NULL);
}

docwin *docwin_open_password(const char *path, const char *password)
{
	docwin *w;
	int st[9], vis_w, vis_h, x0, y1;

	/* Already open: bring it to the front (Style Guide p22). */
	for (w = windows; w; w = w->next)
		if (!strcmp(w->path, path)) {
			tbx_show(w->obj, 0, 0, NULL, 0, -1);
			return w;
		}

	w = calloc(1, sizeof *w);
	if (!w) {
		tbx_report("Not enough memory to open the document.");
		return NULL;
	}
	snprintf(w->path, sizeof w->path, "%s", path);
	snprintf(w->title, sizeof w->title, "%s", leafname(path));
	w->zoom = app.ch.zoom > 0 ? app.ch.zoom : 1.0f;
	w->mode = app.ch.mode;
	w->cols = app.ch.cols == 2 ? 2 : 1;
	w->cover = 1;
	w->goto_page = -1;
	w->menu_page = -1;

	_swix(Hourglass_On, 0);
	make_accel_path(w);
	/* The core is the worker's while it draws: stop it, then take the lock. */
	worker_stop_for(NULL);
	core_lock();
	w->doc = folio_open_password(app.fc, path, w->accel[0] ? w->accel : NULL, password);
	if (!w->doc) {
		char msg[300];
		int code = folio_error_code(app.fc);
		core_unlock();
		_swix(Hourglass_Off, 0);
		free(w);
		if (code == FOLIO_ERR_PASSWORD || code == FOLIO_ERR_WRONG_PASSWORD) {
			/* Ask, then come back here with what was typed. */
			app_log("%s needs a password%s", leafname(path),
				code == FOLIO_ERR_WRONG_PASSWORD ? " (the one given wasn't right)" : "");
			passui_ask_open(path, code == FOLIO_ERR_WRONG_PASSWORD);
			return NULL;
		}
		snprintf(msg, sizeof msg, "Folio can't open %s: %s", leafname(path), folio_error(app.fc));
		tbx_report(msg);
		return NULL;
	}
	if (password)
		snprintf(w->password, sizeof w->password, "%s", password);
	w->reflow = folio_is_reflowable(w->doc);
	w->can_annotate = folio_can_annotate(w->doc);
	read_perms(w);
	w->asel_page = -1;
	w->meas_page = -1;
	w->pen_page = -1;
	/* Count the first chapter now, so page 1 can be shown at once; the rest
	 * are counted in null polls. */
	w->counting = folio_count_step(w->doc, 1) == 0;
	if (folio_page_bounds(w->doc, (folio_loc){ 0, 0 }, &w->page)) {
		w->page.x0 = w->page.y0 = 0;
		w->page.x1 = 595;
		w->page.y1 = 842;
	}
	core_unlock();
	_swix(Hourglass_Off, 0);
	w->fail_page = -1;
	load_place(w);

	w->obj = create_window(w);
	if (!w->obj) {
		core_lock();
		folio_close(w->doc);
		core_unlock();
		free(w);
		return NULL;
	}
	window_get_wimp_handle(w->obj, &w->wimp);
	tbx_set_handle(w->obj, w);
	window_set_menu(w->obj, app.doc_menu);
	w->toolbar = toolbar_create(w);
	w->thumbs = thumbs_new(w);
	w->show_toolbar = w->toolbar != 0 && app.ch.toolbar;
	if (w->show_toolbar)
		toolbar_attach(w->obj, w->toolbar, 1);
	w->top = w->show_toolbar ? toolbar_height() : 0;
	layout(w);
	core_lock();
	w->ocr = ocr_win_new(w);
	core_unlock();

	/* Style Guide p105: the first window centred, at most a quarter of the
	 * screen; each later one 48 OS units lower. */
	vis_w = w->ext_w + 40;
	if (vis_w > app.screen_w / 2)
		vis_w = app.screen_w / 2;
	/* But never narrower than the toolbar's buttons, so all of them show
	 * (Read aloud too, with the plugin); narrower still if the screen is,
	 * leaving room for the scroll bar. If the reader makes it narrower,
	 * the toolbar scrolls. */
	if (w->show_toolbar && vis_w < toolbar_width())
		vis_w = toolbar_width();
	if (vis_w > app.screen_w - 48)
		vis_w = app.screen_w - 48;
	vis_h = app.screen_h / 2;
	x0 = (app.screen_w - vis_w) / 2;
	y1 = (app.screen_h + vis_h) / 2 - 48 * (open_count % 6);
	open_count++;
	/* Where it was when last read, kept on the screen. */
	if (w->have_view) {
		vis_w = w->view[2] - w->view[0];
		vis_h = w->view[3] - w->view[1];
		if (vis_w > app.screen_w - 48)
			vis_w = app.screen_w - 48;
		if (vis_h > app.screen_h - 120)
			vis_h = app.screen_h - 120;
		if (vis_w < 400)
			vis_w = 400;
		if (vis_h < 300)
			vis_h = 300;
		x0 = w->view[0];
		y1 = w->view[3];
		if (x0 < 0)
			x0 = 0;
		if (x0 + vis_w > app.screen_w)
			x0 = app.screen_w - vis_w;
		if (y1 > app.screen_h - 40)
			y1 = app.screen_h - 40;
		if (y1 - vis_h < 0)
			y1 = vis_h;
		app_log("%s: as last read: %d%%, %d across, %dx%d at %d,%d", w->title,
			(int)(w->zoom * 100 + 0.5f), w->cols, vis_w, vis_h, x0, y1);
	}
	st[0] = w->wimp;
	st[1] = x0;
	st[2] = y1 - vis_h;
	st[3] = x0 + vis_w;
	st[4] = y1;
	st[5] = 0;
	st[6] = 0;
	st[7] = -1;
	tbx_show(w->obj, 0, 1, st + 1, 0, -1);
	w->fit_polls = 25;
	tb_update(w);

	w->next = windows;
	windows = w;
	bookshelf_note(w->path, 0, 0, folio_title(w->doc), folio_author(w->doc));
	app_log("opened %s: %s, %d chapter(s), page %gx%g pt", path, folio_format(w->doc),
		folio_chapters(w->doc), w->page.x1, w->page.y1);
	security_on_open(w);
	signui_on_open(w);
	/* An XFA form MuPDF can't draw: its fields as a list. */
	if (w->xfa == FOLIO_XFA_DYNAMIC) {
		app_log("%s: a dynamic XFA form; its fields are listed", leafname(w->path));
		xfaui_open(w);
	}
	return w;
}

void docwin_close(docwin *w)
{
	docwin **p;
	for (p = &windows; *p; p = &(*p)->next)
		if (*p == w) {
			*p = w->next;
			break;
		}
	drag_stop(w);
	sel_forget(w);
	formui_forget(w);
	if (pend.w == w)
		pend.w = NULL;
	ask_forget(w);
	find_forget(w);
	gotopage_forget(w);
	docinfo_forget(w);
	contents_forget(w);
	annotlist_forget(w);
	pageorg_forget(w);
	drawui_forget(w);
	picbox_forget(w);
	signbox_forget(w);
	xfaui_forget(w);
	measure_forget(w);
	annotui_forget(w);
	passui_forget(w);
	print_forget(w);
	export_forget(w);
	pagesave_forget(w);
	scanui_forget(w);
	speech_forget(w);
	worker_stop_for(w);
	core_lock();
	save_place(w);
	if (w->doc) {
		int exact = 0, pages = folio_page_count(w->doc, &exact);
		char title[128], author[96];
		snprintf(title, sizeof title, "%s", folio_title(w->doc));
		snprintf(author, sizeof author, "%s", folio_author(w->doc));
		core_unlock();
		bookshelf_note(w->path, current_page(w), exact ? pages : 0, title, author);
		core_lock();
	}
	ocr_win_close(w->ocr);
	w->ocr = NULL;
	folio_close(w->doc);
	core_unlock();
	cache_forget(w);
	/* Not recursively: the document menu is shared by every window. */
	thumbs_free(w->thumbs);
	free(w->pw);
	free(w->ph);
	free(w->row_y);
	free(w->pen_pt);
	tbx_delete(w->obj, 1);
	if (w->toolbar) {
		toolbar_forget(w->toolbar);
		tbx_delete(w->toolbar, 0);
	}
	wipe(w->password, sizeof w->password);
	wipe(w->new_password, sizeof w->new_password);
	free(w);
}

void docwin_close_all(void)
{
	while (windows)
		docwin_close(windows);
}

void docwin_close_request(docwin *w)
{
	int ptr[5];
	/* A drawing not yet kept goes on the page first, so it counts as a
	 * change (and the Discard/Save box asks). */
	if (w->pen_strokes || w->pen_drag)
		pen_commit(w);
	if (w->unsaved_new) {
		dcs_open(w);
		return;
	}
	if (w->can_annotate) {
		int modified;
		core_now();
		modified = folio_is_modified(w->doc);
		core_unlock();
		if (modified) {
			dcs_open(w);      /* Discard / Cancel / Save (Style Guide p86) */
			return;
		}
	}
	_swix(Wimp_GetPointerInfo, _IN(1), ptr);
	/* Adjust: close and open the document's directory (Style Guide p29). */
	if (ptr[2] & 1) {
		char cmd[300];
		const char *dot = strrchr(w->path, '.');
		if (dot) {
			snprintf(cmd, sizeof cmd, "Filer_OpenDir %.*s", (int)(dot - w->path), w->path);
			_swix(OS_CLI, _IN(0), cmd);
		}
	}
	docwin_close(w);
}

docwin *docwin_from_object(ObjectId id)
{
	docwin *w;
	for (w = windows; w; w = w->next)
		if (w->obj == id)
			return w;
	return NULL;
}

ObjectId docwin_object(docwin *w)
{
	return w->obj;
}

docwin *docwin_from_wimp(int handle)
{
	docwin *w;
	for (w = windows; w; w = w->next)
		if (w->wimp == handle)
			return w;
	return NULL;
}

/* ---- Redraw ------------------------------------------------------------- */

void docwin_redraw(docwin *w, int *b)
{
	int more = 0;
	b[0] = w->wimp;
	_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), b, &more);
	while (more) {
		int ox = b[1] - b[5], oy = b[4] - b[6];
		int cx0 = b[7], cy0 = b[8], cx1 = b[9], cy1 = b[10];
		int top_work = cy1 - oy, bottom_work = cy0 - oy;
		int i0, i1, i;
		fill_rect(gutter_colour(w), cx0, cy0, cx1, cy1);
		i0 = row_at(w, -top_work - w->top - GAP);                 /* rows */
		i1 = row_at(w, -bottom_work - w->top - GAP);
		if (i1 >= rows(w)) i1 = rows(w) - 1;
		i0 = first_in_row(w, i0);                                 /* pages */
		i1 = last_in_row(w, i1);
		for (i = i0; i <= i1; i++) {
			int x0 = ox + page_left(w, i), y1 = oy + page_top(w, i);
			int x1 = x0 + slot_w(w, i), y0 = y1 - slot_h(w, i);
			cache_entry *c;
			if (x1 <= cx0 || x0 >= cx1 || y1 <= cy0 || y0 >= cy1)
				continue;
			c = cache_find(w, i);
			if (c) {
				/* OS_SpriteOp 52 (PutSpriteScaled), user area, sprite by pointer. */
				_swix(OS_SpriteOp, _INR(0, 7), 52 + 512, c->area, c->area + 4, x0, y0, 0, 0, 0);
				sel_draw(w, i, x0, y1);
				asel_draw(w, i, x0, y1);
				spk_draw(w, i, x0, y1);
				meas_draw(w, i, x0, y1);
				pen_draw(w, i, x0, y1);
			} else {
				fill_rect(page_colour(w), x0, y0, x1, y1);
				w->want_render = 1;
			}
		}
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}

/* ---- Background work: counting, positions, pages --------------------- */

static int is_window(docwin *w)
{
	docwin *x;
	for (x = windows; x; x = x->next)
		if (x == w)
			return 1;
	return 0;
}

static void after_counting(docwin *w)
{
	int old = w->pages;
	set_extent(w);
	if (w->pages != old)
		redraw_all(w);
	if (!w->counting && w->reflow && w->accel[0])
		folio_save_accel(w->doc, w->accel);
}

/* Go to the saved reading position once the chapters before it are counted
 * (not if the reader has already moved). Called with the core lock held. */
static void resolve_mark(docwin *w)
{
	folio_loc loc;
	int ch, n;
	for (ch = 0; ch < w->mark.chapter; ch++)
		if (folio_chapter_pages(w->doc, ch) < 0)
			return;
	w->pending_mark = 0;
	if (current_page(w) != 0)
		return;
	if (folio_find_mark(w->doc, &w->mark, &loc) == 0 &&
			(n = folio_page_number(w->doc, loc)) >= 0) {
		app_log("%s: back at page %d", w->title, n + 1);
		set_extent(w);
		go_to_page(w, n);
		w->last_page = n;
	}
}

/* The next page this window wants drawn: visible ones first (from the
 * top), then two ahead in the direction of reading. -1 if none. */
static int wanted_page(docwin *w, int visible_only)
{
	int st[9], vis_h, top, bottom, i0, i1, i, k;
	int order[64], n = 0;
	const render_job *cur = worker_current();
	get_state(w, st);
	vis_h = st[4] - st[2];
	top = st[6];
	bottom = top - vis_h;
	i0 = row_at(w, -top - GAP);                /* rows; the toolbar covers w->top of it */
	i1 = row_at(w, -bottom - w->top - GAP);
	if (i1 >= rows(w)) i1 = rows(w) - 1;
	i0 = first_in_row(w, i0);
	i1 = last_in_row(w, i1);
	for (i = i0; i <= i1 && n < 58; i++)
		order[n++] = i;
	for (k = 1; k <= 2 * w->cols && !visible_only; k++)
		order[n++] = w->dir >= 0 ? i1 + k : i0 - k;
	for (k = 0; k < n; k++) {
		folio_loc loc;
		i = order[k];
		if (i < 0 || i >= w->pages)
			continue;
		if (cache_find(w, i))
			continue;
		if (w->fail_page == i && w->fail_gen == w->gen)
			continue;
		if (cur && cur->owner == w && cur->kind == JOB_PAGE && cur->page == i && cur->gen == w->gen)
			continue;
		if (folio_loc_from_number(w->doc, i, &loc))
			continue;
		return i;
	}
	return -1;
}

/* Whether a cached page is one its window is showing now. */
static int on_screen(cache_entry *c)
{
	docwin *w = c->w;
	int st[9], top, bottom, r0, r1;
	if (!is_window(w) || c->gen != w->gen || c->mode != w->mode)
		return 0;
	get_state(w, st);
	top = st[6];
	bottom = top - (st[4] - st[2]);
	r0 = row_at(w, -top - GAP);
	r1 = row_at(w, -bottom - w->top - GAP);
	return c->page >= first_in_row(w, r0) && c->page <= last_in_row(w, r1);
}

/* Moves the layout to what's known of the pages' sizes, keeping the place
 * being read where it is on screen. */
static void relayout_keep(docwin *w)
{
	int st[9], page = current_page(w), off, gen = w->gen;
	get_state(w, st);
	off = st[6] - (page_top(w, page) + GAP / 2 + w->top);
	layout(w);
	w->gen = gen;           /* the sprites of pages that kept their size are good */
	scroll_to(w, page_top(w, page) + GAP / 2 + w->top + off);
	redraw_all(w);
	w->want_render = 1;
}

/* Measures page i (core lock held). 1 if its size isn't what was assumed. */
static int measure(docwin *w, int i)
{
	folio_loc loc;
	float pw, ph, was_w = pt_w(w, i), was_h = pt_h(w, i);
	if (i >= w->nsize) {
		int n = w->pages + 64;
		float *a = realloc(w->pw, n * sizeof *a), *b;
		if (!a)
			return 0;
		w->pw = a;
		b = realloc(w->ph, n * sizeof *b);
		if (!b)
			return 0;
		w->ph = b;
		memset(w->pw + w->nsize, 0, (n - w->nsize) * sizeof *a);
		memset(w->ph + w->nsize, 0, (n - w->nsize) * sizeof *b);
		w->nsize = n;
	}
	if (w->pw[i] > 0)
		return 0;
	if (folio_loc_from_number(w->doc, i, &loc) || folio_page_size(w->doc, loc, &pw, &ph) ||
			pw < 1 || ph < 1) {
		pw = w->page.x1;
		ph = w->page.y1;
	}
	w->pw[i] = pw;
	w->ph[i] = ph;
	if (pw > w->max_pw)
		w->max_pw = pw;
	return (int)(pw * 4) != (int)(was_w * 4) || (int)(ph * 4) != (int)(was_h * 4);
}

/* Page sizes, a few centiseconds at a time: those near the page being read
 * first, then the rest in order. 1 while there are more to measure. */
static int measure_step(docwin *w)
{
	unsigned t0, t;
	int changed = 0, cur, k;
	if (w->reflow || w->counting || w->sizes_next >= w->pages)
		return 0;
	if (!core_trylock())
		return 1;
	_swix(OS_ReadMonotonicTime, _OUT(0), &t0);
	cur = current_page(w);
	for (k = -1; k <= 4; k++)
		if (cur + k >= 0 && cur + k < w->pages)
			changed |= measure(w, cur + k);
	do {
		changed |= measure(w, w->sizes_next);
		w->sizes_next++;
		_swix(OS_ReadMonotonicTime, _OUT(0), &t);
	} while (w->sizes_next < w->pages && t - t0 < 3);
	core_unlock();
	if (changed)
		relayout_keep(w);
	if (w->sizes_next >= w->pages)
		app_log("%s: %d page sizes measured%s", w->title, w->pages,
			w->max_pw > w->page.x1 + 0.5f ? " (some pages wider than the first)" : "");
	return w->sizes_next < w->pages;
}

static int any_dragging(void)
{
	docwin *w;
	for (w = windows; w; w = w->next)
		if (w->dragging || w->pen_drag)
			return 1;
	return 0;
}

int docwin_background(void)
{
	docwin *w;
	render_job r;
	int busy = 0;
	unsigned t0, t;
	/* fontconfig's start (on the first font asked for), for the log. */
	{
		static char last[400];
		const char *st = folio_fonts_status();
		if (strcmp(st, last)) {
			snprintf(last, sizeof last, "%s", st);
			app_log("fonts: %s", st);
		}
	}

	if (worker_take(&r))
		take_result(&r);

	_swix(OS_ReadMonotonicTime, _OUT(0), &t0);
	for (w = windows; w; w = w->next) {
		int cur = current_page(w);
		if (cur != w->last_page) {
			w->dir = cur > w->last_page ? 1 : -1;
			w->last_page = cur;
			contents_page_changed(w);
			thumbs_set_current(w->thumbs, cur, 0);
			tb_update(w);
		}
		int counted = 0;
		if ((w->counting || w->pending_mark) && core_trylock()) {
			/* Up to 4 cs of counting per poll. */
			while (w->counting) {
				int c = folio_count_step(w->doc, 1);
				if (c != 0) {
					if (c < 0)
						app_log("counting: %s", folio_error(app.fc));
					w->counting = 0;
					counted = 1;
				}
				_swix(OS_ReadMonotonicTime, _OUT(0), &t);
				if (t - t0 >= 4)
					break;
			}
			if (w->pending_mark)
				resolve_mark(w);
			after_counting(w);
			core_unlock();
		}
		if (counted) {
			contents_page_changed(w);
			tb_update(w);
		}
		/* A page asked for on opening that wasn't counted yet. */
		if (w->goto_page >= 0 && (w->goto_page < w->pages || !w->counting)) {
			int t = w->goto_page < w->pages ? w->goto_page : w->pages - 1;
			w->goto_page = -1;
			docwin_go_to(w, t);
			app_log("%s: opened at page %d", w->title, t + 1);
		}
		busy |= w->counting || w->pending_mark || w->goto_page >= 0;
		if (!app.speech_urgent)
			busy |= measure_step(w);
	}

	/* Recognising text takes seconds: a page that needs drawing now comes
	 * first (the recognising starts again later). */
	{
		const render_job *cur = worker_current();
		if (cur && cur->kind == JOB_OCR && worker_busy())
			for (w = windows; w; w = w->next)
				if (w->want_render && wanted_page(w, 1) >= 0) {
					/* Not waiting for it: Tesseract can take a while to
					 * notice, and the desktop would wait with it. */
					if (!folio_aborted(cur->doc))
						app_log("ocr: stopped for a page to draw");
					worker_abort_current();
					break;
				}
	}
	/* Give the worker a page if it has none. */
	if (worker_idle()) {
		for (w = windows; w; w = w->next) {
			int page = w->want_render ? wanted_page(w, app.speech_urgent) : -1;
			if (page < 0) {
				w->want_render = 0;
				continue;
			}
			if (submit_page(w, page))
				break;
		}
	}
	/* Then thumbnails for open drawers. */
	if (worker_idle() && !app.speech_urgent) {
		const render_job *cur = worker_current();
		for (w = windows; w; w = w->next) {
			int px_w, px_h, gen, page;
			float ppp;
			if (!thumbs_shown(w->thumbs))
				continue;
			page = thumbs_wanted(w->thumbs, cur && cur->owner == w && cur->kind == JOB_THUMB ? cur->page : -1,
				&px_w, &px_h, &ppp, &gen);
			if (page >= 0 && submit_thumb(w, page, px_w, px_h, ppp, gen))
				break;
		}
	}
	/* Then text to recognise (not while selecting text). */
	if (worker_idle() && !any_dragging())
		for (w = windows; w; w = w->next)
			if (w->ocr && ocr_win_next(w->ocr))
				break;
	/* And some time to work on it. */
	if (!worker_idle()) {
		if (worker_slice(5) && worker_take(&r))
			take_result(&r);
		busy = 1;
	}
	for (w = windows; w; w = w->next) {
		busy |= sel_drag_tick(w);
		busy |= pen_tick(w);
		busy |= link_tick(w);
		busy |= w->want_render;
		/* The toolbar's width: after every event, since the Toolbox shows
		 * and places it on its own schedule (after the window opens, and
		 * again when the drawer comes or goes); for a few null polls more
		 * after those. */
		if (w->show_toolbar) {
			toolbar_fit(w->toolbar, w->wimp);
			if (w->fit_polls > 0) {
				w->fit_polls--;
				busy = 1;
			}
		}
	}
	return busy;
}

void docwin_mode_changed(void)
{
	docwin *w;
	for (w = windows; w; w = w->next) {
		int page = current_page(w);
		abort_for(w);
		cache_forget(w);
		layout(w);
		if (w->full) {
			int st[9];
			get_state(w, st);
			st[7] = -1;
			docwin_open_request(w, st);
		}
		go_to_page(w, page);
		redraw_all(w);
	}
}

/* ---- Text selection (Style Guide p77-82) ------------------------------- */
/* One page at a time for now. Select drag selects, double-click selects a
 * word, Adjust click or drag extends, Select click clears. The selection is
 * shown reversed out (SG p85) by EORing its quads after the page sprite. */

/* Screen point to a page and a point on it (points from the page's top
 * left). Returns -1 if it isn't over a page. */
static int point_at(docwin *w, int sx, int sy, folio_point *pt, int clamp_page)
{
	int st[9], wx, wy, i;
	float os_per_pt = 2.5f * w->zoom;
	get_state(w, st);
	wx = sx - (st[1] - st[5]);
	wy = sy - (st[4] - st[6]);
	if (clamp_page >= 0) {
		i = clamp_page;
	} else {
		/* The row, then the page in it nearest across. */
		int r = row_at(w, -wy - w->top - GAP / 2), j, best = -1;
		if (-wy - w->top - GAP / 2 < 0 || r >= rows(w))
			return -1;
		i = -1;
		for (j = first_in_row(w, r); j <= last_in_row(w, r); j++) {
			int d = wx - (page_left(w, j) + slot_w(w, j) / 2);
			if (d < 0)
				d = -d;
			if (i < 0 || d < best) {
				i = j;
				best = d;
			}
		}
	}
	if (i < 0 || i >= w->pages)
		return -1;
	pt->x = (wx - page_left(w, i)) / os_per_pt;
	pt->y = (page_top(w, i) - wy) / os_per_pt;
	if (clamp_page >= 0) {
		/* Dragging off the page: hold to its edges. */
		if (pt->x < 0) pt->x = 0;
		if (pt->y < 0) pt->y = 0;
		if (pt->x > pt_w(w, i)) pt->x = pt_w(w, i);
		if (pt->y > pt_h(w, i)) pt->y = pt_h(w, i);
	} else if (pt->x < 0 || pt->y < 0 || pt->x > pt_w(w, i) || pt->y > pt_h(w, i)) {
		return -1;
	}
	return i;
}

static void redraw_page(docwin *w, int i)
{
	int y1 = page_top(w, i), x0 = page_left(w, i);
	_swix(Wimp_ForceRedraw, _INR(0, 4), w->wimp, x0, y1 - slot_h(w, i), x0 + slot_w(w, i), y1);
}

/* ---- The selection's boxes, page by page -------------------------------------
 * A slot holds what's drawn of a window's selection on one page: the part of
 * the page it covers (s to e) and its boxes. A page showing in its window has
 * a slot while the selection is on it; when it can't be worked out at once
 * (the worker is recognising text, which can't stop quickly) the slot is
 * stale, with nothing drawn, and the next null poll does it. */
#define SEL_SLOTS 40

typedef struct {
	docwin *w;                /* NULL: free */
	int page, n, stale;
	unsigned used;
	folio_point s, e;
	folio_quad q[SEL_QUADS];
} selslot;

static selslot selslots[SEL_SLOTS];
static unsigned sel_clock;

/* The selection's span: from point *s on page *lo to point *e on page *hi. */
static void sel_span(docwin *w, int *lo, int *hi, folio_point *s, folio_point *e)
{
	if (w->sel_end < w->sel_page) {
		*lo = w->sel_end;
		*s = w->sel_b;
		*hi = w->sel_page;
		*e = w->sel_a;
	} else {
		*lo = w->sel_page;
		*s = w->sel_a;
		*hi = w->sel_end;
		*e = w->sel_b;
	}
}

/* The part of page p the selection covers: 1 and *s to *e, or 0. Pages in
 * the middle are covered from their top left to their bottom right. */
static int sel_bounds(docwin *w, int p, folio_point *s, folio_point *e)
{
	int lo, hi;
	folio_point a, b;
	if (!w->sel)
		return 0;
	sel_span(w, &lo, &hi, &a, &b);
	if (p < lo || p > hi)
		return 0;
	/* The first and last characters of the text, not the page's corners:
	 * the character nearest a corner can be a page number or heading that
	 * comes anywhere in the text's order, and the part of the page selected
	 * was then the wrong part, or nothing (test38). */
	s->x = FOLIO_TEXT_START;
	s->y = 0;
	e->x = FOLIO_TEXT_END;
	e->y = 0;
	if (p == lo)
		*s = a;
	if (p == hi)
		*e = b;
	return 1;
}

static selslot *slot_of(docwin *w, int p)
{
	int i;
	for (i = 0; i < SEL_SLOTS; i++)
		if (selslots[i].w == w && selslots[i].page == p)
			return &selslots[i];
	return NULL;
}

/* Is page p rendered and (at least partly) in view? */
static int page_showing(docwin *w, int p)
{
	int st[9], top, bot;
	if (!cache_find(w, p))
		return 0;
	get_state(w, st);
	top = st[6];
	bot = st[6] - (st[4] - st[2]);
	return page_top(w, p) > bot && page_top(w, p) - slot_h(w, p) < top;
}

/* A slot for page p of w: its own, a free one, or the one used longest ago
 * (that page is drawn again, as its boxes are no longer known). */
static selslot *slot_new(docwin *w, int p)
{
	selslot *t = slot_of(w, p), *old = NULL;
	int i;
	if (t)
		return t;
	for (i = 0; i < SEL_SLOTS && !t; i++)
		if (!selslots[i].w)
			t = &selslots[i];
	if (!t) {
		for (i = 0; i < SEL_SLOTS; i++)
			if (!old || selslots[i].used < old->used)
				old = &selslots[i];
		t = old;
		if (is_window(t->w) && page_showing(t->w, t->page))
			redraw_page(t->w, t->page);
	}
	memset(t, 0, offsetof(selslot, q));
	t->w = w;
	t->page = p;
	t->used = ++sel_clock;
	return t;
}

static void sel_forget(docwin *w)
{
	int i;
	for (i = 0; i < SEL_SLOTS; i++)
		if (selslots[i].w == w)
			selslots[i].w = NULL;
}

/* Takes the core to work out boxes. Without wait, not while the worker has
 * a job (0 is returned, and it's tried again on a later poll): stopping
 * recognising can take seconds, and the desktop would stop with it. */
static int sel_lock(int wait)
{
	const render_job *j = worker_current();
	if (!wait && worker_busy()) {
		/* Pages being drawn finish first (stopping them each time only
		 * meant they were never drawn); recognising is asked to stop. */
		if (j && j->kind == JOB_OCR)
			worker_abort_current();
		return 0;
	}
	core_now();
	return 1;
}

/* Works out slot t's boxes for s to e on its page (core held). */
static void slot_fill(docwin *w, selslot *t, folio_point s, folio_point e)
{
	folio_loc loc;
	t->s = s;
	t->e = e;
	t->stale = 0;
	t->n = 0;
	t->used = ++sel_clock;
	if (folio_loc_from_number(w->doc, t->page, &loc) == 0) {
		t->n = folio_selection_quads(w->doc, loc, s, e, t->q, SEL_QUADS);
		if (t->n < 0)
			t->n = 0;
	}
}

static int same_pt(folio_point a, folio_point b)
{
	return a.x == b.x && a.y == b.y;
}

/* Moves the selection's end to point b on page end, changing what's drawn
 * page by page. Returns 0 if it couldn't now (see sel_lock): nothing
 * changes, and the caller tries again. */
static int sel_set_end(docwin *w, int end, folio_point b, int wait)
{
	int lo0, hi0, lo1, hi1, lo, hi, p;
	folio_point s, e;
	if (!w->sel)
		return 1;
	if (!sel_lock(wait))
		return 0;
	sel_span(w, &lo0, &hi0, &s, &e);
	w->sel_end = end;
	w->sel_b = b;
	sel_span(w, &lo1, &hi1, &s, &e);
	lo = lo0 < lo1 ? lo0 : lo1;
	hi = hi0 > hi1 ? hi0 : hi1;
	for (p = lo; p <= hi; p++) {
		selslot *t = slot_of(w, p);
		int n_old = 0, in_new = sel_bounds(w, p, &s, &e);
		if (!page_showing(w, p)) {
			/* Worked out when it's drawn. */
			if (t)
				t->w = NULL;
			continue;
		}
		if (t && !t->stale) {
			if (in_new && same_pt(t->s, s) && same_pt(t->e, e))
				continue;
			n_old = t->n;
			memcpy(old_q, t->q, (size_t)n_old * sizeof old_q[0]);
		}
		if (in_new) {
			if (!t)
				t = slot_new(w, p);
			slot_fill(w, t, s, e);
			if (n_old != t->n || memcmp(old_q, t->q, (size_t)n_old * sizeof old_q[0]))
				sel_eor_change(w, p, old_q, n_old, t->q, t->n);
		} else {
			sel_eor_change(w, p, old_q, n_old, NULL, 0);
			if (t)
				t->w = NULL;
		}
	}
	core_unlock();
	tb_update(w);
	return 1;
}

/* Shows a new selection (sel_page, sel_a to sel_end, sel_b set). */
static void sel_show(docwin *w)
{
	sel_set_end(w, w->sel_end, w->sel_b, 1);
}

/* The boxes of the selection on page p (worked out if need be). */
static int sel_quads_of(docwin *w, int p, const folio_quad **q)
{
	folio_point s, e;
	selslot *t;
	int drawn;
	*q = NULL;
	if (!sel_bounds(w, p, &s, &e))
		return 0;
	t = slot_of(w, p);
	if (t && !t->stale && same_pt(t->s, s) && same_pt(t->e, e)) {
		*q = t->q;
		return t->n;
	}
	drawn = t && !t->stale;
	if (!t)
		t = slot_new(w, p);
	core_now();
	slot_fill(w, t, s, e);
	core_unlock();
	if (!drawn && page_showing(w, p))
		sel_eor_change(w, p, NULL, 0, t->q, t->n);
	*q = t->q;
	return t->n;
}

/* Is anything selected? (Cheap: what's known.) */
static int sel_any(docwin *w)
{
	selslot *t;
	if (!w->sel)
		return 0;
	if (w->sel_page != w->sel_end)
		return 1;
	t = slot_of(w, w->sel_page);
	return !t || t->stale || t->n > 0;
}

/* The first and last boxes of the selection, and their pages. 0 if none. */
static int sel_ends(docwin *w, int *p0, folio_quad *first, int *p1, folio_quad *last)
{
	int lo, hi, p, n;
	folio_point s, e;
	const folio_quad *q;
	if (!w->sel)
		return 0;
	sel_span(w, &lo, &hi, &s, &e);
	for (p = lo; p <= hi; p++)
		if ((n = sel_quads_of(w, p, &q)) > 0) {
			*p0 = p;
			*first = q[0];
			break;
		}
	if (p > hi)
		return 0;
	for (p = hi; p >= lo; p--)
		if ((n = sel_quads_of(w, p, &q)) > 0) {
			*p1 = p;
			*last = q[n - 1];
			break;
		}
	return 1;
}

/* The selected text, page after page (malloc'd), or NULL. */
static char *sel_text(docwin *w)
{
	int lo, hi, p;
	folio_point s, e;
	char *out = NULL;
	size_t len = 0;
	if (!w->sel)
		return NULL;
	sel_span(w, &lo, &hi, &s, &e);
	if (hi > lo)
		app_log("%s: copying pages %d to %d", w->title, lo + 1, hi + 1);
	for (p = lo; p <= hi; p++) {
		folio_loc loc;
		char *t = NULL, *more;
		size_t n;
		if (!sel_bounds(w, p, &s, &e) || folio_loc_from_number(w->doc, p, &loc))
			continue;
		core_now();
		t = folio_selection_text(w->doc, loc, s, e);
		core_unlock();
		if (!t)
			continue;
		n = strlen(t);
		if (n && (more = realloc(out, len + n + 2)) != NULL) {
			out = more;
			if (len && out[len - 1] != '\n')
				out[len++] = '\n';
			memcpy(out + len, t, n);
			len += n;
			out[len] = 0;
		}
		folio_free(w->doc, t);
	}
	return out;
}

static void sel_clear(docwin *w)
{
	int i;
	if (!w->sel)
		return;
	for (i = 0; i < SEL_SLOTS; i++) {
		selslot *t = &selslots[i];
		if (t->w == w) {
			if (!t->stale && page_showing(w, t->page))
				sel_eor_change(w, t->page, t->q, t->n, NULL, 0);
			t->w = NULL;
		}
	}
	w->sel = 0;
	w->sel_stale = 0;
	w->sel_word[0] = 0;
	drag_stop(w);
}

/* EOR the selection over page i (screen origin of the page at x0, y1). */
/* EOR quads over a page whose top left is at x0, y1 (screen). */
static void eor_quads_in(docwin *w, const folio_quad *quads, int count, int x0, int y1,
		unsigned colour)
{
	float k = 2.5f * w->zoom;
	int n;
	_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), colour, 0, 3);
	for (n = 0; n < count; n++) {
		const folio_quad *q = &quads[n];
		float qx0 = q->ul.x < q->ll.x ? q->ul.x : q->ll.x;
		float qx1 = q->ur.x > q->lr.x ? q->ur.x : q->lr.x;
		float qy0 = q->ul.y < q->ur.y ? q->ul.y : q->ur.y;
		float qy1 = q->ll.y > q->lr.y ? q->ll.y : q->lr.y;
		/* Edges rounded the same way on both sides, so a strip between two
		 * boxes (sel_eor_change) meets them exactly. */
		int rx0 = x0 + (int)(qx0 * k + 0.5f), rx1 = x0 + (int)(qx1 * k + 0.5f);
		int ry1 = y1 - (int)(qy0 * k + 0.5f), ry0 = y1 - (int)(qy1 * k + 0.5f);
		/* ...and on whole pixels, so neither covers half of one. */
		rx0 = (rx0 >> app.xeig) << app.xeig;
		rx1 = (rx1 >> app.xeig) << app.xeig;
		ry0 = (ry0 >> app.yeig) << app.yeig;
		ry1 = (ry1 >> app.yeig) << app.yeig;
		if (rx1 <= rx0 || ry1 <= ry0)
			continue;
		_swix(OS_Plot, _INR(0, 2), 4, rx0, ry0);
		_swix(OS_Plot, _INR(0, 2), 96 + 5, rx1 - 1, ry1 - 1);
	}
}

static void eor_quads(docwin *w, const folio_quad *quads, int count, int x0, int y1)
{
	eor_quads_in(w, quads, count, x0, y1, 0xffffff00u);
}

/* The sentence being read: EORed in a warm tint (a pale band on a white
 * page, a dark blue one on a dark page; the text stays readable). */
#define SPK_COLOUR 0x60402000u

static void spk_draw(docwin *w, int i, int x0, int y1)
{
	int k;
	for (k = 0; k < w->spk_n; k++)
		if (w->spk_page[k] == i)
			eor_quads_in(w, &w->spk_q[k], 1, x0, y1, SPK_COLOUR);
}

/* Draws the selection on page i (while redrawing). Its boxes are worked
 * out if they aren't known, unless the worker is recognising text: then
 * the page is marked to have them drawn on a later poll. */
static void sel_draw(docwin *w, int i, int x0, int y1)
{
	folio_point s, e;
	selslot *t;
	if (!sel_bounds(w, i, &s, &e))
		return;
	t = slot_of(w, i);
	if (!t || t->stale || !same_pt(t->s, s) || !same_pt(t->e, e)) {
		if (!t)
			t = slot_new(w, i);
		if (!sel_lock(0)) {
			t->stale = 1;
			t->s = s;
			t->e = e;
			w->sel_stale = 1;
			return;
		}
		slot_fill(w, t, s, e);
		core_unlock();
	}
	eor_quads(w, t->q, t->n, x0, y1);
}

/* Draws what sel_draw had to leave (null polls). */
static void sel_catch_up(docwin *w)
{
	int i;
	if (!w->sel_stale || !sel_lock(0))
		return;
	w->sel_stale = 0;
	for (i = 0; i < SEL_SLOTS; i++) {
		selslot *t = &selslots[i];
		folio_point s, e;
		if (t->w != w || !t->stale)
			continue;
		if (!sel_bounds(w, t->page, &s, &e) || !page_showing(w, t->page)) {
			t->w = NULL;
			continue;
		}
		slot_fill(w, t, s, e);
		sel_eor_change(w, t->page, NULL, 0, t->q, t->n);
	}
	core_unlock();
}

/* Changes what's shown of the selection on page from the quads old to new
 * in place (Wimp_UpdateWindow), EORing the old off and the new on, instead
 * of drawing the whole page again: no flicker while dragging. Only where the
 * page itself is showing (a placeholder gets the selection when it's drawn). */
static void sel_eor_change(docwin *w, int page, const folio_quad *oldq, int n_old,
		const folio_quad *newq, int n_new)
{
	int b[11], more = 0, k = 0;
	/* Only the boxes that differ: the lines both have in common, at the
	 * start and at the end, stay as they are. (EORing them all off and on
	 * again made a selection of whole pages flash at each move: test37.) */
	while (k < n_old && k < n_new && !memcmp(&oldq[k], &newq[k], sizeof oldq[0]))
		k++;
	oldq += k;
	newq += k;
	n_old -= k;
	n_new -= k;
	while (n_old > 0 && n_new > 0 &&
			!memcmp(&oldq[n_old - 1], &newq[n_new - 1], sizeof oldq[0])) {
		n_old--;
		n_new--;
	}
	if ((n_old == 0 && n_new == 0) || !cache_find(w, page))
		return;
	/* One line that only grew or shrank at one end: just the strip between
	 * (EORing the old line off and the new one on blinked it). */
	if (n_old == 1 && n_new == 1 && oldq->ul.y == newq->ul.y && oldq->ll.y == newq->ll.y &&
			oldq->ur.y == newq->ur.y && oldq->lr.y == newq->lr.y &&
			oldq->ul.y == oldq->ur.y && oldq->ll.y == oldq->lr.y &&
			oldq->ul.x == oldq->ll.x && oldq->ur.x == oldq->lr.x &&
			newq->ul.x == newq->ll.x && newq->ur.x == newq->lr.x) {
		static folio_quad strip;
		strip = *oldq;
		if (oldq->ul.x == newq->ul.x) {
			float a = oldq->ur.x, c = newq->ur.x;
			strip.ul.x = strip.ll.x = a < c ? a : c;
			strip.ur.x = strip.lr.x = a < c ? c : a;
		} else if (oldq->ur.x == newq->ur.x) {
			float a = oldq->ul.x, c = newq->ul.x;
			strip.ul.x = strip.ll.x = a < c ? a : c;
			strip.ur.x = strip.lr.x = a < c ? c : a;
		} else {
			goto both;
		}
		oldq = &strip;
		newq = NULL;
		n_new = 0;
	}
both:
	b[0] = w->wimp;
	b[1] = page_left(w, page);
	b[2] = page_top(w, page) - slot_h(w, page);
	b[3] = page_left(w, page) + slot_w(w, page);
	b[4] = page_top(w, page);
	_swix(Wimp_UpdateWindow, _IN(1) | _OUT(0), b, &more);
	while (more) {
		int x0 = b[1] - b[5] + page_left(w, page), y1 = b[4] - b[6] + page_top(w, page);
		eor_quads(w, oldq, n_old, x0, y1);
		eor_quads(w, newq, n_new, x0, y1);
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}

static void asel_click(docwin *w, int *b);
static void asel_clear(docwin *w);

void docwin_click(docwin *w, int *b)
{
	folio_point pt;
	int page, buttons = b[2];
	/* Any click in the window takes the input focus (no caret), so the keys
	 * work (Style Guide p69). Button type 10: click = x256, drag = x16,
	 * double-click = x1; Select = 4, Adjust = 1. */
	if (buttons != 0x400)
		w->link_down = 0;                     /* a drag or double-click: not a link */
	/* The Wimp follows a click with a drag (or a double-click) if the button
	 * is held a moment or clicked again: on the field just opened for
	 * typing, that's the same click, not a new one (test44 closed the
	 * editor and started a selection). */
	if (buttons != 0x400 && (page = point_at(w, b[0], b[1], &pt, -1)) >= 0 &&
			formui_on_field(w, page, pt)) {
		app_log("form: %s on the field being typed in: ignored",
			buttons == 0x40 ? "drag" : buttons == 4 ? "double-click" : "click");
		return;
	}
	_swix(Wimp_SetCaretPosition, _INR(0, 5), w->wimp, -1, 0, 0, 1 << 25, -1);
	formui_commit(w);                        /* a field being typed in: keep it */
	/* A stamp or picture waiting: Select places it, Adjust gives up. */
	if (w->stamp_pending[0] || w->pic_pending[0]) {
		if (buttons == 0x400 && (page = point_at(w, b[0], b[1], &pt, -1)) >= 0)
			place_pending(w, page, pt);
		else if (buttons == 0x100)
			docwin_place_cancel(w);
		return;
	}
	/* Drawing: a Select drag draws a stroke, Adjust takes the last off. */
	if (w->pen_on) {
		if (buttons == 0x40 && (page = point_at(w, b[0], b[1], &pt, -1)) >= 0) {
			if (w->pen_page >= 0 && page != w->pen_page)
				pen_commit(w);
			if (w->pen_strokes < PEN_STROKES && w->pen_n < PEN_POINTS - 1) {
				w->pen_page = page;
				w->pen_count[w->pen_strokes] = 1;
				w->pen_pt[w->pen_n++] = pt;
				w->pen_drag = 1;
				drag_start(w);
			}
		} else if (buttons == 0x100 && w->pen_strokes > 0) {
			w->pen_n -= w->pen_count[--w->pen_strokes];
			redraw_page(w, w->pen_page);
			drawui_changed(w);
		}
		return;
	}
	/* Measuring: Select adds a point, Adjust takes the last off; nothing
	 * else happens on the page meanwhile. */
	if (w->meas_on) {
		if (buttons == 0x400 && (page = point_at(w, b[0], b[1], &pt, -1)) >= 0) {
			if (page != w->meas_page || w->meas_n >= MEASURE_MAX) {
				if (w->meas_page >= 0)
					redraw_page(w, w->meas_page);
				w->meas_page = page;
				w->meas_n = 0;
			}
			w->meas_pt[w->meas_n++] = pt;
			redraw_page(w, page);
			measure_changed(w);
		} else if (buttons == 0x100 && w->meas_n > 0) {
			w->meas_n--;
			redraw_page(w, w->meas_page);
			measure_changed(w);
		}
		return;
	}
	if (buttons == 0x400) {                  /* Select click: a form field, an annotation, a link, or deselect */
		sel_clear(w);
		if (w->can_annotate && (page = point_at(w, b[0], b[1], &pt, -1)) >= 0 &&
				formui_click(w, page, pt, b[0], b[1]))
			return;
		asel_click(w, b);
		if (w->asel_page < 0)
			link_press(w, b);
	} else if (buttons == 0x40) {            /* Select drag: start a selection */
		page = point_at(w, b[0], b[1], &pt, -1);
		if (page < 0)
			return;
		sel_clear(w);
		w->sel = 1;
		w->sel_page = w->sel_end = page;
		w->sel_a = w->sel_b = pt;
		w->dragging = 1;
		/* Nothing to draw yet (it starts empty). Stop any recognising now
		 * (it isn't started again while the drag goes on), so it's done by
		 * the time the drag needs the core; don't wait for it here. */
		worker_abort_current();
		drag_start(w);
		tb_update(w);
	} else if (buttons == 0x100 || buttons == 0x10) { /* Adjust: extend */
		if (!w->sel)
			return;
		page = point_at(w, b[0], b[1], &pt, -1);
		if (page < 0 && (page = point_at(w, b[0], b[1], &pt, w->sel_end)) < 0)
			return;
		w->sel_word[0] = 0;
		w->dragging = buttons == 0x10;
		if (w->dragging)
			drag_start(w);
		sel_set_end(w, page, pt, 1);
	} else if (buttons == 4) {               /* Select double-click: a word */
		folio_loc loc;
		char *word;
		page = point_at(w, b[0], b[1], &pt, -1);
		if (page < 0 || folio_loc_from_number(w->doc, page, &loc))
			return;
		core_now();
		word = folio_word_at(w->doc, loc, pt, &w->sel_a, &w->sel_b);
		core_unlock();
		sel_clear(w);
		if (!word)
			return;
		snprintf(w->sel_word, sizeof w->sel_word, "%s", word);
		folio_free(w->doc, word);
		w->sel = 1;
		w->sel_page = w->sel_end = page;
		sel_show(w);
		app_log("word: %s", w->sel_word);
	}
}

/* A selection drag is a Wimp user drag (an invisible point), so the Wimp
 * says when the button is let go (Wimp_EUserDrag), wherever the pointer is. */
static void drag_start(docwin *w)
{
	int b[10], ptr[5];
	_swix(Wimp_GetPointerInfo, _IN(1), ptr);
	b[0] = w->wimp;
	b[1] = 7;                         /* point drag, nothing drawn */
	b[2] = b[4] = ptr[0];
	b[3] = b[5] = ptr[1];
	b[6] = -0x10000;                  /* anywhere on the screen */
	b[7] = -0x10000;
	b[8] = 0x10000;
	b[9] = 0x10000;
	_swix(Wimp_DragBox, _IN(1), b);
	_swix(OS_ReadMonotonicTime, _OUT(0), &w->drag_cs);
}

static void drag_stop(docwin *w)
{
	if (!w->dragging)
		return;
	w->dragging = 0;
	_swix(Wimp_DragBox, _IN(1), 0);   /* cancel, if it's still going */
	app_log("%s: selection pages %d to %d", w->title, w->sel_page + 1, w->sel_end + 1);
}

/* Wimp_EUserDrag: the button was let go. */
void docwin_user_drag_done(void)
{
	docwin *w;
	for (w = windows; w; w = w->next)
		if (w->pen_drag) {
			pen_tick(w);
			w->pen_drag = 0;
			w->pen_strokes++;
			drawui_changed(w);
		}
	for (w = windows; w; w = w->next)
		if (w->dragging) {
			w->dragging = 0;          /* the Wimp's drag is over already */
			app_log("%s: selection pages %d to %d", w->title, w->sel_page + 1, w->sel_end + 1);
		}
}

/* While dragging (null polls): follow the pointer, onto the pages before
 * and after, scrolling while it's near or past the top or bottom of the
 * window. The scroll goes by time, not by poll: from 1/5 of the window's
 * height a second near the edge to one window a second well past it.
 * Recognising isn't started during a drag, and the boxes of a page are only
 * worked out while the worker has nothing to do (sel_lock), so pages being
 * drawn get drawn, and the desktop keeps going. */
static int sel_drag_tick(docwin *w)
{
	int ptr[5], st[9], page, vis_h, top, bot, d = 0;
	unsigned now, dt;
	folio_point pt;
	sel_catch_up(w);
	if (!w->dragging)
		return w->sel_stale;
	_swix(Wimp_GetPointerInfo, _IN(1), ptr);
	if (!(ptr[2] & 5)) {              /* in case the Wimp's word is lost */
		drag_stop(w);
		return w->sel_stale;
	}
	get_state(w, st);
	_swix(OS_ReadMonotonicTime, _OUT(0), &now);
	dt = now - w->drag_cs;
	if (dt > 10)
		dt = 10;                      /* a slow poll doesn't make a jump */
	vis_h = st[4] - st[2];
	top = st[4] - w->top;             /* below the toolbar */
	bot = st[2];
	if (ptr[1] < bot + 48)
		d = -(bot + 48 - ptr[1]);
	else if (ptr[1] > top - 48)
		d = ptr[1] - (top - 48);
	if (d && dt) {
		int dist = d < 0 ? -d : d;
		/* OS units a second: vis_h/5 at the edge, vis_h at 400 past. */
		int speed = vis_h / 5 + (dist > 448 ? 448 : dist) * (vis_h - vis_h / 5) / 448;
		int step = speed * (int)dt / 100;
		if (step < 4)
			step = 4;
		w->drag_cs = now;
		scroll_to(w, d < 0 ? st[6] - step : (st[6] + step > 0 ? 0 : st[6] + step));
	} else if (!d) {
		w->drag_cs = now;
	}
	page = point_at(w, ptr[0], ptr[1], &pt, -1);
	if (page < 0)
		page = point_at(w, ptr[0], ptr[1], &pt, w->sel_end);
	if (page >= 0 && (page != w->sel_end || !same_pt(pt, w->sel_b)))
		sel_set_end(w, page, pt, 0);
	return 1;
}

static void sel_copy(docwin *w)
{
	char *text = sel_text(w);
	if (text) {
		clip_set_text(text);
		free(text);
	}
}

static void sel_all(docwin *w)
{
	int page = current_page(w);
	sel_clear(w);
	w->sel = 1;
	w->sel_page = w->sel_end = page;
	w->sel_a.x = FOLIO_TEXT_START;
	w->sel_a.y = 0;
	w->sel_b.x = FOLIO_TEXT_END;
	w->sel_b.y = 0;
	sel_show(w);
}

/* The word to define: the double-clicked word, or a selection of one word.
 * Returns 0 and fills word, or -1. */
static int define_word(docwin *w, char *word, size_t size)
{
	char *text;
	int rc = -1;
	if (!sel_any(w) || w->sel_page != w->sel_end)
		return -1;
	if (w->sel_word[0])
		return folio_dict_clean_word(w->sel_word, word, size);
	text = sel_text(w);
	if (text && strlen(text) < 64)
		rc = folio_dict_clean_word(text, word, size);
	free(text);
	return rc;
}

void docwin_doc_menu_shown(docwin *w)
{
	{
		/* The page under the pointer: what Rotate's "Page" entries turn. */
		int ptr[5];
		folio_point pt;
		_swix(Wimp_GetPointerInfo, _IN(1), ptr);
		w->menu_page = point_at(w, ptr[0], ptr[1], &pt, -1);
	}
	menu_set_fade(app.doc_menu, DOC_ROTATE, !w->can_annotate);
	menu_set_fade(app.doc_menu, DOC_SAVE, !w->can_annotate);
	menu_set_fade(app.doc_menu, DOC_PASSWORD, !w->can_annotate);
	menu_set_fade(app.doc_menu, DOC_SIGN, !w->can_annotate);
	if (speech_available()) {
		menu_set_tick(app.doc_menu, DOC_READ, speech_window() == w);
		menu_set_fade(app.doc_menu, DOC_SPEAKSEL, !sel_any(w));
	}
	menu_set_tick(app.doc_menu, DOC_SHELF, bookshelf_has(w->path));
	if (ocr_available())
		menu_set_fade(app.doc_menu, DOC_OCR, w->ocr == NULL);
	static char entry[100];
	char word[64];
	if (define_word(w, word, sizeof word) == 0) {
		snprintf(entry, sizeof entry, "Define \"%.40s\"", word);
		tbx_method(app.doc_menu, 4 /* Menu_SetEntryText */, DOC_DEFINE, (int)entry, 0);
		menu_set_fade(app.doc_menu, DOC_DEFINE, 0);
	} else {
		tbx_method(app.doc_menu, 4 /* Menu_SetEntryText */, DOC_DEFINE, (int)"Define", 0);
		menu_set_fade(app.doc_menu, DOC_DEFINE, 1);
	}
}

/* Opens the dictionary pop-up just below the selected word. */
static void define(docwin *w)
{
	char word[64];
	int st[9], x, y;
	if (define_word(w, word, sizeof word))
		return;
	get_state(w, st);
	x = st[1] + 32;
	y = st[4] - 32;
	{
		const folio_quad *q;
		if (sel_quads_of(w, w->sel_page, &q) > 0) {
			float k = 2.5f * w->zoom;
			float qx = q->ul.x < q->ll.x ? q->ul.x : q->ll.x;
			float qy = q->ll.y > q->lr.y ? q->ll.y : q->lr.y;
			x = (st[1] - st[5]) + page_left(w, w->sel_page) + (int)(qx * k);
			y = (st[4] - st[6]) + page_top(w, w->sel_page) - (int)(qy * k) - 8;
		}
	}
	app_log("define '%s' (%s)", word, folio_language(w->doc));
	dict_define(word, folio_language(w->doc), w->mode, x, y);
}

void docwin_edit_menu_shown(docwin *w)
{
	int undo = 0, redo = 0, markup_ok = w->can_annotate && sel_any(w);
	if (w->can_annotate) {
		core_now();
		undo = folio_can_undo(w->doc);
		redo = folio_can_redo(w->doc);
		core_unlock();
	}
	menu_set_fade(app.edit_menu, EDIT_UNDO, !undo);
	menu_set_fade(app.edit_menu, EDIT_REDO, !redo);
	menu_set_fade(app.edit_menu, EDIT_HILITE, !markup_ok);
	menu_set_fade(app.edit_menu, EDIT_ULINE, !markup_ok);
	menu_set_fade(app.edit_menu, EDIT_STRIKE, !markup_ok);
	menu_set_fade(app.edit_menu, EDIT_NOTE, !w->can_annotate);
	menu_set_fade(app.edit_menu, EDIT_XFA, !w->xfa);
	menu_set_fade(app.edit_menu, EDIT_DRAW, !w->can_annotate);
	menu_set_fade(app.edit_menu, EDIT_STAMP, !w->can_annotate);
	menu_set_tick(app.edit_menu, EDIT_DRAW, w->pen_on);
	menu_set_fade(app.edit_menu, EDIT_DELANNOT, w->asel_page < 0);
	/* Not faded when not allowed: choosing it says why, and offers the
	 * owner password. */
	menu_set_fade(app.edit_menu, EDIT_COPY, !sel_any(w));
	menu_set_fade(app.edit_menu, EDIT_CLEAR, !w->sel);
}

/* ---- Events ------------------------------------------------------------- */

/* The toolbar's page, count, zoom and toggles. */
static void tb_update(docwin *w)
{
	int exact = 1;
	if (!w->toolbar || !w->show_toolbar)
		return;
	if (core_trylock()) {
		folio_page_count(w->doc, &exact);
		core_unlock();
	} else {
		exact = !w->counting;
	}
	toolbar_update(w->toolbar, current_page(w), w->pages, exact, w->zoom,
		w->mode == FOLIO_PAGE_DARK, w->show_thumbs, w->cols == 2, sel_any(w), w->can_annotate,
		speech_window() == w);
}

/* The toolbar and the drawer take room at the top and left of the work
 * area: lay the pages out again, keeping the page being read. */
static void relayout_furniture(docwin *w)
{
	int page = current_page(w);
	w->top = w->show_toolbar && w->toolbar ? toolbar_height() : 0;
	w->left = w->show_thumbs && w->thumbs ? thumbs_width() : 0;
	{
		/* Same zoom, same page sprites: keep them (layout() moves to a new generation). */
		int gen = w->gen;
		layout(w);
		w->gen = gen;
	}
	go_to_page(w, page);
	redraw_all(w);
}

static void toggle_toolbar(docwin *w)
{
	w->show_toolbar = !w->show_toolbar;
	toolbar_attach(w->obj, w->toolbar, w->show_toolbar);
	relayout_furniture(w);
	w->fit_polls = 25;
	thumbs_reopen(w->thumbs, w->top);
	tb_update(w);
}

/* Widens the window by the drawer's width when it opens, to the left so the
 * pages stay where they are on the screen (to the right if the screen's
 * edge is in the way), and gives the same back when it closes. Opened
 * through the Toolbox, so the toolbar follows. */
static void make_room_for_drawer(docwin *w, int opening)
{
	int st[9];
	get_state(w, st);
	if (opening) {
		int want = thumbs_width();
		int left = st[1] - want >= 0 ? want : (st[1] > 0 ? st[1] : 0);
		int right = want - left;
		if (st[3] + right > app.screen_w)
			right = app.screen_w - st[3] > 0 ? app.screen_w - st[3] : 0;
		st[1] -= left;
		st[3] += right;
		/* The pages moved right in the work area by the drawer's width: keep
		 * the same part of them under the same bit of screen. */
		st[5] += right;
		w->grew_left = left;
		w->grew_right = right;
	} else {
		if (st[3] - st[1] - w->grew_left - w->grew_right < 400) {
			w->grew_left = w->grew_right = 0;
			return;           /* the reader has made it small since: leave it */
		}
		st[1] += w->grew_left;
		st[3] -= w->grew_right;
		st[5] -= w->grew_right;
		if (st[5] < 0)
			st[5] = 0;
		w->grew_left = w->grew_right = 0;
	}
	st[7] = -1;
	tbx_show(w->obj, 0, 1, st + 1, 0, -1);
}

/* Full screen (View > Full screen, F11): the window opened over the whole
 * screen, above everything (the icon bar too), with its title bar, scroll
 * bars and toolbar taken away, and the pages in the middle. F11 or Escape
 * puts back the window as it was. (F12 is left to the OS: the command line.) */
static void full_screen(docwin *w, int on)
{
	int st[9], ob[9], page;
	if (!on == !w->full)
		return;
	formui_commit(w);
	page = current_page(w);
	get_state(w, st);
	if (on) {
		int info[24];
		w->fs_box[0] = st[1];
		w->fs_box[1] = st[2];
		w->fs_box[2] = st[3];
		w->fs_box[3] = st[4];
		info[0] = w->wimp;
		_swix(Wimp_GetWindowInfo, _IN(1), (int)info | 1);   /* the header only */
		w->fs_flags = (unsigned)info[8];
		w->fs_toolbar = w->show_toolbar;
		w->fs_thumbs = w->show_thumbs;
		if (w->show_thumbs) {
			thumbs_show(w->thumbs, 0, w->top);
			w->show_thumbs = 0;
		}
		if (w->show_toolbar) {
			w->show_toolbar = 0;
			toolbar_attach(w->obj, w->toolbar, 0);
		}
		w->full = 1;
		relayout_furniture(w);
		ob[0] = w->wimp;
		ob[1] = 0;
		ob[2] = 0;
		ob[3] = app.screen_w;
		ob[4] = app.screen_h;
		ob[5] = 0;
		ob[6] = st[6];
		ob[7] = -1;
		/* No back, close, title, toggle, scroll bars or adjust icons. */
		ob[8] = (int)(w->fs_flags & ~((1u << 24) | (1u << 25) | (1u << 26) | (1u << 27) |
			(1u << 28) | (1u << 29) | (1u << 30)));      /* bit 31 stays: new-format flags */
		_swix(Wimp_OpenWindow, _INR(1, 4), ob, 0x4b534154 /* "TASK" */, -1, 1);
		app_log("%s: full screen", w->title);
	} else {
		w->full = 0;
		ob[0] = w->wimp;
		ob[1] = w->fs_box[0];
		ob[2] = w->fs_box[1];
		ob[3] = w->fs_box[2];
		ob[4] = w->fs_box[3];
		ob[5] = 0;
		ob[6] = st[6];
		ob[7] = -1;
		ob[8] = (int)w->fs_flags;
		if (w->fs_toolbar && w->toolbar) {
			w->show_toolbar = 1;
			toolbar_attach(w->obj, w->toolbar, 1);
		}
		w->show_thumbs = w->fs_thumbs && w->thumbs;
		relayout_furniture(w);
		_swix(Wimp_OpenWindow, _INR(1, 4), ob, 0x4b534154, -1, 1);
		/* And through the Toolbox, so the toolbar comes back with it. */
		get_state(w, st);
		st[7] = -1;
		tbx_show(w->obj, 0, 1, st + 1, 0, -1);
		if (w->show_thumbs)
			thumbs_show(w->thumbs, 1, w->top);
		w->fit_polls = 25;
		app_log("%s: back from full screen", w->title);
	}
	go_to_page(w, page);
	redraw_all(w);
	w->want_render = 1;
	/* The keys (Page Down, Escape, F11) go to the window. */
	_swix(Wimp_SetCaretPosition, _INR(0, 5), w->wimp, -1, 0, 0, 1 << 25, -1);
	tb_update(w);
}

static void toggle_thumbs(docwin *w)
{
	if (!w->thumbs)
		return;
	w->show_thumbs = !w->show_thumbs;
	if (!w->show_thumbs)
		thumbs_show(w->thumbs, 0, w->top);
	relayout_furniture(w);
	make_room_for_drawer(w, w->show_thumbs);
	if (w->show_thumbs)
		thumbs_show(w->thumbs, 1, w->top);
	thumbs_set_current(w->thumbs, current_page(w), 1);
	w->fit_polls = 25;
	w->want_render = 1;
	tb_update(w);
}

/* One page or two side by side (and whether the first is alone). The page
 * sprites stay; the window widens to show the second page (to the right,
 * or left if the screen's edge is in the way), and narrows again after. */
static void set_columns(docwin *w, int cols, int cover)
{
	int page = current_page(w), st[9], gen = w->gen;
	if (cols == w->cols && cover == w->cover)
		return;
	sel_clear(w);         /* before the pages move: it's EORed where it was */
	asel_clear(w);
	if (cols != w->cols) {
		get_state(w, st);
		st[5] = 0;
		if (cols == 2) {
			/* Only as much as the two pages need beyond what's showing. */
			int sw = slot_w(w, page);
			int need = w->left + 2 * sw + 3 * GAP - (st[3] - st[1]);
			int want = need < 0 ? 0 : need > sw + GAP ? sw + GAP : need;
			int right = want, left = 0;
			if (st[3] + right > app.screen_w) {
				right = app.screen_w - st[3] > 0 ? app.screen_w - st[3] : 0;
				left = want - right;
				if (st[1] - left < 0)
					left = st[1] > 0 ? st[1] : 0;
			}
			st[1] -= left;
			st[3] += right;
			w->grew_two = left + right;
			w->cols = 2;
			w->cover = cover;
			layout(w);
		} else {
			if (w->grew_two && st[3] - st[1] - w->grew_two >= 400)
				st[3] -= w->grew_two;
			w->grew_two = 0;
			w->cols = 1;
			w->cover = cover;
			layout(w);
		}
		st[7] = -1;
		tbx_show(w->obj, 0, 1, st + 1, 0, -1);
		w->fit_polls = 25;
	} else {
		w->cover = cover;
		layout(w);
	}
	w->gen = gen;         /* same zoom: the page sprites are still good */
	app_log("%s: %d page(s) across%s", w->title, w->cols, w->cols == 2 && w->cover ? ", first alone" : "");
	go_to_page(w, page);
	redraw_all(w);
	w->want_render = 1;
	tb_update(w);
}

static void set_mode(docwin *w, folio_page_mode m)
{
	w->mode = w->mode == m ? FOLIO_PAGE_NORMAL : m;
	abort_for(w);
	app_log("%s: mode %d", w->title, w->mode);
	cache_forget(w);
	w->gen++;
	thumbs_layout(w->thumbs, w->page.x1, w->page.y1, w->pages, w->mode);
	redraw_all(w);
	tb_update(w);
}

void docwin_event(docwin *w, int code)
{
	int st[9];
	switch (code) {
	case EV_DARK: set_mode(w, FOLIO_PAGE_DARK); break;
	case EV_SEPIA: set_mode(w, FOLIO_PAGE_SEPIA); break;
	case EV_ZOOM_IN: set_zoom(w, w->zoom * 1.25f); break;
	case EV_ZOOM_OUT: set_zoom(w, w->zoom / 1.25f); break;
	case EV_ZOOM_100: set_zoom(w, 1.0f); break;
	case EV_DEFAULT_ZOOM: choices_set_zoom(w->zoom); break;
	case EV_FIT_WIDTH:
		get_state(w, st);
		/* The row being read fits (the wider page of the two). */
		{
			int r = row_of(w, current_page(w)), i;
			float pw = 0;
			for (i = first_in_row(w, r); i <= last_in_row(w, r); i++)
				if (pt_w(w, i) > pw)
					pw = pt_w(w, i);
			set_zoom(w, (float)(st[3] - st[1] - w->left - (w->cols + 1) * GAP) /
				(w->cols * pw * 2.5f));
		}
		break;
	case EV_TWO_PAGES: set_columns(w, w->cols == 2 ? 1 : 2, w->cover); break;
	case EV_COVER_ALONE: set_columns(w, w->cols, !w->cover); break;
	case EV_NEXT: go_to_page(w, row_step(w, 1)); break;
	case EV_PREV: go_to_page(w, row_step(w, -1)); break;
	case EV_FIRST: go_to_page(w, 0); break;
	case EV_LAST: go_to_page(w, w->pages - 1); break;
	case EV_BACK: go_back(w); break;
	case EV_PRINT:
		/* Print... while printing stops it: always allowed. */
		if (print_busy_for(w) || allowed(w, code))
			print_open(w);
		break;
	case EV_EXPORT_DRAW: if (allowed(w, code)) export_open(w); break;
	case EV_SAVE_PAGES: if (allowed(w, code)) pagesave_open(w, 0); break;
	case EV_ROT_CW: case EV_ROT_ACW: case EV_ROT_180: case EV_ROTALL_CW: case EV_ROTALL_ACW:
		if (allowed(w, code))
			rotate(w, code);
		break;
	case EV_PG_DELETE: case EV_PG_MOVE: case EV_PG_INSERT:
		if (!w->can_annotate)
			tbx_report("Only a PDF's pages can be organised. Save pages as PDF, or Create PDF, makes one.");
		else if (allowed(w, code))
			pageorg_open(w, code == EV_PG_DELETE ? PO_DELETE : code == EV_PG_MOVE ? PO_MOVE : PO_INSERT,
				app.idb.self_id == app.pages_menu && w->menu_page >= 0 ? w->menu_page : current_page(w));
		break;
	case EV_COL_USUAL: case EV_COL_YELLOW: case EV_COL_GREEN: case EV_COL_BLUE: case EV_COL_PINK:
	case EV_COL_RED: case EV_COL_ORANGE: case EV_COL_PURPLE: case EV_COL_GREY:
		colour_chosen(w, code);
		break;
	case EV_ANNOTS: annotlist_open(w); break;
	case EV_MEASURE: measure_open(w); break;
	case EV_OCR_EMBED: ocr_embed(w); break;
	case EV_SAVE_PICTURES: if (allowed(w, code)) pagesave_open(w, 1); break;
	case EV_SAVE_TEXT: if (allowed(w, code)) pagesave_open(w, 2); break;
	case EV_GOTO: gotopage_open(w); break;
	case EV_DOCINFO: docinfo_open(w); break;
	case EV_OCR_PAGE: ocr_win_this_page(w->ocr); break;
	case EV_OCR_ALL: ocr_win_all(w->ocr); break;
	case EV_LINE_DOWN:
		get_state(w, st);
		scroll_to(w, st[6] - 40);
		break;
	case EV_LINE_UP:
		get_state(w, st);
		scroll_to(w, st[6] + 40);
		break;
	case EV_CLOSE: docwin_close(w); break;
	case EV_COPY: if (allowed(w, code)) sel_copy(w); break;
	case EV_SELECT_ALL: sel_all(w); break;
	case EV_CLEAR_SEL:
		if (w->stamp_pending[0] || w->pic_pending[0]) {
			docwin_place_cancel(w);
			break;
		}
		/* Escape: clears the selection; with none, leaves full screen. */
		if (w->full && !w->sel)
			full_screen(w, 0);
		else
			sel_clear(w);
		break;
	case EV_FULL_SCREEN: full_screen(w, !w->full); break;
	case EV_DEFINE: define(w); break;
	case EV_FIND: find_open(w); break;
	case EV_TOOLBAR: toggle_toolbar(w); break;
	case EV_HIGHLIGHT: markup(w, FOLIO_ANNOT_HIGHLIGHT); break;
	case EV_UNDERLINE: markup(w, FOLIO_ANNOT_UNDERLINE); break;
	case EV_STRIKE: markup(w, FOLIO_ANNOT_STRIKEOUT); break;
	case EV_NOTE: new_note(w); break;
	case EV_UNDO: undo_redo(w, 0); break;
	case EV_REDO: undo_redo(w, 1); break;
	case EV_DEL_ANNOT:
		if (w->asel_page >= 0)
			docwin_delete_annot(w, w->asel_page, w->asel_index);
		break;
	case EV_SAVE:
		if (w->can_annotate)
			save_open(w, 0);
		break;
	case EV_ADD_SHELF: bookshelf_toggle(w->path); break;
	case EV_PASSWORD:
		if (w->can_annotate)
			passui_security(w);
		break;
	case EV_DRAW:
		if (!w->can_annotate)
			tbx_report("Only a PDF can be drawn on. Save pages as PDF, or Create PDF, makes one.");
		else
			drawui_open(w);
		break;
	case EV_STAMP_PICTURE:
		if (!w->can_annotate)
			tbx_report("Only a PDF can be stamped. Save pages as PDF, or Create PDF, makes one.");
		else
			picbox_open(w);
		break;
	case EV_STAMP_0: case EV_STAMP_0 + 1: case EV_STAMP_0 + 2: case EV_STAMP_0 + 3: case EV_STAMP_0 + 4:
	case EV_STAMP_0 + 5: case EV_STAMP_0 + 6: case EV_STAMP_0 + 7: case EV_STAMP_0 + 8: case EV_STAMP_0 + 9:
	case EV_STAMP_0 + 10: case EV_STAMP_0 + 11: case EV_STAMP_0 + 12: case EV_STAMP_0 + 13:
		if (!w->can_annotate)
			tbx_report("Only a PDF can be stamped. Save pages as PDF, or Create PDF, makes one.");
		else if (folio_stamp_name(code - EV_STAMP_0)) {
			docwin_place_cancel(w);
			snprintf(w->stamp_pending, sizeof w->stamp_pending, "%s", folio_stamp_name(code - EV_STAMP_0));
			docwin_set_note(w, " (click on the page to place the stamp)");
		}
		break;
	case EV_XFA:
		if (w->xfa)
			xfaui_open(w);
		else
			tbx_report("This document isn't an XFA form. (Ordinary PDF forms are filled in on the page.)");
		break;
	case EV_SIGN:
		signbox_open(w, app.idb.self_id == app.doc_menu && w->menu_page >= 0 ? w->menu_page : current_page(w), NULL);
		break;
	case EV_READ_ALOUD:
		/* Read aloud again stops it: always allowed. */
		if (speech_window() == w || allowed(w, code))
			speech_read(w, 0);
		break;
	case EV_SPEAK_SEL: if (allowed(w, code)) speech_read(w, 1); break;
	case EV_THUMBS: toggle_thumbs(w); break;
	case EV_CONTENTS: contents_open(w); break;
	}
}

void docwin_view_menu_shown(docwin *w)
{
	menu_set_tick(app.view_menu, VIEW_DARK, w->mode == FOLIO_PAGE_DARK);
	menu_set_tick(app.view_menu, VIEW_SEPIA, w->mode == FOLIO_PAGE_SEPIA);
	menu_set_tick(app.view_menu, VIEW_ZOOM_100, w->zoom > 0.999f && w->zoom < 1.001f);
	menu_set_tick(app.view_menu, VIEW_TOOLBAR, w->show_toolbar);
	menu_set_tick(app.view_menu, VIEW_THUMBS, w->show_thumbs);
	menu_set_tick(app.view_menu, VIEW_FULL_SCREEN, w->full);
	menu_set_fade(app.view_menu, VIEW_TOOLBAR, !w->toolbar || w->full);
	menu_set_fade(app.view_menu, VIEW_THUMBS, !w->thumbs || w->full);
	menu_set_tick(app.view_menu, VIEW_DEFAULT_ZOOM, fabsf(w->zoom - app.ch.zoom) < 0.005f);
	menu_set_tick(app.view_menu, VIEW_TWO_PAGES, w->cols == 2);
	menu_set_tick(app.view_menu, VIEW_COVER_ALONE, w->cover);
	menu_set_fade(app.view_menu, VIEW_COVER_ALONE, w->cols != 2);
	menu_set_fade(app.view_menu, VIEW_ZOOM_IN, w->zoom >= MAX_ZOOM);
	menu_set_fade(app.view_menu, VIEW_ZOOM_OUT, w->zoom <= MIN_ZOOM);
}


/* ---- For the Contents and Find windows -------------------------------- */

folio_doc *docwin_doc(docwin *w) { return w->doc; }
const char *docwin_title(docwin *w) { return w->title; }
int docwin_wimp(docwin *w) { return w->wimp; }
int docwin_current_page(docwin *w) { return current_page(w); }
int docwin_is_counting(docwin *w) { return w->counting; }
int docwin_pages(docwin *w) { return w->pages; }
int docwin_reading_dir(docwin *w) { return w->dir < 0 ? -1 : 1; }
void docwin_page_pt(docwin *w, int page, float *pw, float *ph) { *pw = pt_w(w, page); *ph = pt_h(w, page); }
int docwin_ocr_wait(docwin *w, int page) { return is_window(w) && ocr_win_wait(w->ocr, page); }

void docwin_text_changed(docwin *w, int page)
{
	if (is_window(w) && page == current_page(w))
		tb_update(w);
}

void docwin_ocr_settings_changed(void)
{
	docwin *w;
	for (w = windows; w; w = w->next)
		ocr_settings_changed(w->ocr);
}

void docwin_ocr_menu_shown(docwin *w)
{
	int on = w && w->ocr;
	menu_set_fade(app.ocr_menu, OCR_PAGE, !on);
	menu_set_fade(app.ocr_menu, OCR_ALL, !on);
	menu_set_tick(app.ocr_menu, OCR_ALL, on && ocr_win_all_on(w->ocr));
}
void docwin_refresh_toolbar(docwin *w) { if (is_window(w)) tb_update(w); }

/* ---- Reading aloud ---------------------------------------------------------- */

static void extend_to(docwin *w, int page);

/* Where to start: the selection (1: its first and last points on *page),
 * else the top of what's showing of the page being read (0). */
int docwin_read_start(docwin *w, int *page, folio_point *a, int *end_page, folio_point *b)
{
	folio_quad f, l;
	if (sel_any(w) && sel_ends(w, page, &f, end_page, &l)) {
		a->x = f.ul.x + 0.5f;
		a->y = (f.ul.y + f.ll.y) / 2;
		b->x = l.ur.x - 0.5f;
		b->y = (l.ur.y + l.lr.y) / 2;
		return 1;
	}
	{
		int st[9];
		float k = 2.5f * w->zoom;
		*page = current_page(w);
		get_state(w, st);
		a->x = 0;
		a->y = (page_top(w, *page) - (st[6] - w->top)) / k;
		if (a->y < 0)
			a->y = 0;
		*b = *a;
	}
	return 0;
}

/* Shows the sentence being read (n 0: none), drawn in place, and scrolls
 * to it if it's out of view. */
void docwin_set_spoken(docwin *w, const int *pages, const folio_quad *q, int n)
{
	int old_page[SPK_BOXES], old_n = w->spk_n, i, j, done[2 * SPK_BOXES], nd = 0;
	folio_quad old_q[SPK_BOXES];
	if (n > SPK_BOXES)
		n = SPK_BOXES;
	memcpy(old_page, w->spk_page, sizeof old_page);
	memcpy(old_q, w->spk_q, sizeof old_q);
	for (i = 0; i < n; i++) {
		w->spk_page[i] = pages[i];
		w->spk_q[i] = q[i];
	}
	w->spk_n = n;
	/* Each page involved: EOR the old boxes off and the new ones on. */
	for (i = 0; i < old_n + n; i++) {
		int page = i < old_n ? old_page[i] : pages[i - old_n], b[11], more = 0, seen = 0;
		for (j = 0; j < nd; j++)
			if (done[j] == page)
				seen = 1;
		if (seen || page < 0 || page >= w->pages)
			continue;
		done[nd++] = page;
		if (!cache_find(w, page))
			continue;          /* a placeholder: drawn with the page */
		b[0] = w->wimp;
		b[1] = page_left(w, page);
		b[2] = page_top(w, page) - slot_h(w, page);
		b[3] = page_left(w, page) + slot_w(w, page);
		b[4] = page_top(w, page);
		_swix(Wimp_UpdateWindow, _IN(1) | _OUT(0), b, &more);
		while (more) {
			int x0 = b[1] - b[5] + page_left(w, page), y1 = b[4] - b[6] + page_top(w, page);
			for (j = 0; j < old_n; j++)
				if (old_page[j] == page)
					eor_quads_in(w, &old_q[j], 1, x0, y1, SPK_COLOUR);
			for (j = 0; j < n; j++)
				if (pages[j] == page)
					eor_quads_in(w, &q[j], 1, x0, y1, SPK_COLOUR);
			_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
		}
	}
	/* Out of view: bring it a third of the way down the window. */
	if (n > 0) {
		int st[9], top, bottom, vis_top, vis_bottom;
		float k = 2.5f * w->zoom;
		extend_to(w, pages[0]);
		get_state(w, st);
		top = page_top(w, pages[0]) - (int)(q[0].ul.y * k);
		bottom = page_top(w, pages[n - 1]) - (int)(q[n - 1].ll.y * k);
		vis_top = st[6] - w->top;
		vis_bottom = st[6] - (st[4] - st[2]);
		if (top > vis_top || bottom < vis_bottom) {
			int y = top + (st[4] - st[2]) / 3;
			if (y > 0)
				y = 0;
			scroll_to(w, y);
		}
	}
}

/* Pages counted since the extent was last set. Not with the core lock held. */
static void extend_to(docwin *w, int page)
{
	if (page < w->pages)
		return;
	core_lock();
	set_extent(w);
	core_unlock();
}

void docwin_go_to(docwin *w, int page)
{
	extend_to(w, page);
	go_to_page(w, page);
}

static void push_back(docwin *w);

/* Go to page (F5): remembered for Back, as a link is. */
void docwin_jump(docwin *w, int page)
{
	push_back(w);
	if (page >= w->pages && w->counting)
		docwin_open_page(w, page);
	else
		docwin_go_to(w, page < w->pages ? page : w->pages - 1);
}

/* Opened with a page asked for (Folio$OpenPage, or -page): that page,
 * instead of where the reader left off; a book's later pages once they're
 * counted. */
void docwin_open_page(docwin *w, int page)
{
	w->pending_mark = 0;
	if (page < w->pages) {
		w->goto_page = -1;
		docwin_go_to(w, page);
		app_log("%s: opened at page %d", w->title, page + 1);
	} else if (w->counting) {
		w->goto_page = page;
	} else if (w->pages > 0) {
		docwin_go_to(w, w->pages - 1);
	}
}

void docwin_show_hit(docwin *w, int page, const folio_quad *q, int n)
{
	int st[9], vis_h, y, i;
	float k = 2.5f * w->zoom, top;
	if (n <= 0)
		return;
	if (n > SEL_QUADS)
		n = SEL_QUADS;
	extend_to(w, page);
	sel_clear(w);
	w->sel = 1;
	w->sel_page = w->sel_end = page;
	w->sel_word[0] = 0;
	/* The selection's ends, for Copy and Define: just inside the first and
	 * last characters' outer edges. */
	w->sel_a.x = (q[0].ul.x + q[0].ll.x) / 2 + 0.5f;
	w->sel_a.y = (q[0].ul.y + q[0].ll.y) / 2;
	w->sel_b.x = (q[n - 1].ur.x + q[n - 1].lr.x) / 2 - 0.5f;
	w->sel_b.y = (q[n - 1].ur.y + q[n - 1].lr.y) / 2;
	{
		/* The boxes as found (the page is drawn again below). */
		selslot *t = slot_new(w, page);
		for (i = 0; i < n; i++)
			t->q[i] = q[i];
		t->n = n;
		t->stale = 0;
		t->s = w->sel_a;
		t->e = w->sel_b;
	}
	/* The hit a third of the way down the window. */
	top = q[0].ul.y < q[0].ur.y ? q[0].ul.y : q[0].ur.y;
	get_state(w, st);
	vis_h = st[4] - st[2];
	y = page_top(w, page) - (int)(top * k) + vis_h / 3;
	if (y > 0)
		y = 0;
	scroll_to(w, y);
	redraw_page(w, page);
}

/* ---- Links ------------------------------------------------------------- */
/* A Select click on a link follows it when the button comes up, unless the
 * click turns into a drag (selecting text) or a double-click (a word):
 * button type 10 reports the click first. Links inside the document go to
 * their page (and the place on it, if the link says); web and mail links
 * go to the browser or mail program (URI_Dispatch). Navigate > Back
 * (Ctrl-Left) returns to where the link was followed from. */

#define URI_Dispatch 0x4e381

static void link_press(docwin *w, int *b)
{
	folio_point pt;
	folio_loc loc;
	folio_link link;
	int page, rc;
	w->link_down = 0;
	page = point_at(w, b[0], b[1], &pt, -1);
	if (page < 0 || folio_loc_from_number(w->doc, page, &loc))
		return;
	core_now();
	rc = folio_link_at(w->doc, loc, pt, &link);
	if (rc == 1) {
		w->link_dest = link.dest;
		w->link_y = link.dest_y;
		snprintf(w->link_uri, sizeof w->link_uri, "%s", link.uri ? link.uri : "");
		w->link_down = link.dest.chapter >= 0 || w->link_uri[0];
	}
	core_unlock();
}

/* Remembers where the reader is, for Back. */
static void push_back(docwin *w)
{
	int st[9];
	get_state(w, st);
	if (w->back_n == BACK_MAX) {
		memmove(&w->back[0], &w->back[1], (BACK_MAX - 1) * sizeof w->back[0]);
		w->back_n--;
	}
	w->back[w->back_n].page = current_page(w);
	w->back[w->back_n].y = st[6];
	w->back[w->back_n].cols = w->cols;
	w->back[w->back_n].zoom = w->zoom;
	w->back_n++;
}

static void go_back(docwin *w)
{
	if (w->back_n <= 0)
		return;
	w->back_n--;
	/* The same place, if the layout hasn't changed since; else its page. */
	if (w->back[w->back_n].cols == w->cols && w->back[w->back_n].zoom == w->zoom)
		scroll_to(w, w->back[w->back_n].y);
	else
		go_to_page(w, w->back[w->back_n].page);
	app_log("link: back to page %d", w->back[w->back_n].page + 1);
}

static void follow_link(docwin *w)
{
	int page = -1;
	if (w->link_dest.chapter >= 0) {
		core_now();
		page = folio_page_number(w->doc, w->link_dest);
		if (page < 0) {
			/* Its chapter isn't counted yet: count up to it now. */
			_swix(Hourglass_On, 0);
			while ((page = folio_page_number(w->doc, w->link_dest)) < 0 &&
					folio_count_step(w->doc, 1) == 0)
				;
			_swix(Hourglass_Off, 0);
		}
		core_unlock();
		if (page < 0) {
			app_log("link: %s: page not found", w->link_uri);
			return;
		}
		push_back(w);
		extend_to(w, page);
		if (w->link_y < 0) {
			go_to_page(w, page);
		} else {
			/* The place a little below the top of the window. */
			int y = page_top(w, page) + w->top + GAP / 2 - (int)(w->link_y * 2.5f * w->zoom) + 24;
			scroll_to(w, y > 0 ? 0 : y);
		}
		app_log("link: %s -> page %d, y %g", w->link_uri, page + 1, (double)w->link_y);
		return;
	}
	docwin_follow_uri(w, w->link_uri, 0);
}

/* ---- Links out of the document -------------------------------------------
 * Web and email links go to the browser or mail program (URI_Dispatch).
 * Links to another document Folio reads (a relative path, or file:) open it
 * in Folio; Folio never starts other programs. With Choices > Ask before
 * following links (and always for one a form's script asks for), a box asks
 * first, showing where the link goes. */


static void go_out(docwin *w, const char *uri, const char *path, int page)
{
	if (path[0]) {
		docwin *o;
		app_log("link: open %s%s", path, page >= 0 ? " at a page" : "");
		o = docwin_open(path);
		if (o && page >= 0)
			docwin_open_page(o, page);
		return;
	}
	{
		_kernel_oserror *e = _swix(URI_Dispatch, _INR(0, 2), 0, uri, app.task);
		app_log("link: open %s%s%s", uri, e ? ": " : "", e ? e->errmess : "");
		if (e)
			tbx_report("Folio couldn't open the link: no web browser has been seen by the Filer yet.");
	}
	(void)w;
}

static void link_answer(void *data, int ok)
{
	pending_link *p = data;
	if (ok && (is_window(p->w) || !p->path[0]))
		go_out(p->w, p->uri, p->path, p->page);
	else
		app_log("link: not followed: %s", p->uri);
	p->w = NULL;
}

/* A link to a file: the RISC OS path of a document Folio can open, beside
 * this one, or "" (and -1 page). */
static void link_file(docwin *w, const char *uri, char *path, size_t size, int *page)
{
	char rel[400], *hash;
	const char *u = uri;
	int i, j;
	path[0] = 0;
	*page = -1;
	if (!strncmp(u, "file://", 7))
		u += 7;
	else if (!strncmp(u, "file:", 5))
		u += 5;
	else if (strchr(u, ':') || u[0] == '/' || !u[0])
		return;                  /* another scheme, or an absolute Unix path */
	snprintf(rel, sizeof rel, "%s", u);
	if ((hash = strchr(rel, '#')) != NULL) {
		int n = 0;
		if (sscanf(hash, "#page=%d", &n) == 1 && n > 0)
			*page = n - 1;
		*hash = 0;
	}
	/* Unix-style relative path to RISC OS: '/' to '.', '.' to '/', ".." to '^'. */
	{
		char ro[400];
		for (i = 0, j = 0; rel[i] && j < (int)sizeof ro - 2; i++) {
			if (rel[i] == '.' && rel[i + 1] == '.' && (rel[i + 2] == '/' || !rel[i + 2])) {
				ro[j++] = '^';
				i++;
			} else if (rel[i] == '/')
				ro[j++] = '.';
			else if (rel[i] == '.')
				ro[j++] = '/';
			else if (rel[i] == '%' && rel[i + 1] && rel[i + 2]) {
				int c = 0;
				sscanf(rel + i + 1, "%2x", &c);
				ro[j++] = c == '.' ? '/' : (char)c;
				i += 2;
			} else
				ro[j++] = rel[i];
		}
		ro[j] = 0;
		{
			char dir[400];
			const char *dot;
			snprintf(dir, sizeof dir, "%s", w->path);
			dot = strrchr(dir, '.');
			if (!dot)
				return;
			dir[dot - dir] = 0;
			snprintf(path, size, "%.250s.%.250s", dir, ro);
		}
	}
	/* Only an existing file. */
	{
		int type = 0;
		if (_swix(OS_File, _INR(0, 1) | _OUT(0), 17, path, &type) || type != 1)
			path[0] = 0;
	}
}

void docwin_follow_uri(docwin *w, const char *uri, int from_script)
{
	char msg[700], shown[200];
	int web = !strncmp(uri, "http://", 7) || !strncmp(uri, "https://", 8) ||
		!strncmp(uri, "mailto:", 7);
	if (!is_window(w))
		return;
	pend.w = w;
	snprintf(pend.uri, sizeof pend.uri, "%s", uri);
	pend.path[0] = 0;
	pend.page = -1;
	if (!web) {
		link_file(w, uri, pend.path, sizeof pend.path, &pend.page);
		if (!pend.path[0]) {
			app_log("link: not followed: %s", uri);
			tbx_report("Folio doesn't follow this link: it leads to a file Folio can't find, "
				"or to another program.");
			return;
		}
	}
	if (!app.ch.ask_links && !from_script) {
		link_answer(&pend, 1);
		return;
	}
	{
		char *d = app_desktop_text(pend.path[0] ? pend.path : uri);
		snprintf(shown, sizeof shown, "%.190s", d ? d : uri);
		free(d);
	}
	if (pend.path[0])
		snprintf(msg, sizeof msg, "%s leads to another document:\n%s\nOpen it in Folio?",
			from_script ? "The form's script" : "This link", shown);
	else if (!strncmp(uri, "mailto:", 7))
		snprintf(msg, sizeof msg, "%s writes an email:\n%s\nOpen your email program?",
			from_script ? "The form's script" : "This link", shown);
	else
		snprintf(msg, sizeof msg, "%s goes to a web page:\n%s\nOpen it in your web browser?",
			from_script ? "The form's script" : "This link", shown);
	ask_show("Follow link", msg, "Open", "Cancel", link_answer, &pend);
}

/* Null polls while Select is down on a link: follow it on release. */
static int link_tick(docwin *w)
{
	int ptr[5];
	if (!w->link_down)
		return 0;
	_swix(Wimp_GetPointerInfo, _IN(1), ptr);
	if (ptr[2] & 5)
		return 1;
	w->link_down = 0;
	follow_link(w);
	return 0;
}

void docwin_nav_menu_shown(docwin *w)
{
	menu_set_fade(app.nav_menu, NAV_BACK, w->back_n == 0);
}

/* ---- Toolbar ------------------------------------------------------------ */

docwin *docwin_from_toolbar(ObjectId id)
{
	docwin *w;
	for (w = windows; w; w = w->next)
		if (w->toolbar && w->toolbar == id)
			return w;
	return NULL;
}

void docwin_toolbar_click(docwin *w, int component, int *b)
{
	int ev = toolbar_event_for(component);
	if (toolbar_is_page_field(component))
		return;          /* the Toolbox gives it the caret */
	if (ev)
		docwin_event(w, ev);
	/* Keys back to the document (no caret), unless the box just opened has it. */
	if (ev != EV_FIND && ev != EV_DEFINE)
		_swix(Wimp_SetCaretPosition, _INR(0, 5), w->wimp, -1, 0, 0, 1 << 25, -1);
}

/* A key in the toolbar's page field: Return goes to the page. */
int docwin_toolbar_key(docwin *w, int key)
{
	if (key == 13) {
		int n = toolbar_typed_page(w->toolbar);
		if (n >= 1) {
			docwin_go_to(w, n - 1 < w->pages ? n - 1 : w->pages - 1);
			app_log("%s: go to page %d", w->title, n);
		}
		_swix(Wimp_SetCaretPosition, _INR(0, 5), w->wimp, -1, 0, 0, 1 << 25, -1);
		tb_update(w);
		return 1;
	}
	if (key == 27) {
		_swix(Wimp_SetCaretPosition, _INR(0, 5), w->wimp, -1, 0, 0, 1 << 25, -1);
		tb_update(w);
		return 1;
	}
	return 0;
}

/* An Open_Window_Request (moved, resized, scrolled, brought to the front). */
void docwin_open_request(docwin *w, int *b)
{
	/* A field being typed in: kept (and the editor closed) if the page moves
	 * or the window changes size; brought to the front with it otherwise.
	 * (A click brings the window to the front: test42-43 closed the editor
	 * each time, so the caret showed for a moment and went.) */
	int raise_editor = 0;
	{
		int st[9];
		get_state(w, st);
		if (st[1] != b[1] || st[2] != b[2] || st[3] != b[3] || st[4] != b[4] ||
				st[5] != b[5] || st[6] != b[6])
			formui_commit(w);
		else
			raise_editor = 1;
	}
	if (w->full) {
		/* Full screen stays over the whole screen. */
		b[1] = 0;
		b[2] = 0;
		b[3] = app.screen_w;
		b[4] = app.screen_h;
		_swix(Wimp_OpenWindow, _IN(1), b);
		if (raise_editor)
			formui_raise(w);
		return;
	}
	/* The toolbar is a pane: open it along the top of where the window is
	 * going, then the window just behind it, with plain Wimp_OpenWindow
	 * calls. (Until test32 this went through Toolbox_ShowObject on every
	 * request, which also raised a Toolbox event each time: dragging the
	 * window by its title bar kept Folio busy and starved other tasks.) */
	int pane = w->show_toolbar ? toolbar_open_with(w->toolbar, b) : 0;
	_swix(Wimp_OpenWindow, _IN(1), b);
	if (pane)
		toolbar_follow(w->toolbar, pane, w->wimp);
	else if (w->show_toolbar)
		toolbar_fit(w->toolbar, w->wimp);
	w->fit_polls = 2;
	if (raise_editor)
		formui_raise(w);
}

/* ---- Annotations ---------------------------------------------------------- */

/* The title: the file's name, with " *" while there are unsaved changes
 * (Style Guide p84). */
static void set_title(docwin *w);

static void update_title(docwin *w)
{
	int modified = 0;
	if (w->can_annotate && core_trylock()) {
		modified = folio_is_modified(w->doc);
		core_unlock();
	} else {
		modified = w->modified;
	}
	modified |= w->unsaved_new;
	if (modified == w->modified && w->title[0])
		return;
	w->modified = modified;
	set_title(w);
}

/* Pages were added (a scan's next pages): lay out again, and go to the
 * last page. */
void docwin_pages_added(docwin *w)
{
	core_now();
	remeasure(w);
	ocr_win_resync(w->ocr);
	core_unlock();
	relayout_keep(w);
	page_changed(w, -1);
	update_title(w);
	go_to_page(w, w->pages - 1);
}

/* Recognise every page's text (a new scan), if it isn't already. */
void docwin_ocr_all(docwin *w)
{
	if (w->ocr && !ocr_win_all_on(w->ocr))
		ocr_win_all(w->ocr);
}

void docwin_mark_new(docwin *w)
{
	w->unsaved_new = 1;
	update_title(w);
}

static void set_title(docwin *w)
{
	snprintf(w->title, sizeof w->title, "%.200s%s%s", leafname(w->path), w->modified ? " *" : "", w->note);
	window_set_title(w->obj, w->title);
}

void docwin_set_note(docwin *w, const char *note)
{
	if (!strcmp(w->note, note))
		return;
	snprintf(w->note, sizeof w->note, "%s", note);
	set_title(w);
}

/* A page's annotations changed: draw it (and its thumbnail) again. */
static void page_changed(docwin *w, int page)
{
	int i;
	for (i = 0; i < CACHE_SLOTS; i++)
		if (cache[i].w == w && (page < 0 || cache[i].page == page))
			cache_drop(&cache[i]);
	thumbs_forget(w->thumbs, page);
	w->want_render = 1;
	if (page < 0)
		redraw_all(w);
	else
		redraw_page(w, page);
	update_title(w);
	tb_update(w);
}

static void asel_clear(docwin *w)
{
	if (w->asel_page < 0)
		return;
	{
		int page = w->asel_page;
		w->asel_page = -1;
		redraw_page(w, page);
	}
}

/* A dotted frame round the selected annotation (EOR, so drawing it again
 * removes it; the page is redrawn underneath first anyway). */
static void asel_draw(docwin *w, int i, int x0, int y1)
{
	float k = 2.5f * w->zoom;
	int rx0, ry0, rx1, ry1;
	if (w->asel_page != i)
		return;
	rx0 = x0 + (int)(w->asel_rect.x0 * k) - 6;
	rx1 = x0 + (int)(w->asel_rect.x1 * k) + 6;
	ry1 = y1 - (int)(w->asel_rect.y0 * k) + 6;
	ry0 = y1 - (int)(w->asel_rect.y1 * k) - 6;
	_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), 0xbe5a1e00u, 0, 0);
	/* Two OS units thick. */
	_swix(OS_Plot, _INR(0, 2), 4, rx0, ry0);
	_swix(OS_Plot, _INR(0, 2), 96 + 5, rx1, ry0 + 3);
	_swix(OS_Plot, _INR(0, 2), 4, rx0, ry1 - 3);
	_swix(OS_Plot, _INR(0, 2), 96 + 5, rx1, ry1);
	_swix(OS_Plot, _INR(0, 2), 4, rx0, ry0);
	_swix(OS_Plot, _INR(0, 2), 96 + 5, rx0 + 3, ry1);
	_swix(OS_Plot, _INR(0, 2), 4, rx1 - 3, ry0);
	_swix(OS_Plot, _INR(0, 2), 96 + 5, rx1, ry1);
}

/* A Select click: on a note, open it; on other markup, select it. */
static void asel_click(docwin *w, int *b)
{
	folio_point pt;
	folio_loc loc;
	folio_annot_kind kind = FOLIO_ANNOT_OTHER;
	folio_rect r;
	int page, n;
	char *text = NULL;
	asel_clear(w);
	if (!w->can_annotate)
		goto done;
	page = point_at(w, b[0], b[1], &pt, -1);
	if (page < 0 || folio_loc_from_number(w->doc, page, &loc))
		goto done;
	core_now();
	n = folio_annot_at(w->doc, loc, pt, &kind, &r);
	if (n >= 0 && kind == FOLIO_ANNOT_NOTE)
		text = folio_annot_text(w->doc, loc, n);
	core_unlock();
	if (n < 0)
		goto done;
	if (kind == FOLIO_ANNOT_NOTE) {
		note_open(w, page, n, pt, text ? text : "");
		if (text) {
			core_lock();
			folio_free(w->doc, text);
			core_unlock();
		}
	}
	w->asel_page = page;
	w->asel_index = n;
	w->asel_rect = r;
	redraw_page(w, page);
done:
	tb_update(w);
}

static void markup(docwin *w, folio_annot_kind kind)
{
	folio_loc loc;
	folio_point s, e;
	int lo, hi, page, rc, done = 0;
	if (!w->can_annotate || !sel_any(w))
		return;
	sel_span(w, &lo, &hi, &s, &e);
	/* Each page's part (one undo each). */
	for (page = lo; page <= hi; page++) {
		const folio_quad *q;
		if (!sel_bounds(w, page, &s, &e) || sel_quads_of(w, page, &q) == 0 ||
				folio_loc_from_number(w->doc, page, &loc))
			continue;
		core_now();
		rc = folio_annot_markup(w->doc, loc, kind, s, e);
		core_unlock();
		if (rc) {
			app_log("markup: %s", folio_error(app.fc));
			say_if_signed("mark the text");
			break;
		}
		app_log("%s: markup %d on page %d", w->title, (int)kind, page + 1);
		done++;
	}
	sel_clear(w);
	for (page = lo; page <= hi && done; page++)
		page_changed(w, page);
}

/* Note: at the selection's start (just above it), else the top left of the
 * page being read. */
static void new_note(docwin *w)
{
	folio_point at = { 24, 24 };
	int page;
	if (!w->can_annotate)
		return;
	folio_quad f, l;
	int p1;
	if (sel_any(w) && sel_ends(w, &page, &f, &p1, &l)) {
		const folio_quad *q = &f;
		at.x = q->ul.x;
		at.y = q->ul.y - 22;
		if (at.y < 0)
			at.y = 0;
	} else {
		int st[9];
		float k = 2.5f * w->zoom;
		page = current_page(w);
		get_state(w, st);
		/* The top of what's showing of the page, if it's partly scrolled off. */
		at.y = (page_top(w, page) - (st[6] - w->top)) / k + 24;
		if (at.y < 24)
			at.y = 24;
	}
	note_open(w, page, -1, at, NULL);
}

void docwin_note_done(docwin *w, int page, int index, folio_point at, const char *utf8)
{
	folio_loc loc;
	int rc;
	if (!is_window(w) || folio_loc_from_number(w->doc, page, &loc))
		return;
	core_now();
	rc = index < 0 ? folio_annot_note(w->doc, loc, at, utf8)
		: folio_annot_set_text(w->doc, loc, index, utf8);
	core_unlock();
	if (rc) {
		app_log("note: %s", folio_error(app.fc));
		say_if_signed("change the note");
	} else
		page_changed(w, page);
}

void docwin_delete_annot(docwin *w, int page, int index)
{
	folio_loc loc;
	int rc;
	if (!is_window(w) || folio_loc_from_number(w->doc, page, &loc))
		return;
	core_now();
	rc = folio_annot_delete(w->doc, loc, index);
	core_unlock();
	if (rc) {
		app_log("delete: %s", folio_error(app.fc));
		return;
	}
	w->asel_page = -1;
	page_changed(w, page);
}

/* The colours of the Colour menu, in COL order after COL_USUAL. */
static const int colours[] = { 0xffdb00, 0x5cd65c, 0x3d8bff, 0xff7fc8, 0xe02424, 0xff9a1f,
	0xa35cff, 0x999999 };

int docwin_colour_rgb(int code)
{
	int k = code - EV_COL_YELLOW;
	return k >= 0 && k < (int)(sizeof colours / sizeof colours[0]) ? colours[k] : -1;
}

/* Colour menu: the colour for new markings (saved in Choices), and the
 * annotation selected, if there is one, recoloured. */
static void colour_chosen(docwin *w, int code)
{
	int rgb = docwin_colour_rgb(code), k;
	app.ch.annot_rgb = rgb;
	for (k = 0; k < 4; k++)
		folio_set_annot_colour(app.fc, (folio_annot_kind)k, rgb);
	for (k = COL_USUAL; k < COL_COUNT; k++)
		menu_set_tick(app.colour_menu, k, k - COL_USUAL + EV_COL_USUAL == code);
	choices_save();
	app_log("annotation colour: %s", rgb < 0 ? "the usual ones" : "chosen");
	if (w->asel_page >= 0 && w->can_annotate) {
		folio_loc loc;
		int rc = -1, page = w->asel_page;
		if (!folio_loc_from_number(w->doc, page, &loc)) {
			core_now();
			/* The usual colour for a recolour is the kind's: yellow. */
			rc = folio_annot_recolour(w->doc, loc, w->asel_index, rgb < 0 ? colours[0] : rgb);
			core_unlock();
		}
		if (rc == 0)
			page_changed(w, page);
		else {
			app_log("recolour: %s", folio_error(app.fc));
			say_if_signed("change the colour");
		}
	}
}

/* ---- Measuring ------------------------------------------------------------ */

int docwin_can_annotate(docwin *w)
{
	return is_window(w) && w->can_annotate;
}

void docwin_measure_mode(docwin *w, int on)
{
	if (!is_window(w))
		return;
	w->meas_on = on;
	if (!on && w->meas_page >= 0) {
		int page = w->meas_page;
		w->meas_page = -1;
		w->meas_n = 0;
		redraw_page(w, page);
	}
	if (on) {
		sel_clear(w);
		asel_clear(w);
	}
}

int docwin_measure_points(docwin *w, int *page, folio_point *pt, int max)
{
	int n = is_window(w) ? w->meas_n : 0;
	if (n > max)
		n = max;
	*page = is_window(w) ? w->meas_page : -1;
	if (n)
		memcpy(pt, w->meas_pt, sizeof *pt * (size_t)n);
	return n;
}

int docwin_measure_scale(docwin *w, int page, double *per_point, char *unit, size_t size)
{
	folio_loc loc;
	int own = 0;
	*per_point = 25.4 / 72;
	snprintf(unit, size, "mm");
	if (!is_window(w) || folio_loc_from_number(w->doc, page < 0 ? current_page(w) : page, &loc))
		return 0;
	if (core_trylock()) {
		own = folio_page_measure(w->doc, loc, per_point, unit, size);
		core_unlock();
	}
	return own;
}

void docwin_measure_clear(docwin *w)
{
	if (!is_window(w))
		return;
	if (w->meas_page >= 0)
		redraw_page(w, w->meas_page);
	w->meas_n = 0;
	measure_changed(w);
}

void docwin_measure_keep(docwin *w, const char *utf8)
{
	folio_loc loc;
	int rc = -1, page = w->meas_page;
	if (!is_window(w) || w->meas_n < 2 || !w->can_annotate || folio_loc_from_number(w->doc, page, &loc))
		return;
	core_now();
	rc = folio_annot_measure(w->doc, loc, w->meas_pt, w->meas_n, w->meas_n >= 3, utf8);
	core_unlock();
	if (rc) {
		char m[300];
		snprintf(m, sizeof m, "Folio couldn't add the measurement: %s", folio_error(app.fc));
		tbx_report(m);
		return;
	}
	app_log("measure: kept on page %d (%s)", page + 1, utf8);
	w->meas_n = 0;
	page_changed(w, page);
	measure_changed(w);
}

/* The points and the lines between them, in red; with three or more, the
 * line back to the first in pink (the area's edge). */
static void meas_draw(docwin *w, int i, int x0, int y1)
{
	float k = 2.5f * w->zoom;
	int j, px, py;
	if (!w->meas_on || w->meas_page != i || w->meas_n == 0)
		return;
	if (w->meas_n >= 3) {
		_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), 0x9090ff00u, 0, 0);
		_swix(OS_Plot, _INR(0, 2), 4, x0 + (int)(w->meas_pt[w->meas_n - 1].x * k),
			y1 - (int)(w->meas_pt[w->meas_n - 1].y * k));
		_swix(OS_Plot, _INR(0, 2), 5, x0 + (int)(w->meas_pt[0].x * k), y1 - (int)(w->meas_pt[0].y * k));
	}
	_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), 0x2020e000u, 0, 0);
	for (j = 0; j < w->meas_n; j++) {
		px = x0 + (int)(w->meas_pt[j].x * k);
		py = y1 - (int)(w->meas_pt[j].y * k);
		if (j) {
			int qx = x0 + (int)(w->meas_pt[j - 1].x * k), qy = y1 - (int)(w->meas_pt[j - 1].y * k);
			/* Two units thick. */
			_swix(OS_Plot, _INR(0, 2), 4, qx, qy);
			_swix(OS_Plot, _INR(0, 2), 5, px, py);
			_swix(OS_Plot, _INR(0, 2), 4, qx + 2, qy + 2);
			_swix(OS_Plot, _INR(0, 2), 5, px + 2, py + 2);
		}
		_swix(OS_Plot, _INR(0, 2), 4, px - 6, py - 6);
		_swix(OS_Plot, _INR(0, 2), 96 + 5, px + 6, py + 6);
	}
}

/* ---- Drawing, stamps and pictures ------------------------------------------ */

void docwin_pen_mode(docwin *w, int on, int rgb, float width)
{
	if (!is_window(w))
		return;
	if (on && !w->pen_pt && !(w->pen_pt = malloc(sizeof *w->pen_pt * PEN_POINTS))) {
		tbx_report("There isn't enough memory to draw.");
		return;
	}
	w->pen_rgb = rgb;
	w->pen_width = width;
	if (w->pen_on && w->pen_page >= 0)
		redraw_page(w, w->pen_page);           /* in its new colour */
	if (on == w->pen_on)
		return;
	if (!on)
		pen_commit(w);
	w->pen_on = on;
	if (on) {
		docwin_measure_mode(w, 0);
		docwin_place_cancel(w);
		sel_clear(w);
		asel_clear(w);
	}
}

int docwin_pen_strokes(docwin *w)
{
	return is_window(w) ? w->pen_strokes : 0;
}

void docwin_pen_clear(docwin *w)
{
	if (!is_window(w))
		return;
	if (w->pen_page >= 0)
		redraw_page(w, w->pen_page);
	w->pen_n = w->pen_strokes = 0;
	w->pen_page = -1;
}

void docwin_pen_undo(docwin *w)
{
	if (!is_window(w) || w->pen_strokes == 0)
		return;
	w->pen_n -= w->pen_count[--w->pen_strokes];
	redraw_page(w, w->pen_page);
}

/* The strokes go on the page as one drawing (an Ink annotation). */
static void pen_commit(docwin *w)
{
	folio_loc loc;
	int rc, page = w->pen_page;
	if (w->pen_drag) {
		_swix(Wimp_DragBox, _IN(1), 0);
		w->pen_drag = 0;
		w->pen_strokes++;
	}
	if (w->pen_strokes == 0 || page < 0 || folio_loc_from_number(w->doc, page, &loc)) {
		docwin_pen_clear(w);
		return;
	}
	core_now();
	rc = folio_annot_ink(w->doc, loc, w->pen_pt, w->pen_count, w->pen_strokes, w->pen_rgb, w->pen_width);
	core_unlock();
	if (rc) {
		char m[300];
		app_log("draw: %s", folio_error(app.fc));
		snprintf(m, sizeof m, "Folio couldn't keep the drawing: %s", folio_error(app.fc));
		tbx_report(m);
	} else
		app_log("%s: drawing (%d strokes) on page %d", w->title, w->pen_strokes, page + 1);
	w->pen_n = w->pen_strokes = 0;
	w->pen_page = -1;
	page_changed(w, page);
	update_title(w);
	drawui_changed(w);
}

void docwin_pen_keep(docwin *w)
{
	if (is_window(w))
		pen_commit(w);
}

/* One line of a stroke, as thick as the pen at this zoom (in OS units). */
static void pen_line(docwin *w, int ax, int ay, int bx, int by)
{
	int t = (int)(w->pen_width * 2.5f * w->zoom + 0.5f), d;
	if (t < 2)
		t = 2;
	for (d = -t / 2; d <= t / 2; d += 2) {
		_swix(OS_Plot, _INR(0, 2), 4, ax + d, ay);
		_swix(OS_Plot, _INR(0, 2), 5, bx + d, by);
		_swix(OS_Plot, _INR(0, 2), 4, ax, ay + d);
		_swix(OS_Plot, _INR(0, 2), 5, bx, by + d);
	}
}

static unsigned pen_gcol(docwin *w)
{
	return ((unsigned)(w->pen_rgb & 0xff) << 8) | ((unsigned)((w->pen_rgb >> 8) & 0xff) << 16) |
		((unsigned)((w->pen_rgb >> 16) & 0xff) << 24);
}

static void pen_draw(docwin *w, int i, int x0, int y1)
{
	float k = 2.5f * w->zoom;
	int s, j, at = 0, n;
	if (!w->pen_on || w->pen_page != i || w->pen_n == 0)
		return;
	_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), pen_gcol(w), 0, 0);
	for (s = 0; s <= w->pen_strokes && at < w->pen_n; s++) {
		n = s < w->pen_strokes ? w->pen_count[s] : w->pen_n - at;
		for (j = 1; j < n; j++)
			pen_line(w, x0 + (int)(w->pen_pt[at + j - 1].x * k), y1 - (int)(w->pen_pt[at + j - 1].y * k),
				x0 + (int)(w->pen_pt[at + j].x * k), y1 - (int)(w->pen_pt[at + j].y * k));
		if (n == 1)
			pen_line(w, x0 + (int)(w->pen_pt[at].x * k), y1 - (int)(w->pen_pt[at].y * k),
				x0 + (int)(w->pen_pt[at].x * k) + 1, y1 - (int)(w->pen_pt[at].y * k));
		at += n;
	}
}

/* While drawing (null polls): the pointer's new points, drawn at once
 * over the page (Wimp_UpdateWindow), not by redrawing it. */
static int pen_tick(docwin *w)
{
	int ptr[5], b[11], more = 0, s = w->pen_strokes;
	folio_point pt, last;
	float k = 2.5f * w->zoom;
	if (!w->pen_drag)
		return 0;
	_swix(Wimp_GetPointerInfo, _IN(1), ptr);
	if (point_at(w, ptr[0], ptr[1], &pt, w->pen_page) < 0 || w->pen_n >= PEN_POINTS)
		return 1;
	last = w->pen_pt[w->pen_n - 1];
	if ((pt.x - last.x) * (pt.x - last.x) + (pt.y - last.y) * (pt.y - last.y) < 0.6f / w->zoom)
		return 1;
	w->pen_pt[w->pen_n++] = pt;
	w->pen_count[s]++;
	{
		int x0 = page_left(w, w->pen_page), y1 = page_top(w, w->pen_page), pad = 16 + (int)(w->pen_width * k);
		int ax = x0 + (int)(last.x * k), ay = y1 - (int)(last.y * k);
		int bx = x0 + (int)(pt.x * k), by = y1 - (int)(pt.y * k);
		b[0] = w->wimp;
		b[1] = (ax < bx ? ax : bx) - pad;
		b[2] = (ay < by ? ay : by) - pad;
		b[3] = (ax > bx ? ax : bx) + pad;
		b[4] = (ay > by ? ay : by) + pad;
		_swix(Wimp_UpdateWindow, _IN(1) | _OUT(0), b, &more);
		while (more) {
			int ox = b[1] - b[5], oy = b[4] - b[6];
			_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), pen_gcol(w), 0, 0);
			pen_line(w, ox + ax, oy + ay, ox + bx, oy + by);
			_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
		}
	}
	return 1;
}

void docwin_place_picture(docwin *w, const char *path, int clear_white)
{
	if (!is_window(w))
		return;
	docwin_place_cancel(w);
	docwin_pen_mode(w, 0, w->pen_rgb, w->pen_width);
	snprintf(w->pic_pending, sizeof w->pic_pending, "%s", path);
	w->pic_clear = clear_white;
	docwin_set_note(w, " (click on the page to place the picture)");
}

void docwin_place_cancel(docwin *w)
{
	if (!is_window(w) || (!w->stamp_pending[0] && !w->pic_pending[0]))
		return;
	w->stamp_pending[0] = w->pic_pending[0] = 0;
	docwin_set_note(w, "");
}

static void place_pending(docwin *w, int page, folio_point pt)
{
	folio_loc loc;
	int rc = -1;
	char what[40];
	snprintf(what, sizeof what, "%s", w->stamp_pending[0] ? "place the stamp" : "place the picture");
	if (!folio_loc_from_number(w->doc, page, &loc)) {
		_swix(Hourglass_On, 0);
		core_now();
		rc = w->stamp_pending[0] ? folio_annot_stamp(w->doc, loc, pt, w->stamp_pending) :
			folio_annot_picture(w->doc, loc, pt, 0, w->pic_pending, w->pic_clear);
		core_unlock();
		_swix(Hourglass_Off, 0);
	}
	if (rc) {
		char m[300];
		snprintf(m, sizeof m, "Folio couldn't %s: %s", what, folio_error(app.fc));
		tbx_report(m);
	} else {
		app_log("%s: %s on page %d", w->title, w->stamp_pending[0] ? w->stamp_pending : "picture", page + 1);
		page_changed(w, page);
	}
	docwin_place_cancel(w);
	update_title(w);
}

/* Text recognition > Add recognised text to the PDF. */
static void ocr_embed(docwin *w)
{
	int n;
	char m[300];
	if (!w->can_annotate) {
		tbx_report("Only a PDF can have its recognised text added. Save pages as PDF makes one.");
		return;
	}
	_swix(Hourglass_On, 0);
	core_now();
	n = folio_ocr_embed(w->doc);
	core_unlock();
	_swix(Hourglass_Off, 0);
	if (n < 0) {
		snprintf(m, sizeof m, "Folio couldn't add the recognised text: %s", folio_error(app.fc));
		tbx_report(m);
		return;
	}
	if (n == 0) {
		tbx_report("There's no recognised text to add yet. Choose Recognise all pages first, "
			"and do this when it has finished.");
		return;
	}
	app_log("%s: recognised text added to %d page(s)", w->title, n);
	update_title(w);
	snprintf(m, sizeof m, "The recognised text of %d page%s is now in the PDF, so other programs can find and "
		"copy it too. Save (F3) to keep it; Undo (Ctrl-Z) takes it out again.%s", n, n == 1 ? "" : "s",
		ocr_win_all_on(w->ocr) ? " Recognition is still going on: do this again when it has finished." : "");
	tbx_report(m);
}

/* An annotation chosen in the Annotations window: go to it, and select it
 * (so Delete and Colour act on it). */
void docwin_select_annot(docwin *w, int page, int index, folio_rect r)
{
	if (!is_window(w) || page < 0 || page >= w->pages)
		return;
	asel_clear(w);
	sel_clear(w);
	w->asel_page = page;
	w->asel_index = index;
	w->asel_rect = r;
	docwin_jump(w, page);
	redraw_page(w, page);
}

/* Delete, move or insert pages (pageorg.c), and everything that follows. */
int docwin_organise(docwin *w, int kind, const unsigned char *sel, int to, const char *path)
{
	int rc, first = to, i, before = w->pages;
	if (!is_window(w))
		return -1;
	if (kind != PO_INSERT)
		for (first = 0; first < w->pages && !sel[first]; first++)
			;
	_swix(Hourglass_On, 0);
	core_now();
	rc = kind == PO_DELETE ? folio_delete_pages(w->doc, sel, w->pages) :
		kind == PO_MOVE ? folio_move_pages(w->doc, sel, w->pages, to) :
		folio_insert_file(w->doc, path, to);
	if (!rc) {
		remeasure(w);
		ocr_win_resync(w->ocr);
	}
	core_unlock();
	_swix(Hourglass_Off, 0);
	if (rc) {
		char m[300];
		snprintf(m, sizeof m, "Folio couldn't %s: %s", kind == PO_DELETE ? "delete the pages" :
			kind == PO_MOVE ? "move the pages" : "insert the pages", folio_error(app.fc));
		tbx_report(m);
		return -1;
	}
	sel_clear(w);
	w->asel_page = -1;
	contents_forget(w);
	relayout_keep(w);
	page_changed(w, -1);
	update_title(w);
	/* Where the change is: the first page deleted (or what's there now),
	 * the pages' new place, or the first page inserted. */
	if (kind == PO_MOVE) {
		int k = 0, n = 0;
		for (i = 0; i < before; i++)
			if (sel[i]) {
				n++;
				if (i < to)
					k++;
			}
		first = (to > before ? before : to) - k;
		(void)n;
	} else if (kind == PO_INSERT && (to < 0 || to > before))
		first = before;
	if (first >= w->pages)
		first = w->pages - 1;
	if (first < 0)
		first = 0;
	app_log("%s: pages %s; %d now", w->title, kind == PO_DELETE ? "deleted" : kind == PO_MOVE ? "moved" : "inserted",
		w->pages);
	go_to_page(w, first);
	return 0;
}

/* Pages may have changed size (turned): measure them all again, from the
 * first, and lay them out (core lock held for the first). */
static void remeasure(docwin *w)
{
	folio_rect r;
	if (w->reflow)
		return;
	if (!folio_page_bounds(w->doc, (folio_loc){ 0, 0 }, &r))
		w->page = r;
	if (w->pw && w->nsize > 0) {
		memset(w->pw, 0, (size_t)w->nsize * sizeof *w->pw);
		memset(w->ph, 0, (size_t)w->nsize * sizeof *w->ph);
	}
	w->max_pw = w->page.x1;
	w->sizes_next = 0;
}

/* Rotate: the page Menu was pressed over (or, from the keys, the one under
 * the pointer; else the one being read), or every page. */
static void rotate(docwin *w, int code)
{
	unsigned char *sel;
	int deg = code == EV_ROT_ACW || code == EV_ROTALL_ACW ? -90 : code == EV_ROT_180 ? 180 : 90;
	int all = code == EV_ROTALL_CW || code == EV_ROTALL_ACW, page = -1, rc;
	if (!w->can_annotate) {
		tbx_report("Only a PDF's pages can be turned. Save pages as PDF, or Create PDF, makes one.");
		return;
	}
	if (!all) {
		if (app.idb.self_id == app.rotate_menu && w->menu_page >= 0) {
			page = w->menu_page;
		} else {
			int ptr[5];
			folio_point pt;
			_swix(Wimp_GetPointerInfo, _IN(1), ptr);
			if (ptr[3] == w->wimp)
				page = point_at(w, ptr[0], ptr[1], &pt, -1);
		}
		if (page < 0)
			page = current_page(w);
	}
	sel = calloc((size_t)(w->pages > 0 ? w->pages : 1), 1);
	if (!sel)
		return;
	if (all)
		memset(sel, 1, (size_t)w->pages);
	else if (page >= 0 && page < w->pages)
		sel[page] = 1;
	core_now();
	rc = folio_rotate_pages(w->doc, sel, w->pages, deg);
	if (!rc) {
		remeasure(w);
		ocr_win_resync(w->ocr);
	}
	core_unlock();
	free(sel);
	if (rc) {
		char m[300];
		snprintf(m, sizeof m, "Folio couldn't turn the page: %s", folio_error(app.fc));
		tbx_report(m);
		return;
	}
	if (all)
		app_log("%s: every page turned %d", w->title, deg);
	else
		app_log("%s: page %d turned %d", w->title, page + 1, deg);
	w->asel_page = -1;
	relayout_keep(w);
	page_changed(w, -1);
	update_title(w);
}

static void undo_redo(docwin *w, int redo)
{
	int rc;
	if (!w->can_annotate)
		return;
	core_now();
	rc = redo ? folio_redo(w->doc) : folio_undo(w->doc);
	if (!rc) {
		/* It may have turned pages back (or again). */
		remeasure(w);
		ocr_win_resync(w->ocr);
	}
	core_unlock();
	if (rc)
		return;
	w->asel_page = -1;
	relayout_keep(w);
	page_changed(w, -1);
}

/* ---- Signing (signui.c has the box) --------------------------------------- */

int docwin_can_sign(docwin *w)
{
	if (!is_window(w))
		return 0;
	if (!w->can_annotate) {
		tbx_report("Only a PDF can be signed. Save pages as PDF, or Create PDF, makes one.");
		return 0;
	}
	if (w->unsaved_new) {
		tbx_report("Save the PDF first, then sign it.");
		return 0;
	}
	return 1;
}

/* Signs field (or a new field at the foot of the page, on the right), then
 * opens the Save box: the signature is made as the PDF is saved. */
int docwin_sign(docwin *w, int page, const char *field, folio_signer *sg, const char *reason,
		const char *location, int mode)
{
	folio_loc loc;
	folio_rect where = { 0, 0, 0, 0 };
	float pw = 612, ph = 792;
	int rc;
	if (!is_window(w))
		return -1;
	_swix(Hourglass_On, 0);
	core_now();
	rc = folio_loc_from_number(w->doc, page, &loc);
	if (!rc && !field) {
		folio_page_size(w->doc, loc, &pw, &ph);
		where.x0 = pw - 36 - 216 > 0 ? pw - 36 - 216 : 0;
		where.x1 = pw - 36 > where.x0 + 72 ? pw - 36 : where.x0 + 72;
		where.y1 = ph - 36 > 72 ? ph - 36 : ph;
		where.y0 = where.y1 - 72 > 0 ? where.y1 - 72 : 0;
	}
	if (!rc)
		rc = folio_sign(w->doc, loc, field, where, sg, reason, location, mode);
	core_unlock();
	_swix(Hourglass_Off, 0);
	if (rc) {
		char m[300];
		snprintf(m, sizeof m, "Folio couldn't sign the PDF: %s", folio_error(app.fc));
		tbx_report(m);
		return -1;
	}
	app_log("%s: signed %s on page %d; to be saved", w->title, field ? field : "a new field", page + 1);
	w->asel_page = -1;
	page_changed(w, page);
	update_title(w);
	go_to_page(w, page);
	save_open(w, 0);
	return 0;
}

int docwin_xfa_set(docwin *w, const char *path, const char *utf8)
{
	int rc;
	if (!is_window(w))
		return -1;
	if (app.ch.obey_restrictions && !w->perm_form) {
		allowed(w, EV_FILL_FORM);
		return -1;
	}
	core_now();
	rc = folio_xfa_set(w->doc, path, utf8);
	core_unlock();
	app_log("xfa: %s set%s", path, rc ? " (failed)" : "");
	if (rc) {
		char m[300];
		snprintf(m, sizeof m, "Folio couldn't fill in the field: %s", folio_error(app.fc));
		tbx_report(m);
		return -1;
	}
	form_changed(w);
	return 0;
}

/* A change refused because the PDF is signed (core/sign.c) is said so. */
static void say_if_signed(const char *what)
{
	const char *e = folio_error(app.fc);
	char m[300];
	if (!strstr(e, "signed") && !strstr(e, "certified") && !strstr(e, "locked by a signature"))
		return;
	snprintf(m, sizeof m, "Folio can't %s: %s.", what, e);
	tbx_report(m);
}

const char *docwin_path(docwin *w)
{
	return w->path;
}

/* ---- Passwords ----------------------------------------------------------- */

int docwin_get_security(docwin *w, folio_security *s)
{
	int rc;
	if (!is_window(w) || !w->doc)
		return -1;
	core_now();
	rc = folio_get_security(w->doc, s);
	core_unlock();
	return rc;
}

int docwin_check_owner(docwin *w, const char *utf8)
{
	int rc;
	if (!is_window(w) || !w->doc)
		return -1;
	core_now();
	rc = folio_check_owner_password(w->doc, utf8);
	read_perms(w);
	core_unlock();
	app_log("%s: owner password %s", w->title, rc == 1 ? "right" : rc == 0 ? "wrong" : "not checked");
	return rc;
}

int docwin_set_password(docwin *w, const char *utf8)
{
	int rc;
	if (!is_window(w) || !w->doc || !w->can_annotate)
		return -1;
	core_now();
	rc = folio_set_password(w->doc, utf8);
	core_unlock();
	if (rc) {
		app_log("%s: password: %s", w->title, folio_error(app.fc));
		say_if_signed("change the password");
		return -1;
	}
	if (utf8 && *utf8)
		snprintf(w->new_password, sizeof w->new_password, "%s", utf8);
	else
		wipe(w->new_password, sizeof w->new_password);
	app_log("%s: password to be %s when saved", w->title, utf8 && *utf8 ? "set" : "removed");
	update_title(w);
	return 0;
}

docwin *docwin_first_modified(void)
{
	docwin *w;
	for (w = windows; w; w = w->next) {
		int m = 0;
		if (w->unsaved_new)
			return w;
		if (!w->can_annotate)
			continue;
		core_now();
		m = folio_is_modified(w->doc);
		core_unlock();
		if (m)
			return w;
	}
	return NULL;
}

/* Saves to path. The open PDF is still being read by MuPDF, so the new file
 * is written beside it first; then the document is closed, the old file
 * moved aside, the new one put in its place, and the document opened again
 * from there (RISC OS can't replace a file that's open). */
int docwin_save(docwin *w, const char *path)
{
	char tmp[300], bak[300];
	_kernel_oserror *e;
	int page = current_page(w), had_old, type = 0, replaced;
	folio_doc *doc;
	folio_security sec;
	if (strlen(path) + 3 >= sizeof tmp) {
		tbx_report("The file name is too long.");
		return -1;
	}
	snprintf(tmp, sizeof tmp, "%s~n", path);
	snprintf(bak, sizeof bak, "%s~o", path);
	_swix(Hourglass_On, 0);
	core_now();
	folio_get_security(w->doc, &sec);
	if (folio_save_pdf(w->doc, w->path, tmp)) {
		char msg[300];
		core_unlock();
		_swix(Hourglass_Off, 0);
		_swix(OS_File, _INR(0, 1), 6, tmp);
		snprintf(msg, sizeof msg, "Folio couldn't save the document: %s", folio_error(app.fc));
		tbx_report(msg);
		return -1;
	}
	ocr_win_before_reopen(w->ocr);
	folio_close(w->doc);
	w->doc = NULL;
	/* Move any file already there aside, then the new one in. */
	_swix(OS_File, _INR(0, 1) | _OUT(0), 17, path, &type);
	had_old = type == 1;
	if (had_old) {
		_swix(OS_File, _INR(0, 1), 6, bak);
		e = _swix(OS_FSControl, _INR(0, 2), 25, path, bak);
		if (e)
			had_old = -1;
	}
	e = had_old < 0 ? NULL : _swix(OS_FSControl, _INR(0, 2), 25, tmp, path);
	if (had_old < 0 || e) {
		/* Put things back as they were. */
		if (had_old == 1)
			_swix(OS_FSControl, _INR(0, 2), 25, bak, path);
		app_log("save: rename failed: %s", e ? e->errmess : "the old file couldn't be moved");
	} else {
		if (had_old == 1)
			_swix(OS_File, _INR(0, 1), 6, bak);
		_swix(OS_File, _INR(0, 2), 18, path, 0xadf);
		snprintf(w->path, sizeof w->path, "%s", path);
	}
	/* Open it again (the saved file, or the old one if that failed), with
	 * the password it now has. */
	replaced = !(had_old < 0 || e);
	if (replaced && sec.change == FOLIO_PW_SET)
		snprintf(w->password, sizeof w->password, "%s", w->new_password);
	else if (replaced && sec.change == FOLIO_PW_REMOVE)
		wipe(w->password, sizeof w->password);
	wipe(w->new_password, sizeof w->new_password);
	doc = folio_open_password(app.fc, w->path, NULL, w->password[0] ? w->password : NULL);
	if (doc) {
		w->doc = doc;
		folio_count_step(doc, 1 << 20);
		read_perms(w);
		ocr_win_after_reopen(w->ocr);
	}
	core_unlock();
	_swix(Hourglass_Off, 0);
	if (!doc) {
		tbx_report("Folio couldn't open the document again after saving it; its window will close.");
		docwin_close(w);
		return -1;
	}
	if (!replaced) {
		char msg[700];
		snprintf(msg, sizeof msg, "Folio couldn't replace '%s' (the new copy is in '%s').", path, tmp);
		tbx_report(msg);
		return -1;
	}
	app_log("saved %s%s", path, sec.change == FOLIO_PW_SET ? " with a new password" :
		sec.change == FOLIO_PW_REMOVE ? " without its password" : "");
	w->asel_page = -1;
	w->unsaved_new = 0;
	w->modified = -1;          /* force the title to be redone */
	update_title(w);
	go_to_page(w, page);
	tb_update(w);
	return 0;
}

/* ---- Forms and security ------------------------------------------------------ */

int is_docwin(docwin *w) { return is_window(w); }
float docwin_zoom(docwin *w) { return w->zoom; }

void docwin_page_origin(docwin *w, int page, int *sx, int *sy)
{
	int st[9];
	get_state(w, st);
	*sx = st[1] - st[5] + page_left(w, page);
	*sy = st[4] - st[6] + page_top(w, page);
}

/* After a change to the form: every page drawn again (a total on one page
 * can change a field on another), and the title's '*'. */
static void form_changed(docwin *w)
{
	page_changed(w, -1);
	update_title(w);
	tb_update(w);
}

int docwin_form_set_text(docwin *w, int page, int index, const char *utf8)
{
	folio_loc loc;
	int rc;
	if (!is_window(w) || folio_loc_from_number(w->doc, page, &loc))
		return -1;
	core_now();
	rc = folio_field_set_text(w->doc, loc, index, utf8);
	core_unlock();
	if (rc < 0) {
		app_log("form: %s", folio_error(app.fc));
		say_if_signed("fill in that field");
	}
	app_log("form: field %d on page %d %s", index, page + 1, rc == 0 ? "set" : "not set");
	form_changed(w);
	return rc;
}

int docwin_form_toggle(docwin *w, int page, int index)
{
	folio_loc loc;
	int rc;
	if (!is_window(w) || folio_loc_from_number(w->doc, page, &loc))
		return -1;
	core_now();
	rc = folio_field_toggle(w->doc, loc, index);
	core_unlock();
	if (rc < 0)
		say_if_signed("change that field");
	app_log("form: field %d on page %d toggled%s", index, page + 1, rc ? " (failed)" : "");
	form_changed(w);
	return rc;
}

int docwin_form_choose(docwin *w, int page, int index, int option)
{
	folio_loc loc;
	int rc;
	if (!is_window(w) || folio_loc_from_number(w->doc, page, &loc))
		return -1;
	core_now();
	rc = folio_field_choose(w->doc, loc, index, option);
	core_unlock();
	if (rc < 0)
		say_if_signed("change that field");
	app_log("form: field %d on page %d: choice %d%s", index, page + 1, option, rc ? " (failed)" : "");
	form_changed(w);
	return rc;
}

int docwin_form_press(docwin *w, int page, int index)
{
	folio_loc loc;
	int rc;
	if (!is_window(w) || folio_loc_from_number(w->doc, page, &loc))
		return -1;
	core_now();
	rc = folio_field_press(w->doc, loc, index);
	core_unlock();
	if (rc > 0)
		form_changed(w);
	return rc;
}

void docwin_ask_permission(docwin *w, int event)
{
	allowed(w, event);
}

/* On opening a PDF: its form's JavaScript, if Choices allow, and a word
 * about what's in it that may matter (Choices > Warn about scripts...). */
static void security_on_open(docwin *w)
{
	folio_content c;
	char msg[600], line[100];
	int n = 0;
	core_now();
	folio_get_content(w->doc, &c);
	if (c.pdf && app.ch.run_js && (c.fields || c.javascript))
		folio_form_js(w->doc, 1);
	core_unlock();
	if (!c.pdf)
		return;
	app_log("%s: javascript %d, files %d, fields %d%s, launch %d, submit %d, other files %d, web %d",
		w->title, c.javascript, c.files, c.fields, c.xfa ? " (XFA)" : "", c.launch, c.submit,
		c.other_files, c.uri);
	if (!app.ch.warn_content || !(c.javascript || c.files || c.launch || c.submit))
		return;
	snprintf(msg, sizeof msg, "%.60s has:", w->title);
	if (c.javascript) {
		snprintf(line, sizeof line, "\n- JavaScript (%s)", app.ch.run_js ?
			"its form may use it; it can't reach your files" : "not run");
		strcat(msg, line);
		n++;
	}
	if (c.files) {
		snprintf(line, sizeof line, "\n- %d attached file%s (Folio doesn't open them)", c.files,
			c.files == 1 ? "" : "s");
		strcat(msg, line);
		n++;
	}
	if (c.launch) {
		snprintf(line, sizeof line, "\n- %d link%s to start a program (Folio won't)", c.launch,
			c.launch == 1 ? "" : "s");
		strcat(msg, line);
		n++;
	}
	if (c.submit && n < 4) {
		snprintf(line, sizeof line, "\n- a way to send the form away (Folio won't)");
		strcat(msg, line);
		n++;
	}
	strcat(msg, "\nChoices... on Folio's icon bar menu can turn this warning off.");
	ask_show("Security", msg, "OK", NULL, NULL, NULL);
}
