/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* The Form fields window (Edit > Form fields...), for XFA forms (core/xfa.c):
 * a row per field, its caption and its value. A click changes it: a check
 * box ticks or unticks, a choice (or radio group) gives a menu, anything
 * else opens a small box to type in. Read-only fields are grey. A dynamic
 * XFA form, which MuPDF can't draw (its page says "Please wait..."), opens
 * this window by itself. The list is read again whenever the document
 * changes (a change, undo or redo, or a field filled in on the page). */
#include "app.h"
#include "worker.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define ROW_H    44
#define PAD      8
#define WIDTH    1200
#define LABEL_X  440      /* captions right-aligned at */
#define VALUE_X  464
#define MAX_FIELDS 2000

typedef struct {
	folio_xfa_field f;
	char *label, *shown;  /* desktop alphabet */
} row;

static struct {
	ObjectId obj;
	int wimp;
	docwin *w;
	row *rows;
	int n;
	int changes;
	int current;
	char title[280];
	/* The typing box. */
	ObjectId edit;
	int editing;          /* the row being typed in, or -1 */
	char label_buf[200], value_buf[512];
	/* The choices menu. */
	int menu_row;         /* or -1 */
	char menu_text[FOLIO_XFA_OPTS][64];
} xu = { .current = -1, .editing = -1, .menu_row = -1 };

static int menu[7 + 6 * FOLIO_XFA_OPTS];

static ObjectId create_window(void)
{
	static tbx_window t;
	ObjectId id = 0;
	_kernel_oserror *e;
	memset(&t, 0, sizeof t);
	t.flags = (1 << 1) | (1 << 2);
	t.help_message = "This lists the form's fields and what's in them."
		"|MClick on one to fill it in: type, choose, or tick.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.default_focus = -1;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = WIDTH;
	t.window.vis_y1 = 600;
	t.window.behind = -1;
	t.window.window_flags = 0x80000002u | (1u << 24) | (1u << 25) | (1u << 26) |
		(1u << 28) | (1u << 29);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 0;
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -600;
	t.window.work_x1 = WIDTH;
	t.window.title_flags = 0x00000119;
	t.window.work_flags = 3 << 12;   /* click */
	t.window.sprite_area = (void *)1;
	t.window.min_width = 400;
	t.window.min_height = 100;
	t.window.title_text = xu.title;
	t.window.title_validation = "";
	t.window.title_buflen = sizeof xu.title;
	strcpy(xu.title, "Form fields");
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	window_get_wimp_handle(id, &xu.wimp);
	return id;
}

/* ---- The typing box ------------------------------------------------------------ */

typedef struct {
	int flags, type;
	int x0, y0, x1, y1;
	ComponentId id;
	const char *help;
	int max_help;
} gadget_hdr;

typedef struct { gadget_hdr h; char *text; int max_text; } display_gadget;
typedef struct {
	gadget_hdr h;
	char *text;
	int max_text;
	const char *allowable;
	int max_allowable;
	ComponentId before, after;
} writable_gadget;
typedef struct {
	gadget_hdr h;
	const char *text;
	int max_text;
	const char *click_show;
	int event;
} button_gadget;

enum { E_LABEL = 0, E_VALUE, E_CANCEL, E_OK };

static struct {
	display_gadget label;
	writable_gadget value;
	button_gadget cancel, ok;
} eg;

#define EB_W 900
#define EB_H 200

static void hdr(gadget_hdr *h, int type, int size, int id, int x0, int y0, int x1, int y1, const char *help)
{
	h->flags = 0;
	h->type = (size << 16) | type;
	h->x0 = x0; h->y0 = y0; h->x1 = x1; h->y1 = y1;
	h->id = id;
	h->help = help;
	h->max_help = help ? (int)strlen(help) + 1 : 0;
}

static ObjectId create_edit(void)
{
	static tbx_window t;
	static char title[] = "Fill in";
	ObjectId id = 0;
	_kernel_oserror *e;
	int by = -EB_H + 16;
	memset(&eg, 0, sizeof eg);
	hdr(&eg.label.h, 448, sizeof eg.label, E_LABEL, 16, -84, EB_W - 16, -36, NULL);
	eg.label.text = xu.label_buf;
	eg.label.max_text = sizeof xu.label_buf;
	hdr(&eg.value.h, 512, sizeof eg.value, E_VALUE, 16, -148, EB_W - 16, -92,
		"Type what goes in the field, then press Return.");
	eg.value.text = xu.value_buf;
	eg.value.max_text = sizeof xu.value_buf;
	eg.value.before = eg.value.after = -1;
	hdr(&eg.cancel.h, 128, sizeof eg.cancel, E_CANCEL, EB_W - 360, by, EB_W - 200, by + 68,
		"Click to leave the field as it was.");
	eg.cancel.h.flags = 2;
	eg.cancel.text = "Cancel";
	eg.cancel.max_text = 7;
	eg.cancel.event = EV_XF_CANCEL;
	hdr(&eg.ok.h, 128, sizeof eg.ok, E_OK, EB_W - 184, by, EB_W - 24, by + 68,
		"Click (or press Return) to put this in the field.");
	eg.ok.h.flags = 1;
	eg.ok.text = "OK";
	eg.ok.max_text = 3;
	eg.ok.event = EV_XF_OK;
	memset(&t, 0, sizeof t);
	t.flags = (1 << 1) | (1 << 2);
	t.help_message = "Type the field's new value here.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.num_gadgets = 4;
	t.gadgets = &eg;
	t.default_focus = E_VALUE;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = EB_W;
	t.window.vis_y1 = EB_H;
	t.window.behind = -1;
	t.window.window_flags = 0x80000002u | (1u << 4) | (1u << 24) | (1u << 25) | (1u << 26);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 1;
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -EB_H;
	t.window.work_x1 = EB_W;
	t.window.title_flags = 0x00000119;
	t.window.sprite_area = (void *)1;
	t.window.title_text = title;
	t.window.title_validation = "";
	t.window.title_buflen = sizeof title;
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	return id;
}

/* ---- The list ------------------------------------------------------------------ */

static void free_rows(void)
{
	int i;
	for (i = 0; i < xu.n; i++) {
		free(xu.rows[i].label);
		free(xu.rows[i].shown);
	}
	free(xu.rows);
	xu.rows = NULL;
	xu.n = 0;
}

/* What a row shows for its value (UTF-8). */
static void shown_value(const folio_xfa_field *f, char *out, size_t size)
{
	if (f->type == FOLIO_XFA_CHECK)
		snprintf(out, size, "%s", f->nopts && !strcmp(f->value, f->save[0]) ? "Yes (ticked)" : "No");
	else if (f->type == FOLIO_XFA_CHOICE && f->chosen >= 0)
		snprintf(out, size, "%s", f->opt[f->chosen]);
	else if (f->type == FOLIO_XFA_PASSWORD && f->value[0])
		snprintf(out, size, "%s", "********");
	else {
		char *p;
		snprintf(out, size, "%s", f->value);
		for (p = out; *p; p++)
			if (*p == '\n' || *p == '\r' || *p == '\t')
				*p = ' ';
	}
}

static void set_extent(void)
{
	int box[4];
	box[0] = 0;
	box[1] = -(PAD * 2 + (xu.n ? xu.n : 1) * ROW_H);
	box[2] = WIDTH;
	box[3] = 0;
	_swix(Wimp_SetExtent, _INR(0, 1), xu.wimp, box);
}

static void redraw_all(void)
{
	_swix(Wimp_ForceRedraw, _INR(0, 4), xu.wimp, 0, -(PAD * 2 + (xu.n + 1) * ROW_H) - 4000, WIDTH, 0);
}

/* Reads the fields again. */
static void refresh(void)
{
	folio_doc *doc = docwin_doc(xu.w);
	folio_xfa_field *f = malloc(sizeof *f * 64);
	int n, cap = 64, i;
	if (!f)
		return;
	core_now();
	n = folio_xfa_fields(doc, f, cap);
	if (n > cap) {
		folio_xfa_field *g;
		cap = n < MAX_FIELDS ? n : MAX_FIELDS;
		g = realloc(f, sizeof *f * (size_t)cap);
		if (g) {
			f = g;
			n = folio_xfa_fields(doc, f, cap);
		}
	}
	xu.changes = folio_change_count(doc);
	core_unlock();
	if (n > cap)
		n = cap;
	free_rows();
	if (n > 0)
		xu.rows = calloc((size_t)n, sizeof *xu.rows);
	for (i = 0; xu.rows && i < n; i++) {
		char v[600];
		xu.rows[i].f = f[i];
		xu.rows[i].label = app_desktop_text(f[i].label);
		shown_value(&f[i], v, sizeof v);
		xu.rows[i].shown = app_desktop_text(v);
	}
	xu.n = xu.rows ? (n > 0 ? n : 0) : 0;
	free(f);
	if (xu.current >= xu.n)
		xu.current = -1;
	snprintf(xu.title, sizeof xu.title, "Form fields in %.200s (%d)", docwin_title(xu.w), xu.n);
	window_set_title(xu.obj, xu.title);
	set_extent();
	redraw_all();
}

void xfaui_open(docwin *w)
{
	int st[9], dst[9];
	if (!xu.obj && !(xu.obj = create_window()))
		return;
	xu.w = w;
	xu.current = -1;
	refresh();
	dst[0] = docwin_wimp(w);
	_swix(Wimp_GetWindowState, _IN(1), dst);
	st[1] = dst[1] + 48;
	st[4] = dst[4] - 48;
	st[3] = st[1] + WIDTH;
	st[2] = st[4] - 600;
	if (st[2] < 64)
		st[2] = 64;
	st[5] = 0;
	st[6] = 0;
	st[7] = -1;
	tbx_show(xu.obj, 0, 1, st + 1, 0, -1);
	app_log("xfa: %d fields", xu.n);
}

void xfaui_forget(docwin *w)
{
	if (xu.w != w)
		return;
	xu.w = NULL;
	xu.menu_row = -1;
	xu.editing = -1;
	free_rows();
	if (xu.obj)
		tbx_hide(xu.obj);
	if (xu.edit)
		tbx_hide(xu.edit);
}

int xfaui_owns(int wimp_handle)
{
	return xu.obj && wimp_handle == xu.wimp;
}

static int shown(void)
{
	int st[9];
	st[0] = xu.wimp;
	return xu.obj && xu.w && !_swix(Wimp_GetWindowState, _IN(1), st) && (st[8] & (1 << 16));
}

/* After any change to the document: the list again. */
void xfaui_background(void)
{
	int c;
	if (!xu.w || !shown() || !core_trylock())
		return;
	c = folio_change_count(docwin_doc(xu.w));
	core_unlock();
	if (c != xu.changes)
		refresh();
}

static void text_at(const char *s, int x, int y, unsigned fg, unsigned bg, int right)
{
	_swix(Wimp_TextOp, _INR(0, 2), 0, fg, bg);
	_swix(Wimp_TextOp, _INR(0, 5), 2 | (1u << 28) | (right ? (1u << 31) : 0), s, -1, -1, x, y);
}

void xfaui_redraw(int *b)
{
	int more = 0;
	b[0] = xu.wimp;
	_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), b, &more);
	while (more) {
		int ox = b[1] - b[5], oy = b[4] - b[6];
		int top = oy - b[10], bottom = oy - b[8];
		int i0 = (top - PAD) / ROW_H, i1 = (bottom - PAD) / ROW_H, i;
		if (i0 < 0)
			i0 = 0;
		if (xu.n == 0)
			text_at("This form has no fields Folio can fill in.", ox + 16, oy - PAD - ROW_H / 2,
				0x55555500, 0xffffff00, 0);
		for (i = i0; i <= i1 && i < xu.n; i++) {
			row *r = &xu.rows[i];
			int y1 = oy - PAD - i * ROW_H, yc = y1 - ROW_H / 2;
			unsigned bg = 0xffffff00, grey = 0x55555500;
			if (i == xu.current) {
				bg = 0xffd0a000u;
				_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), bg, 0, 0);
				_swix(OS_Plot, _INR(0, 2), 4, ox, y1 - ROW_H);
				_swix(OS_Plot, _INR(0, 2), 96 + 5, ox + WIDTH, y1 - 2);
			}
			text_at(r->label ? r->label : r->f.label, ox + LABEL_X, yc, grey, bg, 1);
			/* The value in a white box, as a field is. */
			_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), 0xbbbbbb00, 0, 0);
			_swix(OS_Plot, _INR(0, 2), 4, ox + VALUE_X - 8, yc - 18);
			_swix(OS_Plot, _INR(0, 2), 5, ox + WIDTH - 16, yc - 18);
			_swix(OS_Plot, _INR(0, 2), 5, ox + WIDTH - 16, yc + 18);
			_swix(OS_Plot, _INR(0, 2), 5, ox + VALUE_X - 8, yc + 18);
			_swix(OS_Plot, _INR(0, 2), 5, ox + VALUE_X - 8, yc - 18);
			text_at(r->shown ? r->shown : r->f.value, ox + VALUE_X, yc, r->f.read_only ? grey : 0, bg, 0);
		}
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}

static void set_value(int i, const char *utf8)
{
	char path[256];
	if (!xu.w || i < 0 || i >= xu.n)
		return;
	snprintf(path, sizeof path, "%s", xu.rows[i].f.path);
	if (docwin_xfa_set(xu.w, path, utf8) == 0)
		refresh();
}

static void open_menu(int i, int x, int y)
{
	folio_xfa_field *f = &xu.rows[i].f;
	int k, n = f->nopts, width = 4;
	if (n <= 0)
		return;
	memset(menu, 0, sizeof menu);
	memcpy(menu, "Choose", 7);
	((unsigned char *)menu)[12] = 7;
	((unsigned char *)menu)[13] = 2;
	((unsigned char *)menu)[14] = 7;
	((unsigned char *)menu)[15] = 0;
	for (k = 0; k < n; k++) {
		char *d = app_desktop_text(f->opt[k]);
		snprintf(xu.menu_text[k], sizeof xu.menu_text[k], "%s", d ? d : f->opt[k]);
		free(d);
		if ((int)strlen(xu.menu_text[k]) > width)
			width = (int)strlen(xu.menu_text[k]);
	}
	menu[4] = (width + 2) * 16;
	menu[5] = 44;
	menu[6] = 0;
	for (k = 0; k < n; k++) {
		int *it = &menu[7 + k * 6];
		it[0] = (k == n - 1 ? 0x80 : 0) | (k == f->chosen ? 1 : 0);
		it[1] = -1;
		it[2] = 1 | (1 << 8) | (7 << 24);
		it[3] = (int)xu.menu_text[k];
		it[4] = -1;
		it[5] = (int)strlen(xu.menu_text[k]) + 1;
	}
	xu.menu_row = i;
	_swix(Wimp_CreateMenu, _INR(1, 3), menu, x - 64, y);
}

void xfaui_menus_deleted(int tree)
{
	if (tree == (int)menu)
		xu.menu_row = -1;
}

int xfaui_menu_selection(int *block)
{
	int i = xu.menu_row;
	if (i < 0)
		return 0;
	xu.menu_row = -1;
	if (i < xu.n && block[0] >= 0 && block[0] < xu.rows[i].f.nopts)
		set_value(i, xu.rows[i].f.save[block[0]]);
	return 1;
}

static void open_edit(int i)
{
	int dst[9], pos[2];
	char *d;
	if (!xu.edit && !(xu.edit = create_edit()))
		return;
	xu.editing = i;
	d = app_desktop_text(xu.rows[i].f.label);
	tbx_method(xu.edit, 448, E_LABEL, (int)(d ? d : xu.rows[i].f.label), 0);
	free(d);
	d = app_desktop_text(xu.rows[i].f.value);
	{
		char v[512], *p;
		snprintf(v, sizeof v, "%s", d ? d : xu.rows[i].f.value);
		for (p = v; *p; p++)
			if (*p == '\n' || *p == '\r')
				*p = ' ';
		tbx_method(xu.edit, 512, E_VALUE, (int)v, 0);
	}
	free(d);
	dst[0] = xu.wimp;
	_swix(Wimp_GetWindowState, _IN(1), dst);
	pos[0] = dst[1] + 64;
	pos[1] = dst[4] - 64;
	tbx_show(xu.edit, 1, 2, pos, 0, -1);
}

void xfaui_click(int *b)
{
	int st[9], i, old = xu.current;
	folio_xfa_field *f;
	if (!xu.w)
		return;
	st[0] = xu.wimp;
	_swix(Wimp_GetWindowState, _IN(1), st);
	i = (-(b[1] - (st[4] - st[6])) - PAD) / ROW_H;
	if (i < 0 || i >= xu.n)
		return;
	xu.current = i;
	if (old != i)
		redraw_all();
	f = &xu.rows[i].f;
	app_log("xfa: %s (type %d)", f->path, f->type);
	if (f->read_only) {
		tbx_report("That field can't be changed.");
		return;
	}
	switch (f->type) {
	case FOLIO_XFA_CHECK:
		set_value(i, f->nopts && !strcmp(f->value, f->save[0]) ? (f->nopts > 1 ? f->save[1] : "") : f->save[0]);
		break;
	case FOLIO_XFA_CHOICE:
		if (f->nopts)
			open_menu(i, b[0], b[1]);
		else
			open_edit(i);        /* a choice list with no items is typed in */
		break;
	default:
		open_edit(i);
		break;
	}
}

void xfaui_event(int code)
{
	char v[512], *u;
	int i = xu.editing;
	if (code == EV_XF_CANCEL || i < 0 || i >= xu.n) {
		xu.editing = -1;
		if (xu.edit)
			tbx_hide(xu.edit);
		return;
	}
	v[0] = 0;
	_swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, xu.edit, 513, E_VALUE, v, (int)sizeof v);
	v[sizeof v - 1] = 0;
	u = app_utf8_text(v);
	xu.editing = -1;
	tbx_hide(xu.edit);
	set_value(i, u ? u : v);
	free(u);
}
