/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* The Contents window (Navigate ▸ Contents): the document's outline, one
 * entry per row, indented by level, with page numbers on the right. A
 * click goes to that entry. The section being read is shown highlighted.
 * One window, showing the document it was last opened for. */
#include "app.h"
#include "worker.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define ROW_H    44
#define PAD      8
#define INDENT   40
#define WIDTH    700
#define MAX_VIS  880

typedef struct {
	char *title;          /* desktop alphabet */
	int depth;
	folio_loc loc;        /* chapter -1: points nowhere */
	int page;             /* -1 until its chapter is counted */
} row;

static struct {
	ObjectId obj;
	int wimp;
	docwin *w;
	row *rows;
	int n;
	int current;          /* highlighted row, or -1 */
	char title[280];
} ct;

static ObjectId create_window(void)
{
	static tbx_window t;
	ObjectId id = 0;
	_kernel_oserror *e;
	memset(&t, 0, sizeof t);
	t.flags = (1 << 1) | (1 << 2);   /* the Toolbox opens and closes it */
	t.help_message = "This is the document's contents list.|MClick on an entry to go to it.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.default_focus = -1;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = WIDTH;
	t.window.vis_y1 = 400;
	t.window.behind = -1;
	/* Moveable, back, close, title, vertical scroll, adjust size; not auto-redraw. */
	t.window.window_flags = 0x80000002u | (1u << 24) | (1u << 25) | (1u << 26) |
		(1u << 28) | (1u << 29);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 0;          /* white, cleared by the Wimp */
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -400;
	t.window.work_x1 = WIDTH;
	t.window.title_flags = 0x00000119;
	t.window.work_flags = 3 << 12;   /* click */
	t.window.sprite_area = (void *)1;
	t.window.min_width = 200;
	t.window.min_height = 100;
	t.window.title_text = ct.title;
	t.window.title_validation = "";
	t.window.title_buflen = sizeof ct.title;
	strcpy(ct.title, "Contents");
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	window_get_wimp_handle(id, &ct.wimp);
	return id;
}

static void free_rows(void)
{
	int i;
	for (i = 0; i < ct.n; i++)
		free(ct.rows[i].title);
	free(ct.rows);
	ct.rows = NULL;
	ct.n = 0;
	ct.current = -1;
}

/* Copies the outline (core lock held). */
static void load_rows(folio_doc *doc)
{
	const folio_outline_item *items;
	int count = 0, i;
	free_rows();
	if (folio_outline(doc, &items, &count) || count <= 0)
		return;
	ct.rows = calloc((size_t)count, sizeof *ct.rows);
	if (!ct.rows)
		return;
	for (i = 0; i < count; i++) {
		row *r = &ct.rows[ct.n];
		r->title = app_desktop_text(items[i].title && *items[i].title ? items[i].title : "(untitled)");
		if (!r->title)
			continue;
		r->depth = items[i].depth < 8 ? items[i].depth : 8;
		r->loc = items[i].loc;
		r->page = r->loc.chapter >= 0 ? folio_page_number(doc, r->loc) : -1;
		ct.n++;
	}
}

static void set_extent(void)
{
	int box[4];
	box[0] = 0;
	box[1] = -(PAD * 2 + (ct.n ? ct.n : 1) * ROW_H);
	box[2] = WIDTH;
	box[3] = 0;
	_swix(Wimp_SetExtent, _INR(0, 1), ct.wimp, box);
}

static void redraw_all(void)
{
	_swix(Wimp_ForceRedraw, _INR(0, 4), ct.wimp, 0, -(PAD * 2 + (ct.n + 1) * ROW_H), WIDTH, 0);
}

/* The last entry that starts at or before the page being read. */
static int current_row(int page)
{
	int i, best = -1;
	for (i = 0; i < ct.n; i++)
		if (ct.rows[i].page >= 0 && ct.rows[i].page <= page)
			best = i;
	return best;
}

void contents_open(docwin *w)
{
	int st[9], dst[9], h;
	if (!ct.obj && !(ct.obj = create_window()))
		return;
	_swix(Hourglass_On, 0);
	core_now();
	load_rows(docwin_doc(w));
	core_unlock();
	_swix(Hourglass_Off, 0);
	ct.w = w;
	ct.current = current_row(docwin_current_page(w));
	snprintf(ct.title, sizeof ct.title, "Contents of %s", docwin_title(w));
	window_set_title(ct.obj, ct.title);
	set_extent();

	/* Open beside the document window's left edge, as tall as the list. */
	dst[0] = docwin_wimp(w);
	_swix(Wimp_GetWindowState, _IN(1), dst);
	h = PAD * 2 + (ct.n ? ct.n : 1) * ROW_H;
	if (h > MAX_VIS)
		h = MAX_VIS;
	st[0] = ct.wimp;
	st[1] = dst[1] + 48;
	st[4] = dst[4] - 48;
	st[3] = st[1] + WIDTH;
	st[2] = st[4] - h;
	if (st[2] < 64) {
		st[2] = 64;
	}
	st[5] = 0;
	st[6] = 0;
	/* Scroll so the current entry shows. */
	if (ct.current > 0 && (ct.current + 1) * ROW_H > h - PAD)
		st[6] = -(PAD + ct.current * ROW_H - h / 3);
	st[7] = -1;
	tbx_show(ct.obj, 0, 1, st + 1, 0, -1);
	redraw_all();
}

void contents_forget(docwin *w)
{
	if (ct.w != w)
		return;
	ct.w = NULL;
	free_rows();
	if (ct.obj)
		tbx_hide(ct.obj);
}

int contents_owns(int wimp_handle)
{
	return ct.obj && wimp_handle == ct.wimp;
}

void contents_page_changed(docwin *w)
{
	int st[9], i, unknown = 0, cur;
	if (w != ct.w || !ct.obj)
		return;
	st[0] = ct.wimp;
	if (_swix(Wimp_GetWindowState, _IN(1), st) || !(st[8] & (1 << 16)))
		return;
	/* Fill in page numbers counted since. */
	for (i = 0; i < ct.n; i++)
		if (ct.rows[i].page < 0 && ct.rows[i].loc.chapter >= 0)
			unknown = 1;
	if (unknown && core_trylock()) {
		folio_doc *doc = docwin_doc(w);
		for (i = 0; i < ct.n; i++)
			if (ct.rows[i].page < 0 && ct.rows[i].loc.chapter >= 0)
				ct.rows[i].page = folio_page_number(doc, ct.rows[i].loc);
		core_unlock();
		redraw_all();
	}
	cur = current_row(docwin_current_page(w));
	if (cur != ct.current) {
		int old = ct.current;
		ct.current = cur;
		if (old >= 0)
			_swix(Wimp_ForceRedraw, _INR(0, 4), ct.wimp, 0, -(PAD + (old + 1) * ROW_H), WIDTH, -(PAD + old * ROW_H));
		if (cur >= 0)
			_swix(Wimp_ForceRedraw, _INR(0, 4), ct.wimp, 0, -(PAD + (cur + 1) * ROW_H), WIDTH, -(PAD + cur * ROW_H));
	}
}

static void text_at(const char *s, int x, int y, int fg, int bg, int right)
{
	_swix(Wimp_TextOp, _INR(0, 2), 0, fg, bg);
	_swix(Wimp_TextOp, _INR(0, 5), 2 | (1u << 28) | (right ? (1u << 31) : 0), s, -1, -1, x, y);
}

void contents_redraw(int *b)
{
	int more = 0;
	b[0] = ct.wimp;
	_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), b, &more);
	while (more) {
		int ox = b[1] - b[5], oy = b[4] - b[6];
		int top = oy - b[10], bottom = oy - b[8];   /* work-area depths of the clip */
		int i0 = (top - PAD) / ROW_H, i1 = (bottom - PAD) / ROW_H, i;
		if (i0 < 0) i0 = 0;
		if (ct.n == 0) {
			text_at("This document has no contents list.", ox + 16, oy - PAD - ROW_H / 2,
				0x55555500, 0xffffff00, 0);
		}
		for (i = i0; i <= i1 && i < ct.n; i++) {
			row *r = &ct.rows[i];
			int y1 = oy - PAD - i * ROW_H, yc = y1 - ROW_H / 2;
			int fg = 0x00000000, bg = 0xffffff00;
			char num[16];
			if (i == ct.current) {
				fg = 0xffffff00;
				bg = 0x00000000;
				_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), bg, 0, 0);
				_swix(OS_Plot, _INR(0, 2), 4, ox, y1 - ROW_H);
				_swix(OS_Plot, _INR(0, 2), 96 + 5, ox + WIDTH, y1 - 1);
			}
			text_at(r->title, ox + 16 + r->depth * INDENT, yc,
				r->loc.chapter >= 0 ? fg : 0x88888800, bg, 0);
			if (r->page >= 0) {
				/* The page number over a band of background, so a long
				 * title doesn't run under it. */
				snprintf(num, sizeof num, "%d", r->page + 1);
				_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), bg, 0, 0);
				_swix(OS_Plot, _INR(0, 2), 4, ox + WIDTH - 112, y1 - ROW_H + 2);
				_swix(OS_Plot, _INR(0, 2), 96 + 5, ox + WIDTH, y1 - 2);
				text_at(num, ox + WIDTH - 16, yc, fg, bg, 1);
			}
		}
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}

void contents_click(int *b)
{
	int st[9], i, page;
	folio_doc *doc;
	if (!ct.w)
		return;
	st[0] = ct.wimp;
	_swix(Wimp_GetWindowState, _IN(1), st);
	i = (-(b[1] - (st[4] - st[6])) - PAD) / ROW_H;
	if (i < 0 || i >= ct.n || ct.rows[i].loc.chapter < 0)
		return;
	page = ct.rows[i].page;
	if (page < 0) {
		/* Its chapter isn't counted yet: count up to it now. */
		doc = docwin_doc(ct.w);
		_swix(Hourglass_On, 0);
		core_now();
		while ((page = folio_page_number(doc, ct.rows[i].loc)) < 0 &&
				folio_count_step(doc, 1) == 0)
			;
		core_unlock();
		_swix(Hourglass_Off, 0);
		ct.rows[i].page = page;
	}
	if (page >= 0) {
		app_log("contents: '%s' -> page %d", ct.rows[i].title, page + 1);
		docwin_go_to(ct.w, page);
	}
	/* Adjust keeps the window; Select too (it's a list to come back to). */
}
