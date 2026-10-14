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
	int modified;             /* as last shown in the title */
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
	int perm_print, perm_copy, perm_access;
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
static int sel_drag_tick(docwin *w);
static void drag_start(docwin *w);
static void drag_stop(docwin *w);
static int current_page(docwin *w);
static void go_to_page(docwin *w, int i);
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
	if (!app.obey_restrictions)
		return 1;
	switch (event) {
	case EV_PRINT:
		if (!w->perm_print)
			what = "printing";
		break;
	case EV_COPY:
	case EV_EXPORT_DRAW:
		if (!w->perm_copy)
			what = "copying its text and pictures";
		break;
	case EV_READ_ALOUD:
	case EV_SPEAK_SEL:
		if (!w->perm_access)
			what = "its text to be read aloud";
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
	if (!app.obey_restrictions)
		return 1;
	switch (event) {
	case EV_PRINT: return w->perm_print;
	case EV_COPY: case EV_EXPORT_DRAW: return w->perm_copy;
	case EV_READ_ALOUD: case EV_SPEAK_SEL: return w->perm_access;
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
	float widest = w->max_pw > w->page.x1 ? w->max_pw : w->page.x1;
	int sw = (int)(widest * px_per_pt(w) + 0.5f) << app.xeig;
	w->ext_w = w->cols * sw + (w->cols + 1) * GAP;
	if (w->ext_w < 640)
		w->ext_w = 640;
	w->ext_w += w->left;
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
/* <Choices$Write>.Folio.Choices: "folio-choices 1", then "zoom <percent>".
 * Read back through Choices: (Style Guide p92). */

void choices_load(void)
{
	FILE *f = fopen("Choices:Folio.Choices", "r");
	char line[80];
	app.default_zoom = 1.0f;
	app.ocr_fast = 0;
	app.ocr_auto = 1;
	app.obey_restrictions = 1;
	if (!f)
		return;
	if (fgets(line, sizeof line, f) && !strncmp(line, "folio-choices 1", 15))
		while (fgets(line, sizeof line, f)) {
			int pc;
			if (sscanf(line, "zoom %d", &pc) == 1 && pc >= (int)(MIN_ZOOM * 100) && pc <= (int)(MAX_ZOOM * 100))
				app.default_zoom = pc / 100.0f;
			else if (!strncmp(line, "ocr fast", 8))
				app.ocr_fast = 1;
			else if (!strncmp(line, "ocr accurate", 12))
				app.ocr_fast = 0;
			else if (sscanf(line, "ocr-auto %d", &pc) == 1)
				app.ocr_auto = pc != 0;
			else if (sscanf(line, "obey-restrictions %d", &pc) == 1)
				app.obey_restrictions = pc != 0;
		}
	fclose(f);
	app_log("choices: new documents at %d%%", (int)(app.default_zoom * 100 + 0.5f));
}

void choices_save(void)
{
	const char *cw = getenv("Choices$Write");
	char path[220];
	FILE *f;
	if (!cw || !*cw)
		return;
	snprintf(path, sizeof path, "%s.Folio", cw);
	_swix(OS_File, _INR(0, 1) | _IN(4), 8, path, 0);
	snprintf(path, sizeof path, "%s.Folio.Choices", cw);
	f = fopen(path, "w");
	if (!f) {
		tbx_report("Folio couldn't save its choices.");
		return;
	}
	fprintf(f, "folio-choices 1\nzoom %d\nocr %s\nocr-auto %d\nobey-restrictions %d\n",
		(int)(app.default_zoom * 100 + 0.5f), app.ocr_fast ? "fast" : "accurate", app.ocr_auto,
		app.obey_restrictions);
	fclose(f);
	_swix(OS_File, _INR(0, 2), 18, path, 0xfff);
}

static void choices_set_zoom(float z)
{
	app.default_zoom = z;
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
	w->zoom = app.default_zoom > 0 ? app.default_zoom : 1.0f;
	w->mode = FOLIO_PAGE_NORMAL;
	w->cols = 1;
	w->cover = 1;
	w->goto_page = -1;

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
	w->show_toolbar = w->toolbar != 0;
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
	find_forget(w);
	gotopage_forget(w);
	docinfo_forget(w);
	contents_forget(w);
	annotui_forget(w);
	passui_forget(w);
	print_forget(w);
	export_forget(w);
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
		if (w->dragging)
			return 1;
	return 0;
}

int docwin_background(void)
{
	docwin *w;
	render_job r;
	int busy = 0;
	unsigned t0, t;

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
	_swix(Wimp_SetCaretPosition, _INR(0, 5), w->wimp, -1, 0, 0, 1 << 25, -1);
	if (buttons != 0x400)
		w->link_down = 0;                     /* a drag or double-click: not a link */
	if (buttons == 0x400) {                  /* Select click: an annotation, a link, or deselect */
		sel_clear(w);
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
	menu_set_fade(app.doc_menu, DOC_SAVE, !w->can_annotate);
	menu_set_fade(app.doc_menu, DOC_PASSWORD, !w->can_annotate);
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
	case EV_CLEAR_SEL: sel_clear(w); break;
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
	menu_set_tick(app.view_menu, VIEW_DEFAULT_ZOOM, fabsf(w->zoom - app.default_zoom) < 0.005f);
	menu_set_tick(app.view_menu, VIEW_TWO_PAGES, w->cols == 2);
	menu_set_tick(app.view_menu, VIEW_COVER_ALONE, w->cover);
	menu_set_fade(app.view_menu, VIEW_COVER_ALONE, w->cols != 2);
	menu_set_fade(app.view_menu, VIEW_TOOLBAR, !w->toolbar);
	menu_set_fade(app.view_menu, VIEW_THUMBS, !w->thumbs);
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
	if (!strncmp(w->link_uri, "http://", 7) || !strncmp(w->link_uri, "https://", 8) ||
			!strncmp(w->link_uri, "mailto:", 7)) {
		_kernel_oserror *e = _swix(URI_Dispatch, _INR(0, 2), 0, w->link_uri, app.task);
		app_log("link: open %s%s%s", w->link_uri, e ? ": " : "", e ? e->errmess : "");
		if (e)
			tbx_report("Folio couldn't open the link: no web browser has been seen by the Filer yet.");
	} else {
		app_log("link: not followed: %s", w->link_uri);
		tbx_report("Folio can't follow links to other files yet.");
	}
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
	if (modified == w->modified && w->title[0])
		return;
	w->modified = modified;
	set_title(w);
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
	if (rc)
		app_log("note: %s", folio_error(app.fc));
	else
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

static void undo_redo(docwin *w, int redo)
{
	int rc;
	if (!w->can_annotate)
		return;
	core_now();
	rc = redo ? folio_redo(w->doc) : folio_undo(w->doc);
	core_unlock();
	if (rc)
		return;
	w->asel_page = -1;
	page_changed(w, -1);
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
	w->modified = -1;          /* force the title to be redone */
	update_title(w);
	go_to_page(w, page);
	tb_update(w);
	return 0;
}
