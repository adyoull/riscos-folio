/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* The Annotations window (Navigate > Annotations...): every highlight,
 * underline, note, drawing and the like in the document, a row each: the
 * page, a swatch of its colour, what kind it is, the note's text or the
 * text it marks, and who made it. Pages are looked through a few at a time
 * in null polls, so a long document doesn't hold the desktop up. A click
 * goes to that annotation and selects it, so Delete and Edit > Colour act
 * on it. The list is looked through again whenever the document changes
 * (any change, undo or redo). One window, for the document it was last
 * opened from. */
#include "app.h"
#include "worker.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define ROW_H    44
#define PAD      8
#define WIDTH    1000
#define MAX_VIS  880
#define MAX_ROWS 5000
#define PAGE_X   80       /* page numbers right-aligned at */
#define SWATCH_X 104
#define KIND_X   152
#define TEXT_X   352

typedef struct {
	int page, n, rgb;
	folio_rect rect;
	char kind[24];        /* desktop alphabet */
	char *text;           /* desktop alphabet: the text, then " - author" */
} row;

static struct {
	ObjectId obj;
	int wimp;
	docwin *w;
	row *rows;
	int n, cap;
	int next_page;        /* the next page to look at; -1 when done */
	int pages;
	int changes;          /* folio_change_count when last looked through */
	int current;          /* the row clicked, or -1 */
	char title[280];
} al;

static ObjectId create_window(void)
{
	static tbx_window t;
	ObjectId id = 0;
	_kernel_oserror *e;
	memset(&t, 0, sizeof t);
	t.flags = (1 << 1) | (1 << 2);
	t.help_message = "This lists the document's annotations: highlights, notes, drawings and so on."
		"|MClick on one to go to it and select it.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.default_focus = -1;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = WIDTH;
	t.window.vis_y1 = 400;
	t.window.behind = -1;
	/* Moveable, back, close, title, vertical scroll, adjust size; Folio draws it. */
	t.window.window_flags = 0x80000002u | (1u << 24) | (1u << 25) | (1u << 26) |
		(1u << 28) | (1u << 29);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 0;
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -400;
	t.window.work_x1 = WIDTH;
	t.window.title_flags = 0x00000119;
	t.window.work_flags = 3 << 12;   /* click */
	t.window.sprite_area = (void *)1;
	t.window.min_width = 300;
	t.window.min_height = 100;
	t.window.title_text = al.title;
	t.window.title_validation = "";
	t.window.title_buflen = sizeof al.title;
	strcpy(al.title, "Annotations");
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	window_get_wimp_handle(id, &al.wimp);
	return id;
}

static void free_rows(void)
{
	int i;
	for (i = 0; i < al.n; i++)
		free(al.rows[i].text);
	al.n = 0;
	al.current = -1;
}

/* MuPDF's names for the kinds, as people say them. */
static const char *kind_name(const char *type)
{
	static const struct { const char *pdf, *say; } k[] = {
		{ "Text", "Note" }, { "FreeText", "Text box" }, { "Ink", "Drawing" },
		{ "Square", "Rectangle" }, { "Circle", "Ellipse" }, { "Squiggly", "Squiggle" },
		{ "StrikeOut", "Strike-out" }, { "FileAttachment", "Attached file" },
		{ "PolyLine", "Lines" }, { "Redact", "Redaction" }, { "Caret", "Insertion" },
	};
	int i;
	for (i = 0; i < (int)(sizeof k / sizeof k[0]); i++)
		if (!strcmp(type, k[i].pdf))
			return k[i].say;
	return type;
}

static void set_extent(void)
{
	int box[4];
	box[0] = 0;
	box[1] = -(PAD * 2 + (al.n ? al.n : 1) * ROW_H);
	box[2] = WIDTH;
	box[3] = 0;
	_swix(Wimp_SetExtent, _INR(0, 1), al.wimp, box);
}

static void redraw_rows(int from, int to)
{
	_swix(Wimp_ForceRedraw, _INR(0, 4), al.wimp, 0, -(PAD + (to + 1) * ROW_H), WIDTH, -(PAD + from * ROW_H));
}

static void set_title(void)
{
	if (!al.w)
		return;
	if (al.next_page >= 0)
		snprintf(al.title, sizeof al.title, "Annotations in %.200s (looking: page %d of %d)",
			docwin_title(al.w), al.next_page + 1, al.pages);
	else
		snprintf(al.title, sizeof al.title, "Annotations in %.200s (%d)", docwin_title(al.w), al.n);
	window_set_title(al.obj, al.title);
}

/* Start looking through the document again. */
static void restart(void)
{
	free_rows();
	al.next_page = 0;
	al.pages = docwin_pages(al.w);
	set_extent();
	redraw_rows(0, 20);
}

void annotlist_open(docwin *w)
{
	int st[9], dst[9];
	if (!al.obj && !(al.obj = create_window()))
		return;
	al.w = w;
	al.changes = -1;
	restart();
	set_title();
	dst[0] = docwin_wimp(w);
	_swix(Wimp_GetWindowState, _IN(1), dst);
	st[0] = al.wimp;
	st[1] = dst[1] + 48;
	st[4] = dst[4] - 48;
	st[3] = st[1] + WIDTH;
	st[2] = st[4] - 400;
	if (st[2] < 64)
		st[2] = 64;
	st[5] = 0;
	st[6] = 0;
	st[7] = -1;
	tbx_show(al.obj, 0, 1, st + 1, 0, -1);
}

void annotlist_forget(docwin *w)
{
	if (al.w != w)
		return;
	al.w = NULL;
	al.next_page = -1;
	free_rows();
	if (al.obj)
		tbx_hide(al.obj);
}

int annotlist_owns(int wimp_handle)
{
	return al.obj && wimp_handle == al.wimp;
}

static int shown(void)
{
	int st[9];
	st[0] = al.wimp;
	return al.obj && al.w && !_swix(Wimp_GetWindowState, _IN(1), st) && (st[8] & (1 << 16));
}

/* Rows for one page's annotations (core lock held). */
static void take_page(folio_doc *doc, int page)
{
	folio_annot_info info[64];
	folio_loc loc;
	int k, i, first = al.n;
	if (folio_loc_from_number(doc, page, &loc))
		return;
	k = folio_annot_list(doc, loc, info, 64);
	for (i = 0; i < k && al.n < MAX_ROWS; i++) {
		row *r;
		char *t, both[300];
		if (al.n >= al.cap) {
			int cap = al.cap ? al.cap * 2 : 64;
			row *nr = realloc(al.rows, sizeof *nr * (size_t)cap);
			if (!nr)
				return;
			al.rows = nr;
			al.cap = cap;
		}
		r = &al.rows[al.n];
		r->page = page;
		r->n = info[i].n;
		r->rgb = info[i].rgb;
		r->rect = info[i].rect;
		snprintf(r->kind, sizeof r->kind, "%s", kind_name(info[i].type));
		if (info[i].author[0])
			snprintf(both, sizeof both, "%s%s%s", info[i].text, info[i].text[0] ? "  - " : "- ", info[i].author);
		else
			snprintf(both, sizeof both, "%s", info[i].text);
		t = app_desktop_text(both);
		r->text = t ? t : calloc(1, 1);
		if (!r->text)
			return;
		al.n++;
	}
	if (al.n > first) {
		set_extent();
		redraw_rows(first, al.n);
	}
}

int annotlist_background(void)
{
	folio_doc *doc;
	unsigned t0, now;
	if (!al.w || !shown())
		return 0;
	if (!core_trylock())
		return al.next_page >= 0;
	doc = docwin_doc(al.w);
	/* Something changed (or was undone): look through it all again. */
	if (al.next_page < 0 && folio_change_count(doc) != al.changes) {
		core_unlock();
		restart();
		if (!core_trylock())
			return 1;
	}
	if (al.next_page == 0)
		al.changes = folio_change_count(doc);
	_swix(OS_ReadMonotonicTime, _OUT(0), &t0);
	while (al.next_page >= 0) {
		if (al.next_page >= al.pages) {
			al.next_page = -1;
			break;
		}
		take_page(doc, al.next_page++);
		_swix(OS_ReadMonotonicTime, _OUT(0), &now);
		if (now - t0 >= 3)
			break;
	}
	core_unlock();
	set_title();
	if (al.next_page < 0 && al.n == 0)
		redraw_rows(0, 1);
	return al.next_page >= 0;
}

static void text_at(const char *s, int x, int y, int fg, int bg, int right)
{
	_swix(Wimp_TextOp, _INR(0, 2), 0, fg, bg);
	_swix(Wimp_TextOp, _INR(0, 5), 2 | (1u << 28) | (right ? (1u << 31) : 0), s, -1, -1, x, y);
}

void annotlist_redraw(int *b)
{
	int more = 0;
	b[0] = al.wimp;
	_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), b, &more);
	while (more) {
		int ox = b[1] - b[5], oy = b[4] - b[6];
		int top = oy - b[10], bottom = oy - b[8];
		int i0 = (top - PAD) / ROW_H, i1 = (bottom - PAD) / ROW_H, i;
		if (i0 < 0)
			i0 = 0;
		if (al.n == 0)
			text_at(al.next_page >= 0 ? "Looking for annotations..." : "This document has no annotations.",
				ox + 16, oy - PAD - ROW_H / 2, 0x55555500, 0xffffff00, 0);
		for (i = i0; i <= i1 && i < al.n; i++) {
			row *r = &al.rows[i];
			int y1 = oy - PAD - i * ROW_H, yc = y1 - ROW_H / 2;
			unsigned fg = 0x00000000, bg = 0xffffff00, grey = 0x55555500;
			char num[16];
			if (i == al.current) {
				bg = 0xffd0a000u;    /* light blue, as in Create PDF */
				_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), bg, 0, 0);
				_swix(OS_Plot, _INR(0, 2), 4, ox, y1 - ROW_H);
				_swix(OS_Plot, _INR(0, 2), 96 + 5, ox + WIDTH, y1 - 2);
			}
			snprintf(num, sizeof num, "%d", r->page + 1);
			text_at(num, ox + PAGE_X, yc, grey, bg, 1);
			if (r->rgb >= 0) {
				unsigned c = ((unsigned)(r->rgb & 0xff) << 24) | ((unsigned)((r->rgb >> 8) & 0xff) << 16) |
					((unsigned)((r->rgb >> 16) & 0xff) << 8);
				_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), c, 0, 0);
				_swix(OS_Plot, _INR(0, 2), 4, ox + SWATCH_X, yc - 14);
				_swix(OS_Plot, _INR(0, 2), 96 + 5, ox + SWATCH_X + 28, yc + 14);
				_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), 0x77777700, 0, 0);
				_swix(OS_Plot, _INR(0, 2), 4, ox + SWATCH_X, yc - 14);
				_swix(OS_Plot, _INR(0, 2), 5, ox + SWATCH_X + 28, yc - 14);
				_swix(OS_Plot, _INR(0, 2), 5, ox + SWATCH_X + 28, yc + 14);
				_swix(OS_Plot, _INR(0, 2), 5, ox + SWATCH_X, yc + 14);
				_swix(OS_Plot, _INR(0, 2), 5, ox + SWATCH_X, yc - 14);
			}
			text_at(r->kind, ox + KIND_X, yc, fg, bg, 0);
			text_at(r->text, ox + TEXT_X, yc, fg, bg, 0);
		}
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}

void annotlist_click(int *b)
{
	int st[9], i, old = al.current;
	if (!al.w)
		return;
	st[0] = al.wimp;
	_swix(Wimp_GetWindowState, _IN(1), st);
	i = (-(b[1] - (st[4] - st[6])) - PAD) / ROW_H;
	if (i < 0 || i >= al.n)
		return;
	al.current = i;
	if (old >= 0)
		redraw_rows(old, old);
	redraw_rows(i, i);
	app_log("annotations: page %d, number %d (%s)", al.rows[i].page + 1, al.rows[i].n, al.rows[i].kind);
	docwin_select_annot(al.w, al.rows[i].page, al.rows[i].n, al.rows[i].rect);
}
