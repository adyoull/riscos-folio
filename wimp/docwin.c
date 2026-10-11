/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio document windows: pages stacked for continuous scrolling, drawn
 * from a cache of page sprites. Missing pages are loaded and drawn by the
 * worker thread (worker.c), given time after each poll; re-flowed books are
 * counted a chapter at a time; so the desktop never waits (Style Guide p103;
 * DESIGN.md §7). Pages ahead in the direction of reading are drawn early.
 *
 * Work area: x from 0 to ext_w, y from 0 down to -ext_h (OS units). Page i
 * occupies [left, left + slot_w] x [top(i) - slot_h, top(i)]. */
#include "app.h"
#include "worker.h"
#include "dict.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define GAP 32                    /* OS units around and between pages */
#define CACHE_BYTES (64 << 20)    /* page sprites */
#define CACHE_SLOTS 32
#define MIN_ZOOM 0.25f
#define MAX_ZOOM 4.0f
#define SEL_QUADS 256

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
	int px_w, px_h;           /* page size in pixels at this zoom */
	int slot_w, slot_h;       /* the same in OS units */
	int ext_w, ext_h;
	int pages;                /* pages in the extent */
	int counting;             /* 1 until every chapter is counted */
	int want_render;          /* a visible page was drawn as a placeholder */
	int dir;                  /* +1 reading forwards, -1 backwards */
	int last_page;
	int fail_page, fail_gen;  /* a page that failed, not to be retried */
	int pending_mark;         /* mark to go to once its chapter is counted */
	folio_mark mark;
	char place[256];          /* where the reading position is kept */
	/* Selection (one page). */
	int sel, sel_page, sel_n, dragging;
	folio_point sel_a, sel_b;
	folio_quad sel_q[SEL_QUADS];
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

static int page_top(const docwin *w, int i)
{
	return -(w->top + GAP + i * (w->slot_h + GAP));
}

static int page_left(const docwin *w)
{
	return w->left + (w->ext_w - w->left - w->slot_w) / 2;
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
		if (cache[i].w == w && cache[i].page == page && cache[i].gen == w->gen && cache[i].mode == w->mode && cache[i].area) {
			cache[i].age = ++cache_clock;
			return &cache[i];
		}
	return NULL;
}

/* Makes room for 'bytes' more and returns a free slot. */
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
			} else if (!oldest || cache[i].age < oldest->age)
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

static int *new_page_area(docwin *w, size_t *bytes_out)
{
	return new_area(w->px_w, w->px_h, bytes_out);
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
	j.area = new_page_area(w, &j.bytes);
	if (!j.area)
		return 0;
	j.owner = w;
	j.fc = app.fc;
	j.doc = w->doc;
	j.page = page;
	j.gen = w->gen;
	j.mode = w->mode;
	j.ppp = px_per_pt(w);
	j.px_w = w->px_w;
	j.px_h = w->px_h;
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
static void asel_draw(docwin *w, int i, int x0, int y1);
static void sel_eor_change(docwin *w, int page, const folio_quad *oldq, int n_old,
		const folio_quad *newq, int n_new);
static folio_quad old_q[SEL_QUADS];   /* the selection as last drawn, for sel_update */
static void markup(docwin *w, folio_annot_kind kind);
static void new_note(docwin *w);
static void undo_redo(docwin *w, int redo);
static int sel_drag_tick(docwin *w);
static int current_page(docwin *w);
static void go_to_page(docwin *w, int i);

/* A finished page from the worker: into the cache, and onto the screen. */
static void take_result(render_job *r)
{
	docwin *w = r->owner;
	cache_entry *c;
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
		int y1 = page_top(w, r->page);
		_swix(Wimp_ForceRedraw, _INR(0, 4), w->wimp, page_left(w), y1 - w->slot_h,
			page_left(w) + w->slot_w, y1);
	}
}

/* ---- Layout ------------------------------------------------------------ */

static void set_extent(docwin *w)
{
	int box[4];
	int exact;
	w->pages = folio_page_count(w->doc, &exact);
	if (w->pages < 1)
		w->pages = 1;
	w->ext_h = w->top + GAP + w->pages * (w->slot_h + GAP);
	thumbs_layout(w->thumbs, w->page.x1, w->page.y1, w->pages, w->mode);
	box[0] = 0;
	box[1] = -w->ext_h;
	box[2] = w->ext_w;
	box[3] = 0;
	_swix(Wimp_SetExtent, _INR(0, 1), w->wimp, box);
}

static void layout(docwin *w)
{
	float ppp = px_per_pt(w);
	w->px_w = (int)(w->page.x1 * ppp + 0.5f);
	w->px_h = (int)(w->page.y1 * ppp + 0.5f);
	if (w->px_w < 1) w->px_w = 1;
	if (w->px_h < 1) w->px_h = 1;
	w->slot_w = w->px_w << app.xeig;
	w->slot_h = w->px_h << app.xeig;
	w->ext_w = w->slot_w + 2 * GAP;
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

/* The page under the top of the visible area. */
static int current_page(docwin *w)
{
	int st[9], i;
	get_state(w, st);
	i = (-st[6] - GAP / 2) / (w->slot_h + GAP);
	if (i < 0) i = 0;
	if (i >= w->pages) i = w->pages - 1;
	return i;
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
 * "folio-place 1", then chapter and fraction, then the page's first words.
 * Read back through Choices: (Style Guide p92). */

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
	if (fgets(line, sizeof line, f) && !strncmp(line, "folio-place 1", 13) &&
			fscanf(f, "%d %f\n", &w->mark.chapter, &w->mark.fraction) == 2) {
		if (!fgets(w->mark.text, sizeof w->mark.text, f))
			w->mark.text[0] = 0;
		w->mark.text[strcspn(w->mark.text, "\n")] = 0;
		w->pending_mark = 1;
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
	fprintf(f, "folio-place 1\n%d %.5f\n%s\n", m.chapter, m.fraction, m.text);
	fclose(f);
	_swix(OS_File, _INR(0, 2), 18, path, 0xfff);
}

/* If the worker is on a page of w, stop it: it's out of date. */
static void abort_for(docwin *w)
{
	const render_job *cur = worker_current();
	if (cur && cur->owner == w)
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

docwin *docwin_open(const char *path)
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
	w->zoom = 1.0f;
	w->mode = FOLIO_PAGE_NORMAL;

	_swix(Hourglass_On, 0);
	make_accel_path(w);
	/* The core is the worker's while it draws: stop it, then take the lock. */
	worker_stop_for(NULL);
	core_lock();
	w->doc = folio_open(app.fc, path, w->accel[0] ? w->accel : NULL);
	if (!w->doc) {
		char msg[300];
		core_unlock();
		_swix(Hourglass_Off, 0);
		snprintf(msg, sizeof msg, "Folio can't open %s: %s", leafname(path), folio_error(app.fc));
		tbx_report(msg);
		free(w);
		return NULL;
	}
	w->reflow = folio_is_reflowable(w->doc);
	w->can_annotate = folio_can_annotate(w->doc);
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

	/* Style Guide p105: the first window centred, at most a quarter of the
	 * screen; each later one 48 OS units lower. */
	vis_w = w->ext_w + 40;
	if (vis_w > app.screen_w / 2)
		vis_w = app.screen_w / 2;
	vis_h = app.screen_h / 2;
	x0 = (app.screen_w - vis_w) / 2;
	y1 = (app.screen_h + vis_h) / 2 - 48 * (open_count % 6);
	open_count++;
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
	find_forget(w);
	contents_forget(w);
	annotui_forget(w);
	worker_stop_for(w);
	core_lock();
	save_place(w);
	folio_close(w->doc);
	core_unlock();
	cache_forget(w);
	/* Not recursively: the document menu is shared by every window. */
	thumbs_free(w->thumbs);
	tbx_delete(w->obj, 1);
	if (w->toolbar)
		tbx_delete(w->toolbar, 0);
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
		i0 = (-top_work - w->top - GAP) / (w->slot_h + GAP);
		i1 = (-bottom_work - w->top) / (w->slot_h + GAP);
		if (i0 < 0) i0 = 0;
		if (i1 >= w->pages) i1 = w->pages - 1;
		for (i = i0; i <= i1; i++) {
			int x0 = ox + page_left(w), y1 = oy + page_top(w, i);
			int x1 = x0 + w->slot_w, y0 = y1 - w->slot_h;
			cache_entry *c;
			if (x1 <= cx0 || x0 >= cx1 || y1 <= cy0 || y0 >= cy1)
				continue;
			c = cache_find(w, i);
			if (c) {
				/* OS_SpriteOp 52 (PutSpriteScaled), user area, sprite by pointer. */
				_swix(OS_SpriteOp, _INR(0, 7), 52 + 512, c->area, c->area + 4, x0, y0, 0, 0, 0);
				sel_draw(w, i, x0, y1);
				asel_draw(w, i, x0, y1);
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
static int wanted_page(docwin *w)
{
	int st[9], vis_h, top, bottom, i0, i1, i, k;
	int order[64], n = 0;
	const render_job *cur = worker_current();
	get_state(w, st);
	vis_h = st[4] - st[2];
	top = st[6];
	bottom = top - vis_h;
	i0 = (-top - GAP) / (w->slot_h + GAP);     /* the toolbar covers w->top of it */
	i1 = (-bottom - w->top) / (w->slot_h + GAP);
	if (i0 < 0) i0 = 0;
	if (i1 >= w->pages) i1 = w->pages - 1;
	for (i = i0; i <= i1 && n < 60; i++)
		order[n++] = i;
	for (k = 1; k <= 2; k++)
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
		if (cur && cur->owner == w && cur->page == i && cur->gen == w->gen)
			continue;
		if (folio_loc_from_number(w->doc, i, &loc))
			continue;
		return i;
	}
	return -1;
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
		busy |= w->counting || w->pending_mark;
	}

	/* Give the worker a page if it has none. */
	if (worker_idle()) {
		for (w = windows; w; w = w->next) {
			int page = w->want_render ? wanted_page(w) : -1;
			if (page < 0) {
				w->want_render = 0;
				continue;
			}
			if (submit_page(w, page))
				break;
		}
	}
	/* Then thumbnails for open drawers. */
	if (worker_idle()) {
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
	/* And some time to work on it. */
	if (!worker_idle()) {
		if (worker_slice(5) && worker_take(&r))
			take_result(&r);
		busy = 1;
	}
	for (w = windows; w; w = w->next) {
		busy |= sel_drag_tick(w);
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
	i = clamp_page >= 0 ? clamp_page : (-wy - w->top - GAP / 2) / (w->slot_h + GAP);
	if (i < 0 || i >= w->pages)
		return -1;
	pt->x = (wx - page_left(w)) / os_per_pt;
	pt->y = (page_top(w, i) - wy) / os_per_pt;
	if (clamp_page >= 0) {
		/* Dragging off the page: hold to its edges. */
		if (pt->x < 0) pt->x = 0;
		if (pt->y < 0) pt->y = 0;
		if (pt->x > w->page.x1) pt->x = w->page.x1;
		if (pt->y > w->page.y1) pt->y = w->page.y1;
	} else if (pt->x < 0 || pt->y < 0 || pt->x > w->page.x1 || pt->y > w->page.y1) {
		return -1;
	}
	return i;
}

static void redraw_page(docwin *w, int i)
{
	int y1 = page_top(w, i);
	_swix(Wimp_ForceRedraw, _INR(0, 4), w->wimp, page_left(w), y1 - w->slot_h,
		page_left(w) + w->slot_w, y1);
}

/* Recompute the selection's quads (snapping to 'mode') and redraw. */
static void sel_update(docwin *w, folio_select_mode mode)
{
	folio_loc loc;
	int n_old = w->sel_n;
	if (!w->sel || folio_loc_from_number(w->doc, w->sel_page, &loc)) {
		w->sel_n = 0;
		return;
	}
	memcpy(old_q, w->sel_q, (size_t)n_old * sizeof old_q[0]);
	core_now();
	if (mode != FOLIO_SELECT_CHARS)
		folio_snap(w->doc, loc, &w->sel_a, &w->sel_b, mode);
	w->sel_n = folio_selection_quads(w->doc, loc, w->sel_a, w->sel_b, w->sel_q, SEL_QUADS);
	if (w->sel_n < 0)
		w->sel_n = 0;
	core_unlock();
	if (n_old != w->sel_n || memcmp(old_q, w->sel_q, (size_t)n_old * sizeof old_q[0]))
		sel_eor_change(w, w->sel_page, old_q, n_old, w->sel_q, w->sel_n);
	tb_update(w);
}

static void sel_clear(docwin *w)
{
	if (!w->sel)
		return;
	sel_eor_change(w, w->sel_page, w->sel_q, w->sel_n, NULL, 0);
	w->sel = 0;
	w->sel_n = 0;
	w->sel_word[0] = 0;
	w->dragging = 0;
}

/* EOR the selection over page i (screen origin of the page at x0, y1). */
/* EOR quads over a page whose top left is at x0, y1 (screen). */
static void eor_quads(docwin *w, const folio_quad *quads, int count, int x0, int y1)
{
	float k = 2.5f * w->zoom;
	int n;
	_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), 0xffffff00u, 0, 3);
	for (n = 0; n < count; n++) {
		const folio_quad *q = &quads[n];
		float qx0 = q->ul.x < q->ll.x ? q->ul.x : q->ll.x;
		float qx1 = q->ur.x > q->lr.x ? q->ur.x : q->lr.x;
		float qy0 = q->ul.y < q->ur.y ? q->ul.y : q->ur.y;
		float qy1 = q->ll.y > q->lr.y ? q->ll.y : q->lr.y;
		int rx0 = x0 + (int)(qx0 * k), rx1 = x0 + (int)(qx1 * k + 0.999f);
		int ry1 = y1 - (int)(qy0 * k), ry0 = y1 - (int)(qy1 * k + 0.999f);
		_swix(OS_Plot, _INR(0, 2), 4, rx0, ry0);
		_swix(OS_Plot, _INR(0, 2), 96 + 5, rx1 - 1, ry1 - 1);
	}
}

static void sel_draw(docwin *w, int i, int x0, int y1)
{
	if (!w->sel || i != w->sel_page)
		return;
	eor_quads(w, w->sel_q, w->sel_n, x0, y1);
}

/* Changes what's shown of the selection on page from the quads old to new
 * in place (Wimp_UpdateWindow), EORing the old off and the new on, instead
 * of drawing the whole page again: no flicker while dragging. Only where the
 * page itself is showing (a placeholder gets the selection when it's drawn). */
static void sel_eor_change(docwin *w, int page, const folio_quad *oldq, int n_old,
		const folio_quad *newq, int n_new)
{
	int b[11], more = 0;
	if ((n_old == 0 && n_new == 0) || !cache_find(w, page))
		return;
	b[0] = w->wimp;
	b[1] = page_left(w);
	b[2] = page_top(w, page) - w->slot_h;
	b[3] = page_left(w) + w->slot_w;
	b[4] = page_top(w, page);
	_swix(Wimp_UpdateWindow, _IN(1) | _OUT(0), b, &more);
	while (more) {
		int x0 = b[1] - b[5] + page_left(w), y1 = b[4] - b[6] + page_top(w, page);
		eor_quads(w, oldq, n_old, x0, y1);
		eor_quads(w, newq, n_new, x0, y1);
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}

static void asel_click(docwin *w, int *b);

void docwin_click(docwin *w, int *b)
{
	folio_point pt;
	int page, buttons = b[2];
	/* Any click in the window takes the input focus (no caret), so the keys
	 * work (Style Guide p69). Button type 10: click = x256, drag = x16,
	 * double-click = x1; Select = 4, Adjust = 1. */
	_swix(Wimp_SetCaretPosition, _INR(0, 5), w->wimp, -1, 0, 0, 1 << 25, -1);
	if (buttons == 0x400) {                  /* Select click: an annotation, or deselect */
		sel_clear(w);
		asel_click(w, b);
	} else if (buttons == 0x40) {            /* Select drag: start a selection */
		page = point_at(w, b[0], b[1], &pt, -1);
		if (page < 0)
			return;
		sel_clear(w);
		w->sel = 1;
		w->sel_page = page;
		w->sel_a = w->sel_b = pt;
		w->dragging = 1;
		sel_update(w, FOLIO_SELECT_CHARS);
	} else if (buttons == 0x100 || buttons == 0x10) { /* Adjust: extend */
		if (!w->sel)
			return;
		if (point_at(w, b[0], b[1], &pt, w->sel_page) < 0)
			return;
		w->sel_b = pt;
		w->sel_word[0] = 0;
		w->dragging = buttons == 0x10;
		sel_update(w, FOLIO_SELECT_CHARS);
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
		w->sel_page = page;
		sel_update(w, FOLIO_SELECT_CHARS);
		app_log("word: %s", w->sel_word);
	}
}

/* While dragging (null polls): follow the pointer, scrolling near the
 * window's edges. Returns 1 while a drag goes on. */
static int sel_drag_tick(docwin *w)
{
	int ptr[5], st[9];
	folio_point pt;
	if (!w->dragging)
		return 0;
	_swix(Wimp_GetPointerInfo, _IN(1), ptr);
	if (!(ptr[2] & 5)) {
		w->dragging = 0;
		return 0;
	}
	get_state(w, st);
	if (ptr[1] < st[2] + 48)
		scroll_to(w, st[6] - 48);
	else if (ptr[1] > st[4] - 48)
		scroll_to(w, st[6] + 48);
	if (point_at(w, ptr[0], ptr[1], &pt, w->sel_page) >= 0 &&
			(pt.x != w->sel_b.x || pt.y != w->sel_b.y)) {
		w->sel_b = pt;
		sel_update(w, FOLIO_SELECT_CHARS);
	}
	return 1;
}

static void sel_copy(docwin *w)
{
	folio_loc loc;
	char *text;
	if (!w->sel || folio_loc_from_number(w->doc, w->sel_page, &loc))
		return;
	core_now();
	text = folio_selection_text(w->doc, loc, w->sel_a, w->sel_b);
	core_unlock();
	if (text) {
		clip_set_text(text);
		folio_free(w->doc, text);
	}
}

static void sel_all(docwin *w)
{
	int page = current_page(w);
	sel_clear(w);
	w->sel = 1;
	w->sel_page = page;
	w->sel_a.x = 0;
	w->sel_a.y = 0;
	w->sel_b.x = w->page.x1;
	w->sel_b.y = w->page.y1;
	sel_update(w, FOLIO_SELECT_CHARS);
}

/* The word to define: the double-clicked word, or a selection of one word.
 * Returns 0 and fills word, or -1. */
static int define_word(docwin *w, char *word, size_t size)
{
	folio_loc loc;
	char *text = NULL;
	int rc = -1;
	if (!w->sel || w->sel_n == 0)
		return -1;
	if (w->sel_word[0])
		return folio_dict_clean_word(w->sel_word, word, size);
	if (folio_loc_from_number(w->doc, w->sel_page, &loc))
		return -1;
	core_now();
	text = folio_selection_text(w->doc, loc, w->sel_a, w->sel_b);
	core_unlock();
	if (text && strlen(text) < 64)
		rc = folio_dict_clean_word(text, word, size);
	if (text)
		folio_free(w->doc, text);
	return rc;
}

void docwin_doc_menu_shown(docwin *w)
{
	menu_set_fade(app.doc_menu, DOC_SAVE, !w->can_annotate);
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
	if (w->sel_n > 0) {
		folio_quad *q = &w->sel_q[0];
		float k = 2.5f * w->zoom;
		float qx = q->ul.x < q->ll.x ? q->ul.x : q->ll.x;
		float qy = q->ll.y > q->lr.y ? q->ll.y : q->lr.y;
		x = (st[1] - st[5]) + page_left(w) + (int)(qx * k);
		y = (st[4] - st[6]) + page_top(w, w->sel_page) - (int)(qy * k) - 8;
	}
	app_log("define '%s' (%s)", word, folio_language(w->doc));
	dict_define(word, folio_language(w->doc), w->mode, x, y);
}

void docwin_edit_menu_shown(docwin *w)
{
	int undo = 0, redo = 0, markup_ok = w->can_annotate && w->sel && w->sel_n > 0;
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
	menu_set_fade(app.edit_menu, EDIT_COPY, !w->sel || w->sel_n == 0);
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
		w->mode == FOLIO_PAGE_DARK, w->show_thumbs, w->sel && w->sel_n > 0, w->can_annotate);
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

static void toggle_thumbs(docwin *w)
{
	if (!w->thumbs)
		return;
	w->show_thumbs = !w->show_thumbs;
	relayout_furniture(w);
	thumbs_show(w->thumbs, w->show_thumbs, w->top);
	thumbs_set_current(w->thumbs, current_page(w), 1);
	w->fit_polls = 25;
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
	case EV_FIT_WIDTH:
		get_state(w, st);
		set_zoom(w, (float)(st[3] - st[1] - w->left - 2 * GAP) / (w->page.x1 * 2.5f));
		break;
	case EV_NEXT: go_to_page(w, current_page(w) + 1); break;
	case EV_PREV: go_to_page(w, current_page(w) - 1); break;
	case EV_FIRST: go_to_page(w, 0); break;
	case EV_LAST: go_to_page(w, w->pages - 1); break;
	case EV_LINE_DOWN:
		get_state(w, st);
		scroll_to(w, st[6] - 40);
		break;
	case EV_LINE_UP:
		get_state(w, st);
		scroll_to(w, st[6] + 40);
		break;
	case EV_CLOSE: docwin_close(w); break;
	case EV_COPY: sel_copy(w); break;
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
	w->sel_page = page;
	w->sel_word[0] = 0;
	/* The selection's ends, for Copy and Define: just inside the first and
	 * last characters' outer edges. */
	w->sel_a.x = (q[0].ul.x + q[0].ll.x) / 2 + 0.5f;
	w->sel_a.y = (q[0].ul.y + q[0].ll.y) / 2;
	w->sel_b.x = (q[n - 1].ur.x + q[n - 1].lr.x) / 2 - 0.5f;
	w->sel_b.y = (q[n - 1].ur.y + q[n - 1].lr.y) / 2;
	for (i = 0; i < n; i++)
		w->sel_q[i] = q[i];
	w->sel_n = n;
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
	/* Through the Toolbox, so it moves the toolbar with the window
	 * (it's a pane the Toolbox places, not a Wimp child). */
	tbx_show(w->obj, 0, 1, b + 1, 0, -1);
	if (w->show_toolbar)
		toolbar_fit(w->toolbar, w->wimp);
	w->fit_polls = 10;
}

/* ---- Annotations ---------------------------------------------------------- */

/* The title: the file's name, with " *" while there are unsaved changes
 * (Style Guide p84). */
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
	snprintf(w->title, sizeof w->title, "%.250s%s", leafname(w->path), modified ? " *" : "");
	window_set_title(w->obj, w->title);
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
	int page = w->sel_page, rc;
	if (!w->can_annotate || !w->sel || w->sel_n == 0 ||
			folio_loc_from_number(w->doc, page, &loc))
		return;
	core_now();
	rc = folio_annot_markup(w->doc, loc, kind, w->sel_a, w->sel_b);
	core_unlock();
	if (rc) {
		app_log("markup: %s", folio_error(app.fc));
		return;
	}
	app_log("%s: markup %d on page %d", w->title, (int)kind, page + 1);
	sel_clear(w);
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
	if (w->sel && w->sel_n > 0) {
		folio_quad *q = &w->sel_q[0];
		page = w->sel_page;
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
	int page = current_page(w), had_old, type = 0;
	folio_doc *doc;
	if (strlen(path) + 3 >= sizeof tmp) {
		tbx_report("The file name is too long.");
		return -1;
	}
	snprintf(tmp, sizeof tmp, "%s~n", path);
	snprintf(bak, sizeof bak, "%s~o", path);
	_swix(Hourglass_On, 0);
	core_now();
	if (folio_save_pdf(w->doc, w->path, tmp)) {
		char msg[300];
		core_unlock();
		_swix(Hourglass_Off, 0);
		_swix(OS_File, _INR(0, 1), 6, tmp);
		snprintf(msg, sizeof msg, "Folio couldn't save the document: %s", folio_error(app.fc));
		tbx_report(msg);
		return -1;
	}
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
	/* Open it again (the saved file, or the old one if that failed). */
	doc = folio_open(app.fc, w->path, NULL);
	if (doc) {
		w->doc = doc;
		folio_count_step(doc, 1 << 20);
	}
	core_unlock();
	_swix(Hourglass_Off, 0);
	if (!doc) {
		tbx_report("Folio couldn't open the document again after saving it; its window will close.");
		docwin_close(w);
		return -1;
	}
	if (had_old < 0 || e) {
		char msg[700];
		snprintf(msg, sizeof msg, "Folio couldn't replace '%s' (the new copy is in '%s').", path, tmp);
		tbx_report(msg);
		return -1;
	}
	app_log("saved %s", path);
	w->asel_page = -1;
	w->modified = -1;          /* force the title to be redone */
	update_title(w);
	go_to_page(w, page);
	tb_update(w);
	return 0;
}
