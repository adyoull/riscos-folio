/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio document windows: pages stacked for continuous scrolling, drawn
 * from a cache of page sprites. Missing pages are drawn in null polls (one
 * page per poll), and re-flowed books are counted a chapter per poll, so the
 * desktop never waits (Style Guide p103; DESIGN.md §7).
 *
 * Work area: x from 0 to ext_w, y from 0 down to -ext_h (OS units). Page i
 * occupies [left, left + slot_w] x [top(i) - slot_h, top(i)]. */
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define GAP 32                    /* OS units around and between pages */
#define CACHE_BYTES (64 << 20)    /* page sprites */
#define CACHE_SLOTS 32
#define MIN_ZOOM 0.25f
#define MAX_ZOOM 4.0f

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
};

typedef struct {
	docwin *w;
	int page, gen;
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
		if (cache[i].w == w && cache[i].page == page && cache[i].gen == w->gen && cache[i].area) {
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

/* Renders page 'page' of w into a new 32bpp sprite (PRM p1177-1178). */
static cache_entry *render_page(docwin *w, int page)
{
	folio_loc loc;
	size_t img = (size_t)w->px_w * 4 * w->px_h;
	size_t bytes = 16 + 44 + img;
	cache_entry *c;
	int *area, *spr;
	int dpi = 180 >> app.xeig;
	folio_irect tile;

	if (folio_loc_from_number(w->doc, page, &loc))
		return NULL;      /* not counted yet */
	c = cache_slot(bytes);
	if (!c)
		return NULL;
	area = malloc(bytes);
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
	tile.x0 = 0;
	tile.y0 = 0;
	tile.x1 = w->px_w;
	tile.y1 = w->px_h;
	if (folio_render_tile(w->doc, loc, px_per_pt(w), tile, w->mode, 1,
			(uint32_t *)(spr + 11), w->px_w * 4)) {
		app_log("render page %d: %s", page, folio_error(app.fc));
		free(area);
		return NULL;
	}
	c->w = w;
	c->page = page;
	c->gen = w->gen;
	c->age = ++cache_clock;
	c->area = area;
	c->w_px = w->px_w;
	c->h_px = w->px_h;
	c->bytes = bytes;
	cache_bytes += bytes;
	return c;
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
	w->doc = folio_open(app.fc, path, w->accel[0] ? w->accel : NULL);
	if (!w->doc) {
		char msg[300];
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
	_swix(Hourglass_Off, 0);

	w->obj = create_window(w);
	if (!w->obj) {
		folio_close(w->doc);
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
	cache_forget(w);
	/* Not recursively: the document menu is shared by every window. */
	tbx_delete(w->obj, 1);
	folio_close(w->doc);
	free(w);
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
			} else {
				fill_rect(page_colour(w), x0, y0, x1, y1);
				w->want_render = 1;
			}
		}
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}

/* ---- Background work: counting, rendering --------------------------- */

static void after_counting(docwin *w)
{
	int old = w->pages;
	set_extent(w);
	if (w->pages != old)
		redraw_all(w);
	if (!w->counting && w->reflow && w->accel[0])
		folio_save_accel(w->doc, w->accel);
}

/* Renders one visible (or next) page that isn't cached. Returns 1 if it did. */
static int render_one(docwin *w)
{
	int st[9], vis_h, top, bottom, i0, i1, i;
	get_state(w, st);
	vis_h = st[4] - st[2];
	top = st[6];
	bottom = top - vis_h;
	i0 = (-top - GAP) / (w->slot_h + GAP);
	i1 = (-bottom) / (w->slot_h + GAP);
	if (i0 < 0) i0 = 0;
	if (i1 >= w->pages) i1 = w->pages - 1;
	/* Visible pages first, then the next one, to have it ready. */
	for (i = i0; i <= i1 + 1 && i < w->pages; i++) {
		folio_loc loc;
		if (cache_find(w, i))
			continue;
		if (folio_loc_from_number(w->doc, i, &loc))
			continue;
		if (render_page(w, i)) {
			int y1 = page_top(w, i);
			_swix(Wimp_ForceRedraw, _INR(0, 4), w->wimp, page_left(w), y1 - w->slot_h,
				page_left(w) + w->slot_w, y1);
			return 1;
		}
		return 0;
	}
	return 0;
}

int docwin_background(void)
{
	docwin *w;
	int busy = 0;
	unsigned t0, t;
	_swix(OS_ReadMonotonicTime, _OUT(0), &t0);
	for (w = windows; w; w = w->next) {
		if (w->counting) {
			/* Up to 4 cs of counting per poll. */
			do {
				int r = folio_count_step(w->doc, 1);
				if (r != 0) {
					if (r < 0)
						app_log("counting: %s", folio_error(app.fc));
					w->counting = 0;
				}
				_swix(OS_ReadMonotonicTime, _OUT(0), &t);
			} while (w->counting && t - t0 < 4);
			after_counting(w);
			busy |= w->counting;
		}
		if (w->want_render) {
			if (render_one(w))
				busy = 1;
			else
				w->want_render = 0;
		}
		busy |= w->want_render;
	}
	return busy;
}

void docwin_mode_changed(void)
{
	docwin *w;
	for (w = windows; w; w = w->next) {
		int page = current_page(w);
		cache_forget(w);
		layout(w);
		go_to_page(w, page);
		redraw_all(w);
	}
}

/* ---- Events ------------------------------------------------------------- */

static void set_mode(docwin *w, folio_page_mode m)
{
	w->mode = w->mode == m ? FOLIO_PAGE_NORMAL : m;
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

void docwin_click(docwin *w, int *block)
{
	/* Select or Adjust in the window: take the input focus (no caret) so
	 * the keys work (Style Guide p69). */
	if (block[2] & 5)
		_swix(Wimp_SetCaretPosition, _INR(0, 5), w->wimp, -1, 0, 0, 1 << 25, -1);
}
