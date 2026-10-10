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
	return -(GAP + i * (w->slot_h + GAP));
}

static int page_left(const docwin *w)
{
	return (w->ext_w - w->slot_w) / 2;
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
static int *new_page_area(docwin *w, size_t *bytes_out)
{
	size_t img = (size_t)w->px_w * 4 * w->px_h;
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
	spr[4] = w->px_w - 1;
	spr[5] = w->px_h - 1;
	spr[6] = 0;
	spr[7] = 31;
	spr[8] = 44;
	spr[9] = 44;
	/* RISC OS 3.5 mode word: type 6 (32bpp), dpi matching the screen. */
	spr[10] = (6 << 27) | (dpi << 14) | (dpi << 1) | 1;
	*bytes_out = bytes;
	return area;
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
static void abort_for(docwin *w);
static void sel_draw(docwin *w, int i, int x0, int y1);
static int sel_drag_tick(docwin *w);
static int current_page(docwin *w);
static void go_to_page(docwin *w, int i);

/* A finished page from the worker: into the cache, and onto the screen. */
static void take_result(render_job *r)
{
	docwin *w = r->owner;
	cache_entry *c;
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
	w->ext_h = GAP + w->pages * (w->slot_h + GAP);
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
	scroll_to(w, page_top(w, i) + GAP / 2);
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
	if (w->pending_mark)
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
};

static ObjectId create_window(docwin *w)
{
	static tbx_window t;
	ObjectId id = 0;
	_kernel_oserror *e;
	memset(&t, 0, sizeof t);
	t.flags = (1 << 1);   /* auto-open; closing is ours (Adjust opens the directory) */
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
	worker_stop_for(w);
	core_lock();
	save_place(w);
	folio_close(w->doc);
	core_unlock();
	cache_forget(w);
	/* Not recursively: the document menu is shared by every window. */
	tbx_delete(w->obj, 1);
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
		i0 = (-top_work - GAP) / (w->slot_h + GAP);
		i1 = (-bottom_work) / (w->slot_h + GAP);
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
	i0 = (-top - GAP) / (w->slot_h + GAP);
	i1 = (-bottom) / (w->slot_h + GAP);
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
		}
		if ((w->counting || w->pending_mark) && core_trylock()) {
			/* Up to 4 cs of counting per poll. */
			while (w->counting) {
				int c = folio_count_step(w->doc, 1);
				if (c != 0) {
					if (c < 0)
						app_log("counting: %s", folio_error(app.fc));
					w->counting = 0;
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
	/* And some time to work on it. */
	if (!worker_idle()) {
		if (worker_slice(5) && worker_take(&r))
			take_result(&r);
		busy = 1;
	}
	for (w = windows; w; w = w->next) {
		busy |= sel_drag_tick(w);
		busy |= w->want_render;
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
	i = clamp_page >= 0 ? clamp_page : (-wy - GAP / 2) / (w->slot_h + GAP);
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

/* The core, now, for the main thread: the worker's page (if any) is stopped
 * and will be drawn again later. */
static void core_now(void)
{
	worker_stop_for(NULL);
	core_lock();
}

/* Recompute the selection's quads (snapping to 'mode') and redraw. */
static void sel_update(docwin *w, folio_select_mode mode)
{
	folio_loc loc;
	if (!w->sel || folio_loc_from_number(w->doc, w->sel_page, &loc)) {
		w->sel_n = 0;
		return;
	}
	core_now();
	if (mode != FOLIO_SELECT_CHARS)
		folio_snap(w->doc, loc, &w->sel_a, &w->sel_b, mode);
	w->sel_n = folio_selection_quads(w->doc, loc, w->sel_a, w->sel_b, w->sel_q, SEL_QUADS);
	if (w->sel_n < 0)
		w->sel_n = 0;
	core_unlock();
	redraw_page(w, w->sel_page);
}

static void sel_clear(docwin *w)
{
	if (!w->sel)
		return;
	w->sel = 0;
	w->sel_n = 0;
	w->sel_word[0] = 0;
	w->dragging = 0;
	redraw_page(w, w->sel_page);
}

/* EOR the selection over page i (screen origin of the page at x0, y1). */
static void sel_draw(docwin *w, int i, int x0, int y1)
{
	float k = 2.5f * w->zoom;
	int n;
	if (!w->sel || i != w->sel_page)
		return;
	_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), 0xffffff00u, 0, 3);
	for (n = 0; n < w->sel_n; n++) {
		folio_quad *q = &w->sel_q[n];
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

void docwin_click(docwin *w, int *b)
{
	folio_point pt;
	int page, buttons = b[2];
	/* Any click in the window takes the input focus (no caret), so the keys
	 * work (Style Guide p69). Button type 10: click = x256, drag = x16,
	 * double-click = x1; Select = 4, Adjust = 1. */
	_swix(Wimp_SetCaretPosition, _INR(0, 5), w->wimp, -1, 0, 0, 1 << 25, -1);
	if (buttons == 0x400) {                  /* Select click: deselect */
		sel_clear(w);
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

void docwin_edit_menu_shown(docwin *w)
{
	menu_set_fade(app.edit_menu, EDIT_COPY, !w->sel || w->sel_n == 0);
	menu_set_fade(app.edit_menu, EDIT_CLEAR, !w->sel);
}

/* ---- Events ------------------------------------------------------------- */

static void set_mode(docwin *w, folio_page_mode m)
{
	w->mode = w->mode == m ? FOLIO_PAGE_NORMAL : m;
	abort_for(w);
	app_log("%s: mode %d", w->title, w->mode);
	cache_forget(w);
	w->gen++;
	redraw_all(w);
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
		set_zoom(w, (float)(st[3] - st[1] - 2 * GAP) / (w->page.x1 * 2.5f));
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
	}
}

void docwin_view_menu_shown(docwin *w)
{
	menu_set_tick(app.view_menu, VIEW_DARK, w->mode == FOLIO_PAGE_DARK);
	menu_set_tick(app.view_menu, VIEW_SEPIA, w->mode == FOLIO_PAGE_SEPIA);
	menu_set_tick(app.view_menu, VIEW_ZOOM_100, w->zoom > 0.999f && w->zoom < 1.001f);
	menu_set_fade(app.view_menu, VIEW_ZOOM_IN, w->zoom >= MAX_ZOOM);
	menu_set_fade(app.view_menu, VIEW_ZOOM_OUT, w->zoom <= MIN_ZOOM);
}

