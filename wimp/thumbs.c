/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* The thumbnail drawer: a column of small pages down the left of a document
 * window, as in Acrobat. It's a Wimp child window nested in the document
 * window (Nested Window Manager: Wimp_OpenWindow with "TASK", the parent and
 * alignment flags), with its edges tied to the parent's left, top and
 * bottom, so it moves and stretches with the window and scrolls on its own.
 *
 * Thumbnails are drawn by the page worker, after the pages the window shows
 * (docwin_background asks thumbs_wanted for one when the worker is free).
 * The page being read is framed; a click goes to a page. */
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define TW        176       /* thumbnail width, OS units */
#define PAD       24
#define LABEL     40
#define VIS_W     (TW + 2 * PAD)
#define SCROLL_W  44        /* the child's vertical scroll bar, outside its visible area */
#define BUDGET    (32 << 20)
#define TASK      0x4b534154

enum { T_NONE = 0, T_HAVE = 1, T_FAILED = 2 };

struct thumbs {
	thumbs *next;
	docwin *w;
	int wimp;
	int shown;
	int top;                 /* space left above it for the toolbar */
	int pages;
	int th;                  /* thumbnail height, OS units */
	float page_w, page_h;    /* points, from the first page */
	folio_page_mode mode;
	int gen;
	int current;
	int **area;              /* per page: sprite area, or NULL */
	signed char *state;
	size_t bytes;
	int cap;
};

static thumbs *all;
static const unsigned window_flags = 0x80000000u | (1u << 28);   /* vertical scroll bar; new format */

int thumbs_width(void)
{
	return VIS_W + SCROLL_W;
}

static int row_h(const thumbs *t)
{
	return t->th + PAD + LABEL;
}

static int create_wimp_window(void)
{
	int b[23];
	int h = 0;
	memset(b, 0, sizeof b);
	b[0] = 0; b[1] = -400; b[2] = VIS_W; b[3] = 0;   /* visible area (set when opened) */
	b[4] = 0; b[5] = 0;
	b[6] = -1;
	b[7] = (int)window_flags;
	/* Colours: title fg/bg, work fg/bg, scroll outer/inner, title focus. */
	b[8] = 7 | (2 << 8) | (7 << 16) | (255u << 24);  /* work bg 255: we draw it */
	b[9] = 3 | (1 << 8) | (12 << 16);
	b[10] = 0; b[11] = -400; b[12] = VIS_W; b[13] = 0;   /* extent */
	b[14] = 0;                       /* title flags: none */
	b[15] = 3 << 12;                 /* work area: click */
	b[16] = 1;                       /* sprite area: Wimp pool */
	b[17] = 0;                       /* min size */
	b[21] = 0;                       /* no icons */
	if (_swix(Wimp_CreateWindow, _IN(1) | _OUT(0), b + 0, &h))
		return 0;
	return h;
}

thumbs *thumbs_new(docwin *w)
{
	thumbs *t = calloc(1, sizeof *t);
	if (!t)
		return NULL;
	t->wimp = create_wimp_window();
	if (!t->wimp) {
		free(t);
		return NULL;
	}
	t->w = w;
	t->current = -1;
	t->next = all;
	all = t;
	return t;
}

static void forget_all(thumbs *t)
{
	int i;
	for (i = 0; i < t->cap; i++) {
		free(t->area[i]);
		t->area[i] = NULL;
		t->state[i] = T_NONE;
	}
	t->bytes = 0;
}

void thumbs_free(thumbs *t)
{
	thumbs **p;
	if (!t)
		return;
	for (p = &all; *p; p = &(*p)->next)
		if (*p == t) {
			*p = t->next;
			break;
		}
	forget_all(t);
	free(t->area);
	free(t->state);
	{
		int b[1];
		b[0] = t->wimp;
		_swix(Wimp_DeleteWindow, _IN(1), b);
	}
	free(t);
}

thumbs *thumbs_from_wimp(int handle)
{
	thumbs *t;
	for (t = all; t; t = t->next)
		if (t->wimp == handle)
			return t;
	return NULL;
}

int thumbs_shown(thumbs *t)
{
	return t && t->shown;
}

static void set_extent(thumbs *t)
{
	int box[4];
	box[0] = 0;
	box[1] = -(PAD + (t->pages > 0 ? t->pages : 1) * row_h(t));
	box[2] = VIS_W;
	box[3] = 0;
	_swix(Wimp_SetExtent, _INR(0, 1), t->wimp, box);
}

/* Opens it in the parent at the left, below the toolbar, keeping its scroll. */
static void place(thumbs *t, int scroll_y)
{
	int ps[9], b[9];
	unsigned nest;
	ps[0] = docwin_wimp(t->w);
	if (_swix(Wimp_GetWindowState, _IN(1), ps))
		return;
	b[0] = t->wimp;
	b[1] = ps[1];
	b[2] = ps[2];
	b[3] = ps[1] + VIS_W;
	b[4] = ps[4] - t->top;
	if (b[4] < b[2] + 64)
		b[4] = b[2] + 64;
	b[5] = 0;
	b[6] = scroll_y;
	b[7] = -1;                      /* top of the parent's children */
	b[8] = (int)window_flags;
	/* Left and right edges tied to the parent's left edge, bottom to its
	 * bottom, top to its top; scroll offsets tied to left and top. */
	nest = 1u | (1u << 16) | (1u << 18) | (1u << 20) | (2u << 22) | (1u << 24) | (2u << 26);
	if (_swix(Wimp_OpenWindow, _INR(1, 4), b, TASK, ps[0], nest))
		app_log("thumbs: can't open the drawer (needs the nested Window Manager)");
}

void thumbs_show(thumbs *t, int on, int top)
{
	if (!t)
		return;
	t->top = top;
	if (on) {
		int st[9];
		st[0] = t->wimp;
		_swix(Wimp_GetWindowState, _IN(1), st);
		set_extent(t);
		place(t, t->shown ? st[6] : 0);
		t->shown = 1;
		thumbs_set_current(t, t->current, 1);
	} else if (t->shown) {
		int b[1];
		b[0] = t->wimp;
		_swix(Wimp_CloseWindow, _IN(1), b);
		t->shown = 0;
	}
}

/* The parent opened again (moved or resized): the Wimp moves children by
 * their alignment, so this is only needed when the toolbar comes or goes. */
void thumbs_reopen(thumbs *t, int top)
{
	int st[9];
	if (!t || !t->shown)
		return;
	t->top = top;
	st[0] = t->wimp;
	_swix(Wimp_GetWindowState, _IN(1), st);
	place(t, st[6]);
}

static int grow(thumbs *t, int pages)
{
	if (pages > t->cap) {
		int cap = pages + 64, i;
		int **a = realloc(t->area, (size_t)cap * sizeof *a);
		signed char *s;
		if (!a)
			return -1;
		t->area = a;
		s = realloc(t->state, (size_t)cap);
		if (!s)
			return -1;
		t->state = s;
		for (i = t->cap; i < cap; i++) {
			t->area[i] = NULL;
			t->state[i] = T_NONE;
		}
		t->cap = cap;
	}
	return 0;
}

void thumbs_layout(thumbs *t, float page_w, float page_h, int pages, folio_page_mode mode)
{
	int th;
	if (!t || page_w <= 0)
		return;
	th = (int)(TW * page_h / page_w + 0.5f);
	if (th > TW * 3)
		th = TW * 3;
	if (th != t->th || mode != t->mode || page_w != t->page_w || page_h != t->page_h) {
		forget_all(t);
		t->gen++;
		t->th = th;
		t->mode = mode;
		t->page_w = page_w;
		t->page_h = page_h;
		if (t->shown)
			_swix(Wimp_ForceRedraw, _INR(0, 4), t->wimp, 0, -0x1000000, VIS_W, 0);
	}
	if (pages != t->pages) {
		if (grow(t, pages))
			return;
		t->pages = pages;
		set_extent(t);
		if (t->shown)
			_swix(Wimp_ForceRedraw, _INR(0, 4), t->wimp, 0, -0x1000000, VIS_W, 0);
	}
}

static void redraw_row(thumbs *t, int i)
{
	int y1 = -(PAD / 2 + i * row_h(t));
	_swix(Wimp_ForceRedraw, _INR(0, 4), t->wimp, 0, y1 - row_h(t), VIS_W, y1);
}

/* The page being read: framed, and kept in view. */
void thumbs_set_current(thumbs *t, int page, int scroll)
{
	int old;
	if (!t)
		return;
	old = t->current;
	t->current = page;
	if (!t->shown)
		return;
	if (old != page) {
		if (old >= 0)
			redraw_row(t, old);
		if (page >= 0)
			redraw_row(t, page);
	}
	if (page >= 0 && (scroll || old != page)) {
		int st[9], vis_h, top, bottom, y1;
		st[0] = t->wimp;
		_swix(Wimp_GetWindowState, _IN(1), st);
		vis_h = st[4] - st[2];
		top = st[6];
		bottom = top - vis_h;
		y1 = -(PAD / 2 + page * row_h(t));
		if (y1 > top || y1 - row_h(t) < bottom) {
			int b[9];
			st[6] = y1 + vis_h / 3;
			if (st[6] > 0)
				st[6] = 0;
			memcpy(b, st, sizeof b);
			_swix(Wimp_OpenWindow, _IN(1), b);   /* keeps its parent and alignment */
		}
	}
}

/* The next thumbnail to draw, or -1: those in view first, then the rows
 * just below. busy_page is the one the worker has now (or -1). */
int thumbs_wanted(thumbs *t, int busy_page, int *px_w, int *px_h, float *ppp, int *gen)
{
	int st[9], vis_h, i0, i1, i;
	if (!t || !t->shown || t->pages <= 0 || t->th <= 0)
		return -1;
	st[0] = t->wimp;
	_swix(Wimp_GetWindowState, _IN(1), st);
	vis_h = st[4] - st[2];
	i0 = (-st[6]) / row_h(t);
	i1 = (-st[6] + vis_h) / row_h(t) + 3;
	if (i0 < 0) i0 = 0;
	if (i1 >= t->pages) i1 = t->pages - 1;
	for (i = i0; i <= i1; i++) {
		if (t->state[i] != T_NONE || i == busy_page)
			continue;
		*px_w = TW >> app.xeig;
		*px_h = t->th >> app.xeig;
		{
			/* Pages of other sizes fit inside the first page's shape. */
			float pw = t->page_w, ph = t->page_h, kw, kh;
			docwin_page_pt(t->w, i, &pw, &ph);
			kw = (float)*px_w / pw;
			kh = (float)*px_h / ph;
			*ppp = kw < kh ? kw : kh;
		}
		*gen = t->gen;
		return i;
	}
	return -1;
}

/* Evicts the thumbnails furthest from page until under budget. */
static void trim(thumbs *t, int page)
{
	while (t->bytes > BUDGET) {
		int i, far = -1, d = -1;
		for (i = 0; i < t->pages; i++)
			if (t->area[i] && abs(i - page) > d) {
				d = abs(i - page);
				far = i;
			}
		if (far < 0)
			break;
		t->bytes -= (size_t)t->area[far][0];
		free(t->area[far]);
		t->area[far] = NULL;
		t->state[far] = T_NONE;
	}
}

void thumbs_take(thumbs *t, int page, int gen, int *area, int ok)
{
	if (!t || gen != t->gen || page < 0 || page >= t->pages) {
		free(area);
		return;
	}
	if (!ok) {
		free(area);
		t->state[page] = T_FAILED;
		return;
	}
	free(t->area[page]);
	t->area[page] = area;
	t->state[page] = T_HAVE;
	t->bytes += (size_t)area[0];
	trim(t, page);
	if (t->shown)
		redraw_row(t, page);
}

static void fill(unsigned c, int x0, int y0, int x1, int y1)
{
	_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), c, 0, 0);
	_swix(OS_Plot, _INR(0, 2), 4, x0, y0);
	_swix(OS_Plot, _INR(0, 2), 96 + 5, x1, y1);
}

void thumbs_redraw(thumbs *t, int *b)
{
	int more = 0;
	int dark = t->mode == FOLIO_PAGE_DARK;
	unsigned bg = dark ? 0x30303000u : 0xd8d8d800u;
	b[0] = t->wimp;
	_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), b, &more);
	while (more) {
		int ox = b[1] - b[5], oy = b[4] - b[6];
		int top = oy - b[10], bottom = oy - b[8];
		int rh = row_h(t), i0, i1, i;
		fill(bg, b[7], b[8], b[9], b[10]);
		i0 = top / rh - 1;
		i1 = bottom / rh + 1;
		if (i0 < 0) i0 = 0;
		if (i1 >= t->pages) i1 = t->pages - 1;
		for (i = i0; i <= i1; i++) {
			int y1 = oy - PAD / 2 - i * rh - PAD / 2, x0 = ox + PAD;
			int y0 = y1 - t->th;
			char num[16];
			if (i == t->current)
				fill(0xbe5a1e00u, x0 - 8, y0 - 8, x0 + TW + 7, y1 + 7);
			if (t->area[i])
				_swix(OS_SpriteOp, _INR(0, 7), 52 + 512, t->area[i], t->area[i] + 4, x0, y0, 0, 0, 0);
			else
				fill(dark ? 0x00000000u : 0xffffff00u, x0, y0, x0 + TW - 1, y1 - 1);
			snprintf(num, sizeof num, "%d", i + 1);
			{
				int tw = 0;
				_swix(Wimp_TextOp, _INR(0, 2) | _OUT(0), 1, num, 0, &tw);   /* width */
				_swix(Wimp_TextOp, _INR(0, 2), 0, dark ? 0xffffff00u : 0x00000000u, bg);
				_swix(Wimp_TextOp, _INR(0, 5), 2 | (1u << 28), num, -1, -1,
					x0 + (TW - tw) / 2, y0 - LABEL / 2);
			}
		}
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}

void thumbs_click(thumbs *t, int *b)
{
	int st[9], i;
	st[0] = t->wimp;
	_swix(Wimp_GetWindowState, _IN(1), st);
	i = (-(b[1] - (st[4] - st[6])) - PAD / 2) / row_h(t);
	if (i >= 0 && i < t->pages)
		docwin_go_to(t->w, i);
}

/* An Open_Window_Request for it: scrolling. Re-open as asked. */
void thumbs_open_request(thumbs *t, int *b)
{
	_swix(Wimp_OpenWindow, _IN(1), b);
}

/* A page's thumbnail is out of date (its annotations changed); -1: all. */
void thumbs_forget(thumbs *t, int page)
{
	int i;
	if (!t)
		return;
	for (i = 0; i < t->pages; i++) {
		if (page >= 0 && i != page)
			continue;
		if (t->area[i]) {
			t->bytes -= (size_t)t->area[i][0];
			free(t->area[i]);
			t->area[i] = NULL;
		}
		t->state[i] = T_NONE;
		if (t->shown)
			redraw_row(t, i);
	}
	t->gen++;   /* a thumbnail being drawn now is of the old page */
}
