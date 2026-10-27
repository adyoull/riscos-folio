/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Drawing and pictures on the page (PDFs).
 *  - The Draw window (Edit > Draw...): while it's open, a Select drag on
 *    the page draws a stroke in the chosen colour and thickness, and Adjust
 *    takes the last stroke off. The strokes go on the page together as one
 *    drawing (an Ink annotation) on Keep, on closing the window, or when a
 *    stroke is started on another page. One window, for one document
 *    window at a time.
 *  - The Picture box (Edit > Stamp > Picture...): a picture file dragged
 *    into it (JPEG, PNG, GIF, BMP, TIFF and the rest MuPDF reads) is then
 *    placed where the page is next clicked, two inches wide; white can be
 *    made see-through (a signature written on paper and scanned).
 * Standard stamps (Edit > Stamp) are placed by docwin the same way. */
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
typedef struct { gadget_hdr h; char *text; int max_text; } display_gadget;
typedef struct { gadget_hdr h; int group; const char *label; int max_label; int event; } radio_gadget;
typedef struct { gadget_hdr h; const char *label; int max_label; int event; } option_gadget;
typedef struct {
	gadget_hdr h;
	const char *text;
	int max_text;
	const char *click_show;
	int event;
} button_gadget;

static void hdr(gadget_hdr *h, int type, int size, int id, int x0, int y0, int x1, int y1, const char *help)
{
	h->flags = 0;
	h->type = (size << 16) | type;
	h->x0 = x0; h->y0 = y0; h->x1 = x1; h->y1 = y1;
	h->id = id;
	h->help = help;
	h->max_help = help ? (int)strlen(help) + 1 : 0;
}

static void window_template(tbx_window *t, const char *help, int w, int h, char *title, int title_len,
		void *gadgets, int n, int hide_event)
{
	memset(t, 0, sizeof *t);
	t->flags = (1 << 1) | (1 << 2) | (hide_event >= 0 ? (1 << 3) : 0);
	t->help_message = help;
	t->max_help = (int)strlen(help) + 1;
	t->num_gadgets = n;
	t->gadgets = gadgets;
	t->default_focus = -1;
	t->show_event = -1;
	t->hide_event = hide_event;
	t->window.vis_x1 = w;
	t->window.vis_y1 = h;
	t->window.behind = -1;
	t->window.window_flags = 0x80000002u | (1u << 4) | (1u << 24) | (1u << 25) | (1u << 26);
	t->window.title_fore = 7;
	t->window.title_back = 2;
	t->window.work_fore = 7;
	t->window.work_back = 1;
	t->window.scroll_outer = 3;
	t->window.scroll_inner = 1;
	t->window.title_focus = 12;
	t->window.work_y0 = -h;
	t->window.work_x1 = w;
	t->window.title_flags = 0x00000119;
	t->window.sprite_area = (void *)1;
	t->window.title_text = title;
	t->window.title_validation = "";
	t->window.title_buflen = title_len;
}

/* ---- The Draw window ---------------------------------------------------------- */

enum { D_COL_L = 0, D_BLACK, D_BLUE, D_RED, D_PEN_L, D_THIN, D_MEDIUM, D_THICK, D_COUNT_F,
	D_UNDO, D_CLEAR, D_KEEP, D_CLOSE, D_N };

static struct {
	label_gadget col_l;
	radio_gadget black, blue, red;
	label_gadget pen_l;
	radio_gadget thin, medium, thick;
	display_gadget count;
	button_gadget undo, clear, keep, close;
} dg;

static struct {
	ObjectId obj;
	docwin *w;
	int colour, pen;      /* the radio ids chosen */
	char count[48];
} dr = { .colour = D_BLUE, .pen = D_MEDIUM };

#define DW 760
#define DH 300

static void radio(radio_gadget *r, int id, int group, int x0, int y1, const char *label, const char *help)
{
	hdr(&r->h, 384, sizeof *r, id, x0, y1 - 48, x0 + 176, y1, help);
	r->h.flags = 1;                  /* say when it's chosen */
	r->group = group;
	r->label = label;
	r->max_label = (int)strlen(label) + 1;
	r->event = EV_DR_STYLE;
}

static void button(button_gadget *b, int id, int flags, int x0, int x1, int y0, const char *text, int event,
		const char *help)
{
	hdr(&b->h, 128, sizeof *b, id, x0, y0, x1, y0 + 68, help);
	b->h.flags = flags;
	b->text = text;
	b->max_text = (int)strlen(text) + 1;
	b->event = event;
}

static ObjectId create_draw(void)
{
	static tbx_window t;
	static char title[] = "Draw";
	ObjectId id = 0;
	_kernel_oserror *e;
	memset(&dg, 0, sizeof dg);
	hdr(&dg.col_l.h, 320, sizeof dg.col_l, D_COL_L, 16, -72, 184, -24, NULL);
	dg.col_l.label = "Colour";
	radio(&dg.black, D_BLACK, 1, 192, -24, "Black", "Click to draw in black.");
	radio(&dg.blue, D_BLUE, 1, 376, -24, "Blue", "Click to draw in dark blue, like a pen.");
	radio(&dg.red, D_RED, 1, 560, -24, "Red", "Click to draw in red.");
	hdr(&dg.pen_l.h, 320, sizeof dg.pen_l, D_PEN_L, 16, -136, 184, -88, NULL);
	dg.pen_l.label = "Pen";
	radio(&dg.thin, D_THIN, 2, 192, -88, "Thin", "Click for a thin line (1 point), for writing.");
	radio(&dg.medium, D_MEDIUM, 2, 376, -88, "Medium", "Click for a medium line (2 points).");
	radio(&dg.thick, D_THICK, 2, 560, -88, "Thick", "Click for a thick line (4 points).");
	hdr(&dg.count.h, 448, sizeof dg.count, D_COUNT_F, 16, -208, DW - 16, -156, NULL);
	dg.count.text = dr.count;
	dg.count.max_text = sizeof dr.count;
	button(&dg.undo, D_UNDO, 4, 16, 176, -DH + 16, "Undo", EV_DR_UNDO,
		"Click to take the last stroke off (or click Adjust on the page).");
	button(&dg.clear, D_CLEAR, 4, 192, 352, -DH + 16, "Clear", EV_DR_CLEAR,
		"Click to take all the strokes not yet kept off.");
	button(&dg.keep, D_KEEP, 4, 368, 576, -DH + 16, "Keep", EV_DR_KEEP,
		"Click to put the drawing on the page (Ctrl-Z undoes it).");
	button(&dg.close, D_CLOSE, 2, DW - 176, DW - 16, -DH + 16, "Close", EV_DR_CLOSE,
		"Click to stop drawing; what you've drawn is kept on the page.");
	window_template(&t, "Drag with Select on the page to draw; Adjust takes the last stroke off.",
		DW, DH, title, sizeof title, &dg, D_N, EV_DR_HIDDEN);
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	return id;
}

static int colour_rgb(void)
{
	return dr.colour == D_BLACK ? 0x000000 : dr.colour == D_RED ? 0xcc1111 : 0x1a2b8c;
}

static float pen_width(void)
{
	return dr.pen == D_THIN ? 1.0f : dr.pen == D_THICK ? 4.0f : 2.0f;
}

void drawui_changed(docwin *w)
{
	int n;
	if (!dr.obj || w != dr.w)
		return;
	n = docwin_pen_strokes(w);
	snprintf(dr.count, sizeof dr.count, n ? "%d stroke%s, not yet kept" : "Drag on the page to draw",
		n, n == 1 ? "" : "s");
	tbx_method(dr.obj, 448, D_COUNT_F, (int)dr.count, 0);
}

void drawui_open(docwin *w)
{
	int dst[9], pos[2];
	if (!dr.obj && !(dr.obj = create_draw()))
		return;
	if (dr.w && dr.w != w)
		docwin_pen_mode(dr.w, 0, colour_rgb(), pen_width());
	dr.w = w;
	measure_forget(w);
	docwin_pen_mode(w, 1, colour_rgb(), pen_width());
	tbx_method(dr.obj, 388, dr.colour, 1, 0);
	tbx_method(dr.obj, 388, dr.pen, 1, 0);
	drawui_changed(w);
	dst[0] = docwin_wimp(w);
	_swix(Wimp_GetWindowState, _IN(1), dst);
	pos[0] = dst[3] - DW - 64;
	pos[1] = dst[4] - 64;
	tbx_show(dr.obj, 0, 2, pos, 0, -1);
	app_log("draw: on");
}

void drawui_forget(docwin *w)
{
	if (dr.w != w)
		return;
	dr.w = NULL;
	if (dr.obj)
		tbx_hide(dr.obj);
}

void drawui_event(int code)
{
	int i, on;
	switch (code) {
	case EV_DR_UNDO:
		if (dr.w) {
			docwin_pen_undo(dr.w);
			drawui_changed(dr.w);
		}
		break;
	case EV_DR_CLEAR:
		if (dr.w) {
			docwin_pen_clear(dr.w);
			drawui_changed(dr.w);
		}
		break;
	case EV_DR_KEEP:
		if (dr.w)
			docwin_pen_keep(dr.w);
		break;
	case EV_DR_STYLE:
		for (i = D_BLACK; i <= D_THICK; i++) {
			on = 0;
			tbx_method_out(dr.obj, 389, i, &on);
			if (on && i <= D_RED)
				dr.colour = i;
			else if (on && i >= D_THIN)
				dr.pen = i;
		}
		if (dr.w)
			docwin_pen_mode(dr.w, 1, colour_rgb(), pen_width());
		break;
	case EV_DR_CLOSE:
		if (dr.obj)
			tbx_hide(dr.obj);
		/* The hidden event may not come for a Close click. */
		/* fall through */
	case EV_DR_HIDDEN:
		if (dr.w) {
			docwin_pen_mode(dr.w, 0, colour_rgb(), pen_width());
			app_log("draw: off");
		}
		dr.w = NULL;
		break;
	}
}

/* ---- The Picture box ---------------------------------------------------------- */

enum { P_HINT = 0, P_WHITE, P_CANCEL, P_N };

static struct {
	label_gadget hint;
	option_gadget white;
	button_gadget cancel;
} pg;

static struct {
	ObjectId obj;
	int wimp;
	docwin *w;
	int white;
} pb = { .white = 1 };

#define PW 760
#define PH 220

static ObjectId create_picbox(void)
{
	static tbx_window t;
	static char title[] = "Picture";
	ObjectId id = 0;
	_kernel_oserror *e;
	memset(&pg, 0, sizeof pg);
	hdr(&pg.hint.h, 320, sizeof pg.hint, P_HINT, 16, -72, PW - 16, -24,
		"Drag a picture file (JPEG, PNG and so on) into this box, then click on the page where it goes.");
	pg.hint.label = "Drag a picture here, then click on the page.";
	hdr(&pg.white.h, 192, sizeof pg.white, P_WHITE, 16, -136, PW - 16, -88,
		"Click to make the picture's white see-through, as for a signature written on paper and scanned.");
	pg.white.label = "Make white see-through (a scanned signature)";
	pg.white.max_label = (int)strlen(pg.white.label) + 1;
	button(&pg.cancel, P_CANCEL, 2, PW - 176, PW - 16, -PH + 16, "Cancel", EV_PIC_CANCEL,
		"Click to close this box.");
	window_template(&t, "Drag a picture into this box, then click on the page where it goes.",
		PW, PH, title, sizeof title, &pg, P_N, -1);
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	window_get_wimp_handle(id, &pb.wimp);
	return id;
}

void picbox_open(docwin *w)
{
	int dst[9], pos[2];
	if (!pb.obj && !(pb.obj = create_picbox()))
		return;
	pb.w = w;
	tbx_method(pb.obj, 196 /* OptionButton_SetState */, P_WHITE, pb.white, 0);
	dst[0] = docwin_wimp(w);
	_swix(Wimp_GetWindowState, _IN(1), dst);
	pos[0] = dst[1] + 64;
	pos[1] = dst[4] - 64;
	tbx_show(pb.obj, 0, 2, pos, 0, -1);
}

void picbox_forget(docwin *w)
{
	if (pb.w != w)
		return;
	pb.w = NULL;
	if (pb.obj)
		tbx_hide(pb.obj);
}

void picbox_event(int code)
{
	if (code == EV_PIC_CANCEL && pb.obj)
		tbx_hide(pb.obj);
}

/* A file dropped in the Picture box (Message_DataLoad). 1 if it was ours. */
int picbox_data_load(int *b)
{
	int on = 0;
	if (!pb.obj || !pb.w || b[5] != pb.wimp)
		return 0;
	if (b[10] == 0x1000 || b[10] == 0x2000) {
		tbx_report("Drag a picture file here, not a directory.");
		return 1;
	}
	tbx_method_out(pb.obj, 197 /* OptionButton_GetState */, P_WHITE, &on);
	pb.white = on != 0;
	app_log("picture: %s", (const char *)(b + 11));
	docwin_place_picture(pb.w, (const char *)(b + 11), pb.white);
	tbx_hide(pb.obj);
	return 1;
}
