/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Create PDF (icon bar menu): a window to collect files in, in order, and
 * join into one new PDF (core folio_combine). Drag PDFs, pictures or other
 * documents into it (several at once arrive one after another, in the
 * Filer's order); a directory adds its PDFs and pictures in name order.
 * Click a file to choose it; the toolbar moves it up or down, removes it,
 * sorts the list by name or clears it, and Create makes the PDF: a PDF's
 * pages are copied as they are, pictures and JPEGs aren't compressed again.
 * The new PDF opens in a window, unsaved, named "Combined"; the list stays,
 * to change and try again. The window is a Toolbox window Folio draws itself
 * (rows of Wimp_TextOp text) with an internal toolbar of action buttons. */
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define MAX_FILES 1000
#define ROW_H     44
#define PAD       12
#define WIDTH     960
#define TB_H      76
#define NUM_W     80          /* the number column, right-justified */
#define KIND_X    (WIDTH - 200)

enum { C_UP = 0, C_DOWN, C_REMOVE, C_SORT, C_CLEAR, C_CREATE, C_COUNT };

typedef struct {
	int flags, type;
	int x0, y0, x1, y1;
	ComponentId id;
	const char *help;
	int max_help;
} gadget_hdr;

typedef struct {
	gadget_hdr h;
	const char *text;
	int max_text;
	const char *click_show;
	int event;
} button_gadget;

static struct {
	ObjectId obj, tb;
	int wimp, tb_wimp;
	int n, sel;               /* sel: the chosen row, or -1 */
	char *path[MAX_FILES];
	int type[MAX_FILES];
	char title[48];
	button_gadget g[C_COUNT];
} cp;

static const char *leaf_of(const char *path)
{
	const char *p = strrchr(path, '.'), *c = strrchr(path, ':');
	if (c && (!p || c > p))
		p = c;
	return p ? p + 1 : path;
}

static const char *kind_of(int type, const char *leaf)
{
	switch (type) {
	case 0xadf: return "PDF";
	case 0xc85: return "JPEG";
	case 0xb60: return "PNG";
	case 0x695: return "GIF";
	case 0x69c: return "BMP";
	case 0xff0: return "TIFF";
	}
	if (pagesave_is_pdf(type, leaf))
		return "PDF";
	if (pagesave_is_picture(type, leaf))
		return "picture";
	return "document";
}

static void button(button_gadget *b, int id, int x0, int x1, const char *text, int event,
		const char *help)
{
	gadget_hdr *h = &b->h;
	/* Local: without it the Toolbox closes the window after each click
	 * (test49-56 closed it on Up, Down...). */
	h->flags = 1 << 2;
	h->type = ((int)sizeof *b << 16) | 128;
	h->x0 = x0;
	h->y0 = -TB_H + 8;
	h->x1 = x1;
	h->y1 = -8;
	h->id = id;
	h->help = help;
	h->max_help = (int)strlen(help) + 1;
	b->text = text;
	b->max_text = (int)strlen(text) + 1;
	b->click_show = NULL;
	b->event = event;
}

static ObjectId create_toolbar(void)
{
	static tbx_window t;
	ObjectId id = 0;
	_kernel_oserror *e;
	button(&cp.g[C_UP], C_UP, 8, 128, "Up", EV_CP_UP, "Click to move the chosen file up the list.");
	button(&cp.g[C_DOWN], C_DOWN, 136, 256, "Down", EV_CP_DOWN, "Click to move the chosen file down the list.");
	button(&cp.g[C_REMOVE], C_REMOVE, 264, 424, "Remove", EV_CP_REMOVE,
		"Click to take the chosen file off the list (the file itself isn't touched).");
	button(&cp.g[C_SORT], C_SORT, 432, 552, "Sort", EV_CP_SORT,
		"Click to put the list in name order (Page2 before Page10).");
	button(&cp.g[C_CLEAR], C_CLEAR, 560, 680, "Clear", EV_CP_CLEAR, "Click to empty the list.");
	button(&cp.g[C_CREATE], C_CREATE, WIDTH - 248, WIDTH - 16, "Create PDF", EV_CP_CREATE,
		"Click to join the files, in this order, into one new PDF. It opens in a window, to look at and save.");
	memset(&t, 0, sizeof t);
	t.flags = 1 << 4;                       /* a toolbar */
	t.help_message = "These buttons change the list, and make the PDF.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.num_gadgets = C_COUNT;
	t.gadgets = cp.g;
	t.default_focus = -1;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = WIDTH;
	t.window.vis_y1 = TB_H;
	t.window.behind = -1;
	t.window.window_flags = 0x80000000u | (1u << 4);   /* no furniture; auto-redraw */
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 1;
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -TB_H;
	t.window.work_x1 = WIDTH;
	t.window.sprite_area = (void *)1;
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	return id;
}

static ObjectId create_window(void)
{
	static tbx_window t;
	ObjectId id = 0;
	_kernel_oserror *e;
	memset(&t, 0, sizeof t);
	/* The Toolbox closes it; Folio opens it (open requests), so the toolbar
	 * stays a child of it (place_toolbar). */
	t.flags = 1 << 2;
	t.help_message = "Drag PDFs, pictures or other documents here, or a directory of them, in the order "
		"they should go. Click one to choose it.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.default_focus = -1;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = WIDTH;
	t.window.vis_y1 = 560;
	t.window.behind = -1;
	/* Moveable, back, close, title, vertical scroll, adjust size; Folio
	 * draws the list. */
	t.window.window_flags = 0x80000002u | (1u << 24) | (1u << 25) | (1u << 26) | (1u << 28) | (1u << 29);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 0;          /* white, as a list */
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -560;
	t.window.work_x1 = WIDTH;
	t.window.title_flags = 0x00000119;
	t.window.work_flags = 3u << 12;  /* click */
	t.window.sprite_area = (void *)1;
	t.window.title_text = cp.title;
	t.window.title_validation = "";
	t.window.title_buflen = sizeof cp.title;
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	window_get_wimp_handle(id, &cp.wimp);
	return id;
}

/* The title, extent and contents after a change. */
static void update(void)
{
	int box[4], h = TB_H + PAD * 2 + (cp.n > 10 ? cp.n : 10) * ROW_H;
	if (!cp.obj)
		return;
	if (cp.n)
		snprintf(cp.title, sizeof cp.title, "Create PDF (%d file%s)", cp.n, cp.n == 1 ? "" : "s");
	else
		snprintf(cp.title, sizeof cp.title, "Create PDF");
	window_set_title(cp.obj, cp.title);
	box[0] = 0;
	box[1] = -h;
	box[2] = WIDTH;
	box[3] = 0;
	_swix(Wimp_SetExtent, _INR(0, 1), cp.wimp, box);
	_swix(Wimp_ForceRedraw, _INR(0, 4), cp.wimp, 0, -h, WIDTH, 0);
	/* Faded when there's nothing for them to do. */
	{
		int k;
		for (k = 0; k < C_COUNT; k++) {
			int fl = 0, off;
			off = (k == C_UP && cp.sel <= 0) || (k == C_DOWN && (cp.sel < 0 || cp.sel >= cp.n - 1)) ||
				(k == C_REMOVE && cp.sel < 0) || ((k == C_SORT || k == C_CLEAR) && cp.n < (k == C_SORT ? 2 : 1)) ||
				(k == C_CREATE && cp.n == 0);
			tbx_method_out(cp.tb, 64 /* Gadget_GetFlags */, k, &fl);
			fl = off ? fl | (int)(1u << 31) : fl & ~(int)(1u << 31);
			tbx_method(cp.tb, 65 /* Gadget_SetFlags */, k, fl, 0);
		}
	}
}

/* The toolbar as a child window of the list window, its left, right and
 * top edges tied to the window's, so the Wimp moves and stacks it with the
 * window. (The Toolbox shows an internal toolbar as a separate pane: when
 * the new PDF's window opened over it and the list window was brought back
 * to the front, the toolbar stayed behind and the list's white showed in
 * its place.) If the Wimp can't nest windows, it's opened as a pane just
 * in front of the window instead, on every open request. */
static int nested, nest_failed;

static void place_toolbar(void)
{
	int ps[9], st[9];
	ps[0] = cp.wimp;
	st[0] = cp.tb_wimp;
	nested = 0;
	if (!cp.tb_wimp || _swix(Wimp_GetWindowState, _IN(1), ps) || !(ps[8] & (1 << 16)) ||
			_swix(Wimp_GetWindowState, _IN(1), st))
		return;
	st[1] = ps[1];
	st[3] = ps[3];
	st[4] = ps[4];
	st[2] = ps[4] - TB_H;
	st[5] = 0;
	st[6] = 0;
	st[7] = -1;
	if (!nest_failed) {
		if (!_swix(Wimp_OpenWindow, _INR(1, 4), st, 0x4b534154 /* TASK */, cp.wimp,
				(1u << 16) | (2u << 18) | (2u << 20) | (2u << 22) | (1u << 24) | (2u << 26))) {
			nested = 1;
			return;
		}
		nest_failed = 1;
		app_log("create PDF: the toolbar can't be nested; a pane instead");
	}
	/* A pane: just in front of the window. */
	st[7] = ps[7];
	_swix(Wimp_OpenWindow, _IN(1), st);
	ps[7] = cp.tb_wimp;
	_swix(Wimp_OpenWindow, _IN(1), ps);
}

void combine_open_request(int *b)
{
	if (!nested && cp.tb_wimp) {
		int st[9];
		st[0] = cp.tb_wimp;
		if (!_swix(Wimp_GetWindowState, _IN(1), st)) {
			st[1] = b[1];
			st[3] = b[3];
			st[4] = b[4];
			st[2] = b[4] - TB_H;
			if (b[7] != cp.tb_wimp)
				st[7] = b[7];
			_swix(Wimp_OpenWindow, _IN(1), st);
			b[7] = cp.tb_wimp;
		}
	}
	_swix(Wimp_OpenWindow, _IN(1), b);
}

void combine_open(void)
{
	int pos[2];
	if (!cp.obj) {
		snprintf(cp.title, sizeof cp.title, "Create PDF");
		cp.sel = -1;
		if (!(cp.tb = create_toolbar()))
			return;
		window_get_wimp_handle(cp.tb, &cp.tb_wimp);
		if (!(cp.obj = create_window()))
			return;
		/* Window_SetToolBars: internal top-left (User Interface Toolbox p336). */
		_swix(Toolbox_ObjectMiscOp, _INR(0, 6), 2, cp.obj, 18, 0, cp.tb, 0, 0);
		pos[0] = (app.screen_w - WIDTH) / 2;
		pos[1] = app.screen_h / 2 + 300;
		update();
		tbx_show(cp.obj, 0, 2, pos, 0, -1);
		place_toolbar();
		return;
	}
	update();
	tbx_show(cp.obj, 0, 0, NULL, 0, -1);
	place_toolbar();
}

int combine_owns(int wimp)
{
	return cp.obj && wimp == cp.wimp;
}

static void text_at(const char *s, int x, int y, unsigned fg, unsigned bg, int right)
{
	_swix(Wimp_TextOp, _INR(0, 2), 0, fg, bg);
	_swix(Wimp_TextOp, _INR(0, 5), 2 | (1u << 28) | (right ? (1u << 31) : 0), s, -1, -1, x, y);
}

void combine_redraw(int *b)
{
	int more = 0;
	b[0] = cp.wimp;
	_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), b, &more);
	while (more) {
		int ox = b[1] - b[5], oy = b[4] - b[6], i;
		/* Only the rows in this rectangle. */
		int top = oy - TB_H - PAD, first = (top - b[10]) / ROW_H, last = (top - b[8]) / ROW_H + 1;
		if (first < 0)
			first = 0;
		if (last > cp.n)
			last = cp.n;
		if (cp.n == 0)
			text_at("Drag PDFs, pictures or other documents here, or a directory of them,", ox + PAD * 2,
				top - ROW_H / 2, 0x77777700u, 0xffffff00u, 0),
			text_at("in the order they should go. Then click Create PDF.", ox + PAD * 2,
				top - ROW_H - ROW_H / 2, 0x77777700u, 0xffffff00u, 0);
		for (i = first; i < last; i++) {
			int yt = top - i * ROW_H, yc = yt - ROW_H / 2;
			unsigned bg = 0xffffff00u;
			char num[12], name[200];
			const char *leaf = leaf_of(cp.path[i]);
			if (i == cp.sel) {
				bg = 0xffd0a000u;     /* light blue */
				_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), bg, 0, 0);
				_swix(OS_Plot, _INR(0, 2), 4, ox, yt - ROW_H);
				_swix(OS_Plot, _INR(0, 2), 101, ox + WIDTH, yt - 2);
			}
			snprintf(num, sizeof num, "%d.", i + 1);
			snprintf(name, sizeof name, "%.199s", leaf);
			text_at(num, ox + NUM_W, yc, 0x55555500u, bg, 1);
			text_at(name, ox + NUM_W + 16, yc, 0x00000000u, bg, 0);
			text_at(kind_of(cp.type[i], leaf), ox + KIND_X, yc, 0x55555500u, bg, 0);
		}
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}

void combine_click(int *b)
{
	int st[9], row;
	st[0] = cp.wimp;
	_swix(Wimp_GetWindowState, _IN(1), st);
	/* Work area y of the click, then the row under it. */
	row = (-(b[1] - (st[4] - st[6])) - TB_H - PAD) / ROW_H;
	if (b[2] & 1)                 /* Adjust: no row chosen */
		row = -1;
	else if (row < 0 || row >= cp.n)
		row = -1;
	if (row != cp.sel) {
		cp.sel = row;
		update();
	}
}

static void add(const char *path, int type)
{
	if (cp.n >= MAX_FILES) {
		tbx_report("The list is full.");
		return;
	}
	cp.path[cp.n] = malloc(strlen(path) + 1);
	if (!cp.path[cp.n])
		return;
	strcpy(cp.path[cp.n], path);
	cp.type[cp.n] = type;
	cp.n++;
}

int combine_data_load(int *b)
{
	const char *path = (const char *)(b + 11);
	int type = b[10];
	if (!cp.obj || (b[5] != cp.wimp && b[5] != cp.tb_wimp))
		return 0;
	if (type == 0x1000) {
		char **names;
		int n = pagesave_list(path, 1, &names), i;
		for (i = 0; i < n; i++) {
			int t = 0, load = 0;
			if (!_swix(OS_File, _INR(0, 1) | _OUT(2), 17, names[i], &load) &&
					((unsigned)load & 0xfff00000u) == 0xfff00000u)
				t = (load >> 8) & 0xfff;
			add(names[i], t ? t : -1);
		}
		pagesave_free_list(names, n);
		if (n == 0) {
			char m[300];
			snprintf(m, sizeof m, "There are no PDFs or pictures in '%s'.", leaf_of(path));
			tbx_report(m);
		}
	} else if (type == 0x2000) {
		return 1;                     /* an application: not a document */
	} else {
		add(path, type);
	}
	app_log("create PDF: %d files after adding %s", cp.n, path);
	update();
	return 1;
}

static int cmp_names(const void *a, const void *b)
{
	return pagesave_natural(leaf_of(*(char *const *)a), leaf_of(*(char *const *)b));
}

static void create(void)
{
	char out[600];
	int ok;
	docwin *w;
	if (cp.n == 0)
		return;
	pagesave_new_path("Combined", out, sizeof out);
	_swix(Hourglass_On, 0);
	core_now();
	ok = folio_combine(app.fc, (const char *const *)cp.path, cp.n, out) == 0;
	core_unlock();
	_swix(Hourglass_Off, 0);
	app_log("create PDF: %d files to %s%s%s", cp.n, out, ok ? "" : ": ", ok ? "" : folio_error(app.fc));
	if (!ok) {
		char m[400];
		snprintf(m, sizeof m, "Folio couldn't make the PDF: %s", folio_error(app.fc));
		tbx_report(m);
		return;
	}
	_swix(OS_File, _INR(0, 2), 18, out, 0xadf);
	w = docwin_open(out);
	if (w)
		docwin_mark_new(w);
}

void combine_event(int code)
{
	int i;
	switch (code) {
	case EV_CREATE_PDF:
		combine_open();
		return;
	case EV_CP_UP:
	case EV_CP_DOWN: {
		int to = cp.sel + (code == EV_CP_UP ? -1 : 1);
		char *p;
		int t;
		if (cp.sel < 0 || to < 0 || to >= cp.n)
			return;
		p = cp.path[to]; cp.path[to] = cp.path[cp.sel]; cp.path[cp.sel] = p;
		t = cp.type[to]; cp.type[to] = cp.type[cp.sel]; cp.type[cp.sel] = t;
		cp.sel = to;
		break;
	}
	case EV_CP_REMOVE:
		if (cp.sel < 0)
			return;
		free(cp.path[cp.sel]);
		for (i = cp.sel; i < cp.n - 1; i++) {
			cp.path[i] = cp.path[i + 1];
			cp.type[i] = cp.type[i + 1];
		}
		cp.n--;
		if (cp.sel >= cp.n)
			cp.sel = cp.n - 1;
		break;
	case EV_CP_SORT: {
		/* Keep the types with their files: sort an index. */
		char *paths[MAX_FILES];
		int types[MAX_FILES], j;
		memcpy(paths, cp.path, sizeof(char *) * (size_t)cp.n);
		qsort(paths, (size_t)cp.n, sizeof *paths, cmp_names);
		for (i = 0; i < cp.n; i++)
			for (j = 0; j < cp.n; j++)
				if (cp.path[j] == paths[i]) {
					types[i] = cp.type[j];
					break;
				}
		memcpy(cp.path, paths, sizeof(char *) * (size_t)cp.n);
		memcpy(cp.type, types, sizeof(int) * (size_t)cp.n);
		cp.sel = -1;
		break;
	}
	case EV_CP_CLEAR:
		for (i = 0; i < cp.n; i++)
			free(cp.path[i]);
		cp.n = 0;
		cp.sel = -1;
		break;
	case EV_CP_CREATE:
		create();
		return;
	default:
		return;
	}
	update();
}
