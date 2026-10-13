/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Go to page (F5, or Navigate > Go to page...): a small dialogue with the
 * page number (digits only), "of N", and Cancel and Go (the default, so
 * Return goes). Shown as a transient box near the top of the document
 * window: it closes when it's done or when the pointer clicks elsewhere. */
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

enum { G_LABEL = 0, G_PAGE = 1, G_OF = 2, G_CANCEL = 3, G_GO = 4 };

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

static struct {
	label_gadget label;
	writable_gadget page;
	display_gadget of;
	button_gadget cancel, go;
} g;

static struct {
	ObjectId obj;
	docwin *w;
} gp;

static char page_buf[12], of_buf[32];

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

static void button(button_gadget *b, int id, int flags, int x0, const char *text,
		int event, const char *help)
{
	hdr(&b->h, 128, sizeof *b, id, x0, -168, x0 + 160, -100, help);
	b->h.flags = flags;
	b->text = text;
	b->max_text = (int)strlen(text) + 1;
	b->event = event;
}

#define BOX_W 400
#define BOX_H 184

static ObjectId create_window(void)
{
	static tbx_window t;
	static char title[32] = "Go to page";
	ObjectId id = 0;
	_kernel_oserror *e;

	memset(&g, 0, sizeof g);
	hdr(&g.label.h, 320, sizeof g.label, G_LABEL, 16, -84, 104, -36, NULL);
	g.label.label = "Page";
	hdr(&g.page.h, 512, sizeof g.page, G_PAGE, 104, -88, 232, -32,
		"Type the page number.|MPress Return to go there.");
	g.page.text = page_buf;
	g.page.max_text = sizeof page_buf;
	g.page.allowable = "0-9";
	g.page.max_allowable = 4;
	g.page.before = -1;
	g.page.after = -1;
	hdr(&g.of.h, 448, sizeof g.of, G_OF, 240, -84, 384, -36, NULL);
	g.of.text = of_buf;
	g.of.max_text = sizeof of_buf;
	button(&g.cancel, G_CANCEL, 2 /* Cancel: Escape */, 16, "Cancel", EV_GOTO_CANCEL,
		"Click to close this box.");
	button(&g.go, G_GO, 1 /* default */, 224, "Go", EV_GOTO_OK,
		"Click (or press Return) to go to the page.");

	memset(&t, 0, sizeof t);
	t.flags = (1 << 1) | (1 << 2);   /* the Toolbox opens and closes it */
	t.help_message = "Use this box to go straight to a page.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.num_gadgets = 5;
	t.gadgets = &g;
	t.default_focus = G_PAGE;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = BOX_W;
	t.window.vis_y1 = BOX_H;
	t.window.behind = -1;
	t.window.window_flags = 0x80000002u | (1u << 4) | (1u << 26);
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

void gotopage_open(docwin *w)
{
	int dst[9], pos[2];
	char num[12], of[32];
	if (!gp.obj && !(gp.obj = create_window()))
		return;
	gp.w = w;
	snprintf(num, sizeof num, "%d", docwin_current_page(w) + 1);
	snprintf(of, sizeof of, "of %d%s", docwin_pages(w), docwin_is_counting(w) ? "+" : "");
	tbx_method(gp.obj, 512 /* WritableField_SetValue */, G_PAGE, (int)num, 0);
	tbx_method(gp.obj, 448 /* DisplayField_SetValue */, G_OF, (int)of, 0);
	/* Near the top left of the document window. */
	dst[0] = docwin_wimp(w);
	_swix(Wimp_GetWindowState, _IN(1), dst);
	pos[0] = dst[1] + 64;
	pos[1] = dst[4] - 64;
	/* Transient, like a menu's dialogue: it goes when you click elsewhere. */
	tbx_show(gp.obj, 1, 2, pos, 0, -1);
}

void gotopage_forget(docwin *w)
{
	if (gp.w != w)
		return;
	gp.w = NULL;
	if (gp.obj)
		tbx_hide(gp.obj);
}

void gotopage_event(int code)
{
	char text[12];
	int page;
	if (code != EV_GOTO_OK || !gp.w)
		return;
	text[0] = 0;
	_swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, gp.obj, 513, G_PAGE, text, (int)sizeof text);
	text[sizeof text - 1] = 0;
	page = atoi(text);
	if (page < 1)
		page = 1;
	app_log("go to page %d", page);
	docwin_jump(gp.w, page - 1);
	tbx_hide(gp.obj);
}
