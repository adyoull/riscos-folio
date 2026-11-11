/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Pages > Overlay: lays the first page of another file over (or under) the
 * chosen pages of a PDF. The box takes the pages, how far to move the
 * overlay (mm right and down; 0, 0 puts it exactly where it is on its own
 * page, top left to top left) and in front or behind; then the file is
 * dragged into it, as for Insert pages. A drawing printed from Draw to a
 * PDF the same size as the pages lands where it was drawn. One step in the
 * undo history (folio_overlay). */
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

typedef struct {
	int flags, type;
	int x0, y0, x1, y1;
	ComponentId id;
	const char *help;
	int max_help;
} gadget_hdr;

typedef struct { gadget_hdr h; const char *label; } label_gadget;
typedef struct {
	gadget_hdr h;
	char *text;
	int max_text;
	const char *allowable;
	int max_allowable;
	ComponentId before, after;
} writable_gadget;
typedef struct { gadget_hdr h; char *text; int max_text; } display_gadget;
typedef struct { gadget_hdr h; int group; const char *label; int max_label; int event; } radio_gadget;
typedef struct {
	gadget_hdr h;
	const char *text;
	int max_text;
	const char *click_show;
	int event;
} button_gadget;

enum { O_PAGES_L = 0, O_PAGES, O_OF, O_RIGHT_L, O_RIGHT, O_DOWN_L, O_DOWN, O_MM, O_FRONT, O_BEHIND, O_HINT,
	O_CANCEL, O_COUNT };

static struct {
	label_gadget pages_l;
	writable_gadget pages;
	display_gadget of;
	label_gadget right_l;
	writable_gadget right;
	label_gadget down_l;
	writable_gadget down;
	label_gadget mm;
	radio_gadget front, behind;
	label_gadget hint;
	button_gadget cancel;
} g;

static struct {
	ObjectId obj;
	docwin *w;
	char pages[64], of[40], right[12], down[12];
} ov;

#define BOX_W 760
#define BOX_H 376

static void hdr(gadget_hdr *h, int type, int size, int id, int x0, int y0, int x1, int y1, const char *help)
{
	h->flags = 0;
	h->type = (size << 16) | type;
	h->x0 = x0; h->y0 = y0; h->x1 = x1; h->y1 = y1;
	h->id = id;
	h->help = help;
	h->max_help = help ? (int)strlen(help) + 1 : 0;
}

static void label(label_gadget *l, int id, int x0, int y0, int x1, const char *text)
{
	hdr(&l->h, 320, sizeof *l, id, x0, y0, x1, y0 + 48, NULL);
	l->h.flags = 1;                   /* no box */
	l->label = text;
}

static void writable(writable_gadget *w, int id, int x0, int y0, int x1, char *buf, int size, const char *allow,
		int before, int after, const char *help)
{
	hdr(&w->h, 512, sizeof *w, id, x0, y0 - 4, x1, y0 + 52, help);
	w->text = buf;
	w->max_text = size;
	w->allowable = allow;
	w->max_allowable = (int)strlen(allow) + 1;
	w->before = before;
	w->after = after;
}

static void radio(radio_gadget *r, int id, int x0, int x1, const char *text, const char *help)
{
	hdr(&r->h, 384, sizeof *r, id, x0, -212, x1, -164, help);
	r->group = 1;
	r->label = text;
	r->max_label = (int)strlen(text) + 1;
	r->event = 0;
}

static ObjectId create_box(void)
{
	static tbx_window t;
	static char title[] = "Overlay";
	ObjectId id = 0;
	_kernel_oserror *e;

	memset(&g, 0, sizeof g);
	label(&g.pages_l, O_PAGES_L, 16, -84, 136, "Pages");
	writable(&g.pages, O_PAGES, 136, -84, 520, ov.pages, sizeof ov.pages, "0-9,\\- ", -1, O_RIGHT,
		"Type the pages to lay it over, such as 1-3, 20, 45-50.");
	hdr(&g.of.h, 448, sizeof g.of, O_OF, 528, -84, BOX_W - 16, -36, NULL);
	g.of.text = ov.of;
	g.of.max_text = sizeof ov.of;
	label(&g.right_l, O_RIGHT_L, 16, -148, 196, "Move right");
	writable(&g.right, O_RIGHT, 196, -148, 312, ov.right, sizeof ov.right, "\\-0-9.", O_PAGES, O_DOWN,
		"Type how far to move it right, in mm (a minus number moves it left). 0 leaves it where it is on "
		"its own page.");
	label(&g.down_l, O_DOWN_L, 328, -148, 488, "mm, down");
	writable(&g.down, O_DOWN, 488, -148, 604, ov.down, sizeof ov.down, "\\-0-9.", O_RIGHT, -1,
		"Type how far to move it down, in mm (a minus number moves it up).");
	label(&g.mm, O_MM, 620, -148, BOX_W - 16, "mm");
	radio(&g.front, O_FRONT, 16, 248, "In front", "Click to put it over the page's own drawing.");
	radio(&g.behind, O_BEHIND, 264, 496, "Behind", "Click to put it under the page's own drawing, like headed paper.");
	label(&g.hint, O_HINT, 16, -276, BOX_W - 16, "Now drag the PDF or picture here.");
	g.hint.h.help = "Drag a PDF (such as a drawing printed from Draw to PDF) or a picture into this box. "
		"Its first page goes on the pages, its top left on each page's top left.";
	g.hint.h.max_help = (int)strlen(g.hint.h.help) + 1;
	hdr(&g.cancel.h, 128, sizeof g.cancel, O_CANCEL, BOX_W - 196, -BOX_H + 16, BOX_W - 16, -BOX_H + 84,
		"Click to close this box.");
	g.cancel.h.flags = 2;             /* Escape */
	g.cancel.text = "Cancel";
	g.cancel.max_text = 7;
	g.cancel.event = EV_OV_CANCEL;

	memset(&t, 0, sizeof t);
	t.flags = (1 << 1) | (1 << 2);
	t.help_message = "Use this box to lay a page from another file over the PDF's pages: drag the file here.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.num_gadgets = O_COUNT;
	t.gadgets = &g;
	t.default_focus = O_PAGES;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = BOX_W;
	t.window.vis_y1 = BOX_H;
	t.window.behind = -1;
	/* Moveable, back, close, title; auto-redraw. */
	t.window.window_flags = 0x80000002u | (1u << 4) | (1u << 24) | (1u << 25) | (1u << 26);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 1;
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -BOX_H;
	t.window.work_x1 = BOX_W;
	t.window.title_flags = 0x00000119;
	t.window.work_flags = 0;
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

void overlay_open(docwin *w, int page)
{
	int dst[9], pos[2], n = docwin_pages(w);
	char text[40];
	(void)page;
	if (docwin_is_counting(w)) {
		tbx_report("Folio is still counting this document's pages. Try again in a moment.");
		return;
	}
	if (!ov.obj && !(ov.obj = create_box()))
		return;
	ov.w = w;
	snprintf(text, sizeof text, "of %d", n);
	tbx_method(ov.obj, 448 /* DisplayField_SetValue */, O_OF, (int)text, 0);
	snprintf(text, sizeof text, n > 1 ? "1-%d" : "1", n);
	tbx_method(ov.obj, 512 /* WritableField_SetValue */, O_PAGES, (int)text, 0);
	tbx_method(ov.obj, 512, O_RIGHT, (int)"0", 0);
	tbx_method(ov.obj, 512, O_DOWN, (int)"0", 0);
	tbx_method(ov.obj, 388 /* RadioButton_SetState */, O_FRONT, 1, 0);
	dst[0] = docwin_wimp(w);
	_swix(Wimp_GetWindowState, _IN(1), dst);
	pos[0] = dst[1] + 64;
	pos[1] = dst[4] - 64;
	/* Not transient: it stays open for the drag. */
	tbx_show(ov.obj, 0, 2, pos, 0, -1);
}

void overlay_forget(docwin *w)
{
	if (ov.w != w)
		return;
	ov.w = NULL;
	if (ov.obj)
		tbx_hide(ov.obj);
}

void overlay_event(int code)
{
	if (code == EV_OV_CANCEL && ov.obj)
		tbx_hide(ov.obj);
}

static void get_field(int id, char *out, int size)
{
	out[0] = 0;
	_swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, ov.obj, 513 /* WritableField_GetValue */, id, out, size);
	out[size - 1] = 0;
}

/* A file dropped on the box (Message_DataLoad). 1 if it was ours. */
int overlay_data_load(int *b)
{
	char text[64];
	int wimp = 0, count, n, behind = 0;
	unsigned char *sel;
	double right, down;
	if (!ov.obj || !ov.w)
		return 0;
	window_get_wimp_handle(ov.obj, &wimp);
	if (b[5] != wimp)
		return 0;
	if (b[10] == 0x1000 || b[10] == 0x2000) {
		tbx_report("Drag a file here, not a directory.");
		return 1;
	}
	count = docwin_pages(ov.w);
	get_field(O_PAGES, text, sizeof text);
	sel = calloc((size_t)(count > 0 ? count : 1), 1);
	if (!sel)
		return 1;
	n = folio_parse_pages(text, count, sel);
	if (n <= 0) {
		char m[200];
		snprintf(m, sizeof m, "Folio can't tell which pages '%s' means. Type page numbers and ranges "
			"from 1 to %d, such as 3, or 1-3, 20.", text, count);
		tbx_report(m);
		free(sel);
		return 1;
	}
	get_field(O_RIGHT, text, sizeof text);
	right = strtod(text, NULL);
	get_field(O_DOWN, text, sizeof text);
	down = strtod(text, NULL);
	tbx_method_out(ov.obj, 389 /* RadioButton_GetState */, O_BEHIND, &behind);
	app_log("overlay: %s on %d page(s), %.1f mm right, %.1f mm down, %s", (const char *)(b + 11), n, right, down,
		behind & 1 ? "behind" : "in front");
	if (docwin_overlay(ov.w, (const char *)(b + 11), sel, (float)(right * 72 / 25.4), (float)(-down * 72 / 25.4),
			behind & 1) == 0)
		tbx_hide(ov.obj);
	free(sel);
	return 1;
}
