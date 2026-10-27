/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Organising a PDF's pages (document menu > Pages): three small boxes.
 *  - Delete pages: which pages (1-3, 7, or one page); Delete.
 *  - Move pages: which pages, and the page to put them before (empty: the
 *    end); Move.
 *  - Insert pages: the page to put them before (empty: the end), then a PDF,
 *    picture or other document dragged into the box. This one isn't
 *    transient: starting a drag in a Filer window would close it.
 * Each change is one step in the document's undo history; docwin_organise
 * does it and brings the window up to date. */
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
typedef struct {
	gadget_hdr h;
	const char *text;
	int max_text;
	const char *click_show;
	int event;
} button_gadget;

enum { P_LABEL = 0, P_PAGES, P_OF, P_CANCEL, P_OK, P_TO_LABEL, P_TO, P_TO_HINT };

/* In this order; a box uses the first num_gadgets of them. */
typedef struct {
	label_gadget label;
	writable_gadget pages;
	display_gadget of;
	button_gadget cancel;
	union { button_gadget b; label_gadget l; } ok;   /* Insert has a label there */
	label_gadget to_label;
	writable_gadget to;
	label_gadget to_hint;
} box_gadgets;

static struct {
	ObjectId box[3];
	box_gadgets g[3];
	char pages_buf[3][64], of_buf[3][40], to_buf[3][12];
	docwin *w;
	int kind;
} po;

#define BOX_W 640

static void hdr(gadget_hdr *h, int type, int size, int id, int x0, int y0, int x1, int y1,
		const char *help)
{
	h->flags = 0;
	h->type = (size << 16) | type;
	h->x0 = x0; h->y0 = y0; h->x1 = x1; h->y1 = y1;
	h->id = id;
	h->help = help;
	h->max_help = help ? (int)strlen(help) + 1 : 0;
}

static ObjectId create_box(int kind)
{
	static tbx_window t[3];
	static char title[3][32] = { "Delete pages", "Move pages", "Insert pages" };
	box_gadgets *g = &po.g[kind];
	tbx_window *tw = &t[kind];
	int box_h = kind == PO_DELETE ? 184 : 248, by = -box_h + 16, n;
	ObjectId id = 0;
	_kernel_oserror *e;

	memset(g, 0, sizeof *g);
	if (kind == PO_INSERT) {
		/* Row 1: the page to put them before. */
		hdr(&g->label.h, 320, sizeof g->label, P_LABEL, 16, -84, 216, -36, NULL);
		g->label.label = "Before page";
		hdr(&g->pages.h, 512, sizeof g->pages, P_PAGES, 216, -88, 344, -32,
			"Type the page the new pages go in front of.|MLeave it empty to add them at the end.");
		g->pages.allowable = "0-9";
		g->pages.max_allowable = 4;
	} else {
		hdr(&g->label.h, 320, sizeof g->label, P_LABEL, 16, -84, 112, -36, NULL);
		g->label.label = "Pages";
		hdr(&g->pages.h, 512, sizeof g->pages, P_PAGES, 112, -88, 400, -32, kind == PO_DELETE ?
			"Type the pages to delete, such as 3, or 1-3, 20, 45-50." :
			"Type the pages to move, such as 3, or 1-3, 20, 45-50. They keep their order.");
		g->pages.allowable = "0-9,\\- ";
		g->pages.max_allowable = 8;
	}
	g->pages.text = po.pages_buf[kind];
	g->pages.max_text = sizeof po.pages_buf[kind];
	g->pages.before = -1;
	g->pages.after = kind == PO_MOVE ? P_TO : -1;
	hdr(&g->of.h, 448, sizeof g->of, P_OF, kind == PO_INSERT ? 352 : 408, -84, BOX_W - 16, -36, NULL);
	g->of.text = po.of_buf[kind];
	g->of.max_text = sizeof po.of_buf[kind];
	hdr(&g->cancel.h, 128, sizeof g->cancel, P_CANCEL, BOX_W - 360, by, BOX_W - 200, by + 68,
		"Click to close this box.");
	g->cancel.h.flags = 2;          /* Escape */
	g->cancel.text = "Cancel";
	g->cancel.max_text = 7;
	g->cancel.event = EV_PO_CANCEL;
	hdr(&g->ok.b.h, 128, sizeof g->ok.b, P_OK, BOX_W - 184, by, BOX_W - 24, by + 68, kind == PO_DELETE ?
		"Click (or press Return) to delete the pages. Undo (^Z) puts them back." :
		"Click (or press Return) to move the pages. Undo (^Z) puts them back.");
	g->ok.b.h.flags = 1;            /* default: Return */
	g->ok.b.text = kind == PO_DELETE ? "Delete" : "Move";
	g->ok.b.max_text = 7;
	g->ok.b.event = kind == PO_DELETE ? EV_PO_DELETE : EV_PO_MOVE;
	n = 5;
	if (kind == PO_MOVE) {
		hdr(&g->to_label.h, 320, sizeof g->to_label, P_TO_LABEL, 16, -148, 216, -100, NULL);
		g->to_label.label = "Before page";
		hdr(&g->to.h, 512, sizeof g->to, P_TO, 216, -152, 344, -96,
			"Type the page they go in front of.|MLeave it empty to move them to the end.");
		g->to.text = po.to_buf[kind];
		g->to.max_text = sizeof po.to_buf[kind];
		g->to.allowable = "0-9";
		g->to.max_allowable = 4;
		g->to.before = P_PAGES;
		g->to.after = -1;
		hdr(&g->to_hint.h, 320, sizeof g->to_hint, P_TO_HINT, 352, -148, BOX_W - 16, -100, NULL);
		g->to_hint.label = "(empty: the end)";
		n = 8;
	} else if (kind == PO_INSERT) {
		/* No OK button: dragging a file in does it. A label takes its
		 * place; it's the last gadget, so its smaller size is fine (the
		 * Toolbox steps through gadgets by the size in each header). */
		memset(&g->ok, 0, sizeof g->ok);
		hdr(&g->ok.l.h, 320, sizeof g->ok.l, P_OK, 16, -148, BOX_W - 16, -100,
			"Drag a PDF, a picture, or a document Folio reads into this box.");
		g->ok.l.label = "Now drag a PDF, picture or document here.";
	}

	memset(tw, 0, sizeof *tw);
	tw->flags = (1 << 1) | (1 << 2);
	tw->help_message = kind == PO_DELETE ? "Use this box to delete pages from the PDF." :
		kind == PO_MOVE ? "Use this box to move pages to another place in the PDF." :
		"Use this box to add the pages of another file to the PDF: drag the file here.";
	tw->max_help = (int)strlen(tw->help_message) + 1;
	tw->num_gadgets = n;
	tw->gadgets = g;
	tw->default_focus = P_PAGES;
	tw->show_event = -1;
	tw->hide_event = -1;
	tw->window.vis_x1 = BOX_W;
	tw->window.vis_y1 = box_h;
	tw->window.behind = -1;
	/* Moveable, back, close, title; auto-redraw. */
	tw->window.window_flags = 0x80000002u | (1u << 4) | (1u << 24) | (1u << 25) | (1u << 26);
	tw->window.title_fore = 7;
	tw->window.title_back = 2;
	tw->window.work_fore = 7;
	tw->window.work_back = 1;
	tw->window.scroll_outer = 3;
	tw->window.scroll_inner = 1;
	tw->window.title_focus = 12;
	tw->window.work_y0 = -box_h;
	tw->window.work_x1 = BOX_W;
	tw->window.title_flags = 0x00000119;
	tw->window.work_flags = 0;
	tw->window.sprite_area = (void *)1;
	tw->window.title_text = title[kind];
	tw->window.title_validation = "";
	tw->window.title_buflen = sizeof title[kind];
	e = tbx_create(Class_Window, 102, 0, tw, sizeof *tw, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	return id;
}

void pageorg_open(docwin *w, int kind, int page)
{
	int dst[9], pos[2], k;
	char of[40], num[16];
	if (kind < 0 || kind > 2)
		return;
	if (docwin_is_counting(w)) {
		tbx_report("Folio is still counting this document's pages. Try again in a moment.");
		return;
	}
	if (!po.box[kind] && !(po.box[kind] = create_box(kind)))
		return;
	for (k = 0; k < 3; k++)
		if (k != kind && po.box[k])
			tbx_hide(po.box[k]);
	po.w = w;
	po.kind = kind;
	snprintf(of, sizeof of, kind == PO_INSERT ? "of %d" : "of %d", docwin_pages(w));
	tbx_method(po.box[kind], 448 /* DisplayField_SetValue */, P_OF, (int)of, 0);
	snprintf(num, sizeof num, "%d", page + 1);
	tbx_method(po.box[kind], 512 /* WritableField_SetValue */, P_PAGES, (int)num, 0);
	if (kind == PO_MOVE)
		tbx_method(po.box[kind], 512, P_TO, (int)"", 0);
	dst[0] = docwin_wimp(w);
	_swix(Wimp_GetWindowState, _IN(1), dst);
	pos[0] = dst[1] + 64;
	pos[1] = dst[4] - 64;
	/* Delete and Move are transient, like Go to page; Insert stays open for
	 * the drag. */
	tbx_show(po.box[kind], kind == PO_INSERT ? 0 : 1, 2, pos, 0, -1);
}

void pageorg_forget(docwin *w)
{
	int k;
	if (po.w != w)
		return;
	po.w = NULL;
	for (k = 0; k < 3; k++)
		if (po.box[k])
			tbx_hide(po.box[k]);
}

static void get_field(int kind, int id, char *out, int size)
{
	out[0] = 0;
	_swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, po.box[kind], 513, id, out, size);
	out[size - 1] = 0;
}

/* The "before page" field: 1-based, empty or past the end: the end. */
static int before_page(int kind, int id)
{
	char text[12];
	int n = docwin_pages(po.w), p;
	get_field(kind, id, text, sizeof text);
	p = atoi(text);
	return p < 1 || p > n ? n : p - 1;
}

void pageorg_event(int code)
{
	char text[64];
	int kind = code == EV_PO_DELETE ? PO_DELETE : PO_MOVE, count, n;
	unsigned char *sel;
	if (code == EV_PO_CANCEL || !po.w)
		return;
	count = docwin_pages(po.w);
	get_field(kind, P_PAGES, text, sizeof text);
	if (!*text) {
		tbx_report("Type the pages, such as 3, or 1-3, 20.");
		return;
	}
	sel = calloc((size_t)(count > 0 ? count : 1), 1);
	if (!sel)
		return;
	n = folio_parse_pages(text, count, sel);
	if (n <= 0) {
		char m[200];
		snprintf(m, sizeof m, "Folio can't tell which pages '%s' means. Type page numbers and ranges "
			"from 1 to %d, such as 3, or 1-3, 20.", text, count);
		tbx_report(m);
		free(sel);
		return;
	}
	if (kind == PO_DELETE && n == count) {
		tbx_report("A PDF has to keep at least one page.");
		free(sel);
		return;
	}
	if (docwin_organise(po.w, kind, sel, kind == PO_MOVE ? before_page(kind, P_TO) : 0, NULL) == 0)
		tbx_hide(po.box[kind]);
	free(sel);
}

/* A file dropped on the Insert box (Message_DataLoad). 1 if it was ours. */
int pageorg_data_load(int *b)
{
	int wimp = 0;
	if (!po.box[PO_INSERT] || !po.w)
		return 0;
	window_get_wimp_handle(po.box[PO_INSERT], &wimp);
	if (b[5] != wimp)
		return 0;
	if (b[10] == 0x1000 || b[10] == 0x2000) {
		tbx_report("Drag a file here, not a directory.");
		return 1;
	}
	app_log("insert pages: %s before page %d", (const char *)(b + 11), before_page(PO_INSERT, P_PAGES) + 1);
	if (docwin_organise(po.w, PO_INSERT, NULL, before_page(PO_INSERT, P_PAGES), (const char *)(b + 11)) == 0)
		tbx_hide(po.box[PO_INSERT]);
	return 1;
}
