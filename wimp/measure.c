/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Measuring (Edit > Measure...): while this window is open, Select clicks
 * on the page add points, joined by red lines, and Adjust takes the last
 * one off; the window shows the last line's length, all of them together,
 * and with three points or more the area they enclose. Lengths are in mm,
 * cm, inches or points at the page's paper size, or, when the PDF gives
 * the page a scale of its own (plans, drawings), in its units. "Add to
 * page" keeps the measurement as an annotation (a line, lines, or a closed
 * shape for an area), with the figures as its note. Closing the window
 * ends measuring. One window, for one document window at a time. */
#include "app.h"
#include "worker.h"
#include <math.h>
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
typedef struct {
	gadget_hdr h;
	const char *text;
	int max_text;
	const char *click_show;
	int event;
} button_gadget;

enum { M_L1 = 0, M_LAST, M_L2, M_TOTAL, M_L3, M_AREA, M_SCALE, M_MM, M_CM, M_IN, M_PT,
	M_CLEAR, M_ADD, M_CLOSE, M_COUNT };

static struct {
	label_gadget l1;
	display_gadget last;
	label_gadget l2;
	display_gadget total;
	label_gadget l3;
	display_gadget area;
	display_gadget scale;
	radio_gadget mm, cm, in, pt;
	button_gadget clear, add, close;
} g;

static struct {
	ObjectId obj;
	docwin *w;
	int units;            /* M_MM ... M_PT */
	char last[40], total[40], area[48], scale[80];
	char text[160];       /* the figures, for the annotation's note (UTF-8) */
} ms;

#define BOX_W 760
#define BOX_H 420

static void hdr(gadget_hdr *h, int type, int size, int id, int x0, int y0, int x1, int y1, const char *help)
{
	h->flags = 0;
	h->type = (size << 16) | type;
	h->x0 = x0; h->y0 = y0; h->x1 = x1; h->y1 = y1;
	h->id = id;
	h->help = help;
	h->max_help = help ? (int)strlen(help) + 1 : 0;
}

static void row(label_gadget *l, int lid, const char *label, display_gadget *d, int did, char *buf,
		int size, int y)
{
	hdr(&l->h, 320, sizeof *l, lid, 16, y - 48, 232, y, NULL);
	l->label = label;
	hdr(&d->h, 448, sizeof *d, did, 240, y - 52, BOX_W - 16, y - 4, NULL);
	d->text = buf;
	d->max_text = size;
}

static void radio(radio_gadget *r, int id, int x0, const char *label)
{
	hdr(&r->h, 384, sizeof *r, id, x0, -296, x0 + 168, -248, "Click to measure in these units.");
	r->h.flags = 1;                  /* say when it's chosen */
	r->group = 1;
	r->label = label;
	r->max_label = (int)strlen(label) + 1;
	r->event = EV_MEAS_UNITS;
}

static void button(button_gadget *b, int id, int flags, int x0, int x1, const char *text, int event,
		const char *help)
{
	hdr(&b->h, 128, sizeof *b, id, x0, -BOX_H + 16, x1, -BOX_H + 84, help);
	b->h.flags = flags;
	b->text = text;
	b->max_text = (int)strlen(text) + 1;
	b->event = event;
}

static ObjectId create_window(void)
{
	static tbx_window t;
	static char title[] = "Measure";
	ObjectId id = 0;
	_kernel_oserror *e;
	memset(&g, 0, sizeof g);
	row(&g.l1, M_L1, "Last line", &g.last, M_LAST, ms.last, sizeof ms.last, -24);
	row(&g.l2, M_L2, "All lines", &g.total, M_TOTAL, ms.total, sizeof ms.total, -84);
	row(&g.l3, M_L3, "Area", &g.area, M_AREA, ms.area, sizeof ms.area, -144);
	hdr(&g.scale.h, 448, sizeof g.scale, M_SCALE, 16, -232, BOX_W - 16, -184, NULL);
	g.scale.text = ms.scale;
	g.scale.max_text = sizeof ms.scale;
	radio(&g.mm, M_MM, 16, "mm");
	radio(&g.cm, M_CM, 200, "cm");
	radio(&g.in, M_IN, 384, "inches");
	radio(&g.pt, M_PT, 568, "points");
	button(&g.clear, M_CLEAR, 4 /* local */, 16, 196, "Clear", EV_MEAS_CLEAR,
		"Click to start again with no points.");
	button(&g.add, M_ADD, 4 /* local */, 212, 452, "Add to page", EV_MEAS_ADD,
		"Click to keep this measurement on the page, as an annotation with the figures as its note (PDFs).");
	button(&g.close, M_CLOSE, 2 /* Escape */, BOX_W - 196, BOX_W - 16, "Close", EV_MEAS_CLOSE,
		"Click to stop measuring.");

	memset(&t, 0, sizeof t);
	t.flags = (1 << 1) | (1 << 2) | (1 << 3);   /* opened and closed by the Toolbox; say when hidden */
	t.help_message = "Click Select on the page to add points; Adjust takes the last one off.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.num_gadgets = M_COUNT;
	t.gadgets = &g;
	t.default_focus = -1;
	t.show_event = -1;
	t.hide_event = EV_MEAS_HIDDEN;
	t.window.vis_x1 = BOX_W;
	t.window.vis_y1 = BOX_H;
	t.window.behind = -1;
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

static void set_field(int id, const char *utf8)
{
	char *d = app_desktop_text(utf8);
	tbx_method(ms.obj, 448 /* DisplayField_SetValue */, id, (int)(d ? d : utf8), 0);
	free(d);
}

/* Lengths and the area, from the points, in the chosen units. */
void measure_changed(docwin *w)
{
	folio_point pt[MEASURE_MAX];
	int page, n, i, own;
	double per, len = 0, last = 0, area = 0, k = 1;
	char unit[16];
	const char *u;
	if (!ms.obj || w != ms.w)
		return;
	n = docwin_measure_points(w, &page, pt, MEASURE_MAX);
	own = docwin_measure_scale(w, page, &per, unit, sizeof unit);
	for (i = M_MM; i <= M_PT; i++)
		tbx_method(ms.obj, 65 /* Gadget_SetFlags */, i, own ? (int)(1u << 31) : 0, 0);
	if (own) {
		u = unit;
		snprintf(ms.scale, sizeof ms.scale, "The drawing's own scale, in %s", unit);
	} else {
		static const char *const names[] = { "mm", "cm", "in", "pt" };
		static const double from_mm[] = { 1, 0.1, 1 / 25.4, 72 / 25.4 };
		k = from_mm[ms.units - M_MM];
		u = names[ms.units - M_MM];
		snprintf(ms.scale, sizeof ms.scale, "The page's paper size%s", n ? "" : ": click on the page");
	}
	for (i = 1; i < n; i++) {
		double dx = pt[i].x - pt[i - 1].x, dy = pt[i].y - pt[i - 1].y;
		last = sqrt(dx * dx + dy * dy) * per * k;
		len += last;
	}
	if (n >= 3) {
		for (i = 0; i < n; i++) {
			const folio_point *a = &pt[i], *b = &pt[(i + 1) % n];
			area += (double)a->x * b->y - (double)b->x * a->y;
		}
		area = fabs(area) / 2 * per * per * k * k;
	}
	snprintf(ms.last, sizeof ms.last, n >= 2 ? "%.2f %s" : "", last, u);
	snprintf(ms.total, sizeof ms.total, n >= 3 ? "%.2f %s" : n == 2 ? "(one line)" : "", len, u);
	snprintf(ms.area, sizeof ms.area, n >= 3 ? "%.2f %s\xc2\xb2" : "", area, u);
	if (n == 2)
		snprintf(ms.text, sizeof ms.text, "Length %.2f %s", last, u);
	else if (n >= 3)
		snprintf(ms.text, sizeof ms.text, "Lines %.2f %s; closed, area %.2f %s\xc2\xb2", len, u, area, u);
	else
		ms.text[0] = 0;
	set_field(M_LAST, ms.last);
	set_field(M_TOTAL, ms.total);
	set_field(M_AREA, ms.area);
	set_field(M_SCALE, ms.scale);
	tbx_method(ms.obj, 65, M_ADD, n >= 2 && docwin_can_annotate(w) ? 0 : (int)(1u << 31), 0);
}

void measure_open(docwin *w)
{
	int dst[9], pos[2];
	if (!ms.obj && !(ms.obj = create_window()))
		return;
	if (!ms.units)
		ms.units = M_MM;
	if (ms.w && ms.w != w)
		docwin_measure_mode(ms.w, 0);
	ms.w = w;
	docwin_measure_mode(w, 1);
	tbx_method(ms.obj, 388 /* RadioButton_SetState */, ms.units, 1, 0);
	measure_changed(w);
	dst[0] = docwin_wimp(w);
	_swix(Wimp_GetWindowState, _IN(1), dst);
	pos[0] = dst[3] - BOX_W - 64;
	pos[1] = dst[4] - 64;
	tbx_show(ms.obj, 0, 2, pos, 0, -1);
	app_log("measure: on");
}

void measure_forget(docwin *w)
{
	if (ms.w != w)
		return;
	ms.w = NULL;
	if (ms.obj)
		tbx_hide(ms.obj);
}

void measure_event(int code)
{
	int i, on;
	switch (code) {
	case EV_MEAS_CLEAR:
		if (ms.w)
			docwin_measure_clear(ms.w);
		break;
	case EV_MEAS_ADD:
		if (ms.w && ms.text[0])
			docwin_measure_keep(ms.w, ms.text);
		break;
	case EV_MEAS_UNITS:
		for (i = M_MM; i <= M_PT; i++) {
			on = 0;
			tbx_method_out(ms.obj, 389 /* RadioButton_GetState */, i, &on);
			if (on)
				ms.units = i;
		}
		if (ms.w)
			measure_changed(ms.w);
		break;
	case EV_MEAS_CLOSE:
		if (ms.obj)
			tbx_hide(ms.obj);
		/* The hidden event may not come for a Close click: on as for it. */
		/* fall through */
	case EV_MEAS_HIDDEN:
		if (ms.w) {
			docwin_measure_mode(ms.w, 0);
			app_log("measure: off");
		}
		ms.w = NULL;
		break;
	}
}
