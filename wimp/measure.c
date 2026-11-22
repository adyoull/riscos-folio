/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Measuring (Folio menu > Measure..., or the ruler on the toolbar): while
 * this window is open, Select clicks on the page add points, joined by red
 * lines, and Adjust takes the last one off; the window shows the last
 * line's length, all of them together, and with three points or more the
 * area they enclose.
 *
 * The scale, one of:
 *   Paper        the page's real size (mm, cm, inches or points on paper);
 *   Drawing's    the PDF's own scale for the page (plans and drawings
 *                made with one), in its units, or in the chosen units
 *                when Folio knows its unit;
 *   1: N         a ratio on the paper size, such as 1:1250 for a map
 *                printed at that scale;
 *   Last line =  the reader draws a line over something of known length
 *                (a scale bar), types its length and clicks Set: Folio
 *                works out the scale from that.
 * The units: mm, cm, m, km, inches, feet or points.
 *
 * "Add to page" keeps the measurement as annotations: a line with the
 * figures as its caption, or lines/an area with the figures beside it
 * (core/annot.c). Closing the window ends measuring. One window, for one
 * document window at a time. */
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

/* Component ids. The scale's radio buttons are one group, the units'
 * another; S_PAPER..S_LINE and U_MM..U_PT are in the order of the tables
 * below. */
enum {
	M_L1 = 0, M_LAST, M_L2, M_TOTAL, M_L3, M_AREA, M_L4, M_SCALE,
	S_PAPER, S_OWN, S_RATIO, S_LINE, M_RATIO, M_LINE, M_LINE_UNIT, M_SET,
	M_L5, U_MM, U_CM, U_M, U_KM, U_IN, U_FT, U_PT,
	M_CLEAR, M_ADD, M_CLOSE, M_COUNT
};
#define N_UNITS 7

static struct {
	label_gadget l1;
	display_gadget last;
	label_gadget l2;
	display_gadget total;
	label_gadget l3;
	display_gadget area;
	label_gadget l4;
	display_gadget scale;
	radio_gadget paper, own, ratio, line;
	writable_gadget ratio_w, line_w;
	display_gadget line_unit;
	button_gadget set;
	label_gadget l5;
	radio_gadget units[N_UNITS];
	button_gadget clear, add, close;
} g;

static const char *const unit_name[N_UNITS] = { "mm", "cm", "m", "km", "in", "ft", "pt" };
static const char *const unit_label[N_UNITS] = { "mm", "cm", "m", "km", "inches", "feet", "points" };
static const double unit_mm[N_UNITS] = { 1, 10, 1000, 1e6, 25.4, 304.8, 25.4 / 72 };

static struct {
	ObjectId obj;
	docwin *w;
	folio_doc *doc;       /* the document the scale below was set for */
	int scale;            /* S_PAPER ... S_LINE */
	int units;            /* U_MM ... U_PT */
	double line_mm;       /* Last line =: real mm per point on the page; 0 not set */
	char ratio[12], line[12];
	char last[40], total[40], area[48], scale_text[100], line_unit[12];
	char text[160];       /* the figures, for the page (UTF-8) */
} ms;

#define BOX_W 840
#define BOX_H 640
#define LX 224            /* the labels' right edge */
#define FX 240            /* the fields' left edge */

static void hdr(gadget_hdr *h, int type, int size, int id, int x0, int y0, int x1, int y1, const char *help)
{
	h->flags = 0;
	h->type = (size << 16) | type;
	h->x0 = x0; h->y0 = y0; h->x1 = x1; h->y1 = y1;
	h->id = id;
	h->help = help;
	h->max_help = help ? (int)strlen(help) + 1 : 0;
}

/* A label at the left, with no box, against the field it names (Style
 * Guide). */
static void label(label_gadget *l, int id, const char *text, int y)
{
	hdr(&l->h, 320, sizeof *l, id, 16, y - 48, LX, y, NULL);
	l->h.flags = 1 | (1 << 1);       /* no box; right-aligned */
	l->label = text;
}

static void display(display_gadget *d, int id, char *buf, int size, int x0, int x1, int y, const char *help)
{
	hdr(&d->h, 448, sizeof *d, id, x0, y - 48, x1, y, help);
	d->text = buf;
	d->max_text = size;
}

static void row(label_gadget *l, int lid, const char *text, display_gadget *d, int did, char *buf,
		int size, int y, const char *help)
{
	label(l, lid, text, y);
	display(d, did, buf, size, FX, BOX_W - 16, y, help);
}

static void radio(radio_gadget *r, int id, int group, int x0, int x1, int y, const char *text, int event,
		const char *help)
{
	hdr(&r->h, 384, sizeof *r, id, x0, y - 48, x1, y, help);
	r->h.flags = 1;                  /* say when it's chosen */
	r->group = group;
	r->label = text;
	r->max_label = (int)strlen(text) + 1;
	r->event = event;
}

static void writable(writable_gadget *w, int id, char *buf, int size, int x0, int x1, int y, const char *help)
{
	hdr(&w->h, 512, sizeof *w, id, x0, y - 52, x1, y + 4, help);
	w->text = buf;
	w->max_text = size;
	w->allowable = "0-9.";
	w->max_allowable = 5;
	w->before = -1;
	w->after = -1;
}

/* An action button, the usual height (52 OS units). */
static void button(button_gadget *b, int id, int flags, int x0, int x1, int y, const char *text, int event,
		const char *help)
{
	hdr(&b->h, 128, sizeof *b, id, x0, y - 52, x1, y, help);
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
	int k;
	memset(&g, 0, sizeof g);
	row(&g.l1, M_L1, "Last line", &g.last, M_LAST, ms.last, sizeof ms.last, -24, NULL);
	row(&g.l2, M_L2, "All lines", &g.total, M_TOTAL, ms.total, sizeof ms.total, -84, NULL);
	row(&g.l3, M_L3, "Area", &g.area, M_AREA, ms.area, sizeof ms.area, -144, NULL);
	row(&g.l4, M_L4, "Scale", &g.scale, M_SCALE, ms.scale_text, sizeof ms.scale_text, -204,
		"This is the scale the figures are worked out at.");
	radio(&g.paper, S_PAPER, 1, FX, FX + 200, -264, "Paper", EV_MEAS_MODE,
		"Click to measure the page as printed, at its paper size.");
	radio(&g.own, S_OWN, 1, FX + 220, BOX_W - 16, -264, "Drawing's own", EV_MEAS_MODE,
		"Click to use the scale the PDF gives this page (plans and drawings made with one).");
	radio(&g.ratio, S_RATIO, 1, FX, FX + 76, -324, "1:", EV_MEAS_MODE,
		"Click to measure at a ratio of the paper size, such as 1:1250 for a map printed at that scale.");
	writable(&g.ratio_w, M_RATIO, ms.ratio, sizeof ms.ratio, FX + 84, FX + 284, -324,
		"Type the scale's ratio, such as 1250 for 1:1250, and press Return.");
	radio(&g.line, S_LINE, 1, FX, FX + 220, -384, "Last line is", EV_MEAS_MODE,
		"Click to set the scale from a line of known length: draw the last line over a scale bar, type its length and click Set.");
	writable(&g.line_w, M_LINE, ms.line, sizeof ms.line, FX + 228, FX + 368, -384,
		"Type how long the last line really is, in the units below, and press Return.");
	display(&g.line_unit, M_LINE_UNIT, ms.line_unit, sizeof ms.line_unit, FX + 376, FX + 460, -384, NULL);
	g.line_unit.h.flags = 0;
	button(&g.set, M_SET, 0, BOX_W - 128, BOX_W - 16, -382, "Set", EV_MEAS_SET,
		"Click (or press Return) to use the ratio, or the last line's length, as the scale.");
	label(&g.l5, M_L5, "Units", -444);
	for (k = 0; k < N_UNITS; k++) {
		int col = k < 4 ? k : k - 4, y = k < 4 ? -444 : -504;
		radio(&g.units[k], U_MM + k, 2, FX + col * 144, FX + col * 144 + 140, y, unit_label[k],
			EV_MEAS_UNITS, "Click to measure in these units.");
	}
	button(&g.clear, M_CLEAR, 4 /* local */, 16, 160, -BOX_H + 68, "Clear", EV_MEAS_CLEAR,
		"Click to start again with no points.");
	button(&g.add, M_ADD, 4 /* local */, 172, 380, -BOX_H + 68, "Add to page", EV_MEAS_ADD,
		"Click to keep this measurement on the page, with its figures (PDFs).");
	button(&g.close, M_CLOSE, 2 /* Escape */, BOX_W - 160, BOX_W - 16, -BOX_H + 68, "Close", EV_MEAS_CLOSE,
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

static void fade(int id, int faded)
{
	tbx_method(ms.obj, 65 /* Gadget_SetFlags */, id, faded ? (int)(1u << 31) : 0, 0);
}

static void get_text(int id, char *out, int size)
{
	out[0] = 0;
	_swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, ms.obj, 513 /* WritableField_GetValue */, id, out, size);
}

/* Which unit a drawing's own unit name is, or -1. */
static int known_unit(const char *u)
{
	static const struct { const char *name; int k; } names[] = {
		{ "mm", 0 }, { "millimetres", 0 }, { "millimeters", 0 }, { "cm", 1 }, { "m", 2 },
		{ "metres", 2 }, { "meters", 2 }, { "km", 3 }, { "in", 4 }, { "inch", 4 },
		{ "inches", 4 }, { "\"", 4 }, { "ft", 5 }, { "feet", 5 }, { "foot", 5 }, { "'", 5 }
	};
	size_t i;
	for (i = 0; i < sizeof names / sizeof names[0]; i++)
		if (!strcmp(u, names[i].name))
			return names[i].k;
	return -1;
}

/* A number to show: two decimal places, or more for small ones. */
static void figure(char *out, size_t size, double v, const char *u, const char *sq)
{
	snprintf(out, size, v != 0 && fabs(v) < 0.1 ? "%.4g %s%s" : "%.2f %s%s", v, u, sq);
}

/* Lengths and the area, from the points, at the chosen scale and units. */
void measure_changed(docwin *w)
{
	folio_point pt[MEASURE_MAX];
	int page, n, i, own, own_k = -1, units_free = 1;
	double paper, per_own, len = 0, last = 0, area = 0, mm_per_pt, k = 1;
	char unit[16], ratio[48];
	const char *u;
	if (!ms.obj || w != ms.w)
		return;
	n = docwin_measure_points(w, &page, pt, MEASURE_MAX);
	/* The paper size (with the page's UserUnit), and the drawing's own. */
	own = docwin_measure_scale(w, page, &per_own, unit, sizeof unit, ratio, sizeof ratio, &paper);
	if (!own) {
		if (ms.scale == S_OWN)
			ms.scale = S_PAPER;
	} else
		own_k = known_unit(unit);
	fade(S_OWN, !own);
	tbx_method(ms.obj, 388 /* RadioButton_SetState */, ms.scale, 1, 0);
	mm_per_pt = paper;
	switch (ms.scale) {
	case S_OWN:
		if (own_k >= 0)
			mm_per_pt = per_own * unit_mm[own_k];
		else {
			mm_per_pt = 0;      /* its own units, which Folio doesn't know */
			units_free = 0;
		}
		snprintf(ms.scale_text, sizeof ms.scale_text, "The drawing's own%s%s, in %s",
			ratio[0] ? ": " : "", ratio, unit);
		break;
	case S_RATIO: {
		double r = atof(ms.ratio);
		if (r > 0) {
			mm_per_pt = paper * r;
			snprintf(ms.scale_text, sizeof ms.scale_text, "1:%g of the page's paper size", r);
		} else
			snprintf(ms.scale_text, sizeof ms.scale_text, "Type the ratio and press Return");
		break;
	}
	case S_LINE:
		if (ms.line_mm > 0) {
			mm_per_pt = ms.line_mm;
			snprintf(ms.scale_text, sizeof ms.scale_text, "From the line: about 1:%.0f of the paper size",
				ms.line_mm / paper);
		} else
			snprintf(ms.scale_text, sizeof ms.scale_text,
				"Draw a line over a known length, type it and click Set");
		break;
	default:
		snprintf(ms.scale_text, sizeof ms.scale_text, "The page's paper size");
		break;
	}
	for (i = U_MM; i <= U_PT; i++)
		fade(i, !units_free);
	if (units_free) {
		k = mm_per_pt / unit_mm[ms.units - U_MM];
		u = unit_name[ms.units - U_MM];
	} else {
		k = per_own;
		u = unit;
	}
	snprintf(ms.line_unit, sizeof ms.line_unit, "%s", unit_name[ms.units - U_MM]);
	for (i = 1; i < n; i++) {
		double dx = pt[i].x - pt[i - 1].x, dy = pt[i].y - pt[i - 1].y;
		last = sqrt(dx * dx + dy * dy) * k;
		len += last;
	}
	if (n >= 3) {
		for (i = 0; i < n; i++) {
			const folio_point *a = &pt[i], *b = &pt[(i + 1) % n];
			area += (double)a->x * b->y - (double)b->x * a->y;
		}
		area = fabs(area) / 2 * k * k;
	}
	ms.last[0] = ms.total[0] = ms.area[0] = 0;
	if (n == 0)
		snprintf(ms.last, sizeof ms.last, "Click on the page");
	else if (n >= 2)
		figure(ms.last, sizeof ms.last, last, u, "");
	if (n == 2)
		snprintf(ms.total, sizeof ms.total, "(one line)");
	else if (n >= 3)
		figure(ms.total, sizeof ms.total, len, u, "");
	if (n >= 3)
		figure(ms.area, sizeof ms.area, area, u, "\xc2\xb2");
	if (n == 2)
		snprintf(ms.text, sizeof ms.text, "%s", ms.last);
	else if (n >= 3)
		snprintf(ms.text, sizeof ms.text, "Lines %s\nArea %s", ms.total, ms.area);
	else
		ms.text[0] = 0;
	set_field(M_LAST, ms.last);
	set_field(M_TOTAL, ms.total);
	set_field(M_AREA, ms.area);
	set_field(M_SCALE, ms.scale_text);
	set_field(M_LINE_UNIT, ms.line_unit);
	fade(M_ADD, !(n >= 2 && docwin_can_annotate(w)));
}

/* A scale other than the paper: mm, cm and points become metres. */
static void metres_for(int scale)
{
	if (scale != S_PAPER && (ms.units == U_MM || ms.units == U_CM || ms.units == U_PT)) {
		ms.units = U_M;
		tbx_method(ms.obj, 388 /* RadioButton_SetState */, ms.units, 1, 0);
	}
}

/* Set (or Return): take the ratio, or work the scale out from the last
 * line and its typed length (line: that, if a length is typed). */
static void set_scale(int line)
{
	folio_point pt[MEASURE_MAX];
	int page, n;
	get_text(M_RATIO, ms.ratio, sizeof ms.ratio);
	get_text(M_LINE, ms.line, sizeof ms.line);
	if (!ms.w)
		return;
	n = docwin_measure_points(ms.w, &page, pt, MEASURE_MAX);
	if (line && ms.line[0] && atof(ms.line) > 0 && n >= 2) {
		double dx = pt[n - 1].x - pt[n - 2].x, dy = pt[n - 1].y - pt[n - 2].y;
		double d = sqrt(dx * dx + dy * dy);
		if (d > 0) {
			/* (The length is in the units shown beside it, before any
			 * change to metres.) */
			ms.line_mm = atof(ms.line) * unit_mm[ms.units - U_MM] / d;
			metres_for(S_LINE);
			ms.scale = S_LINE;
			app_log("measure: scale from a line: %g mm a point", ms.line_mm);
		}
	} else if (ms.ratio[0] && atof(ms.ratio) > 0) {
		ms.scale = S_RATIO;
		metres_for(S_RATIO);
		app_log("measure: scale 1:%s", ms.ratio);
	}
}

void measure_open(docwin *w)
{
	int dst[9], pos[2];
	if (!ms.obj && !(ms.obj = create_window()))
		return;
	if (!ms.units)
		ms.units = U_MM;
	if (ms.w && ms.w != w)
		docwin_measure_mode(ms.w, 0);
	/* Another document: start again from its own scale, or the paper's. */
	if (docwin_doc(w) != ms.doc) {
		double per;
		char unit[16];
		ms.doc = docwin_doc(w);
		ms.line_mm = 0;
		ms.scale = docwin_measure_scale(w, -1, &per, unit, sizeof unit, NULL, 0, NULL) ? S_OWN : S_PAPER;
		/* A drawing with its own scale: its units to start with. */
		if (ms.scale == S_OWN && known_unit(unit) >= 0)
			ms.units = U_MM + known_unit(unit);
	}
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

static int chosen(int from, int to)
{
	int i, on;
	for (i = from; i <= to; i++) {
		on = 0;
		tbx_method_out(ms.obj, 389 /* RadioButton_GetState */, i, &on);
		if (on)
			return i;
	}
	return -1;
}

/* Return in the ratio or the length: as Set. */
int measure_key(const int *block)
{
	int icons[2] = { -1, -1 };
	if (!ms.obj || app.idb.self_id != ms.obj || block[6] != 13)
		return 0;
	/* Which field: the one with the caret (Gadget_GetIconList). */
	_swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, ms.obj, 68, M_LINE, icons, (int)sizeof icons);
	set_scale(block[1] == icons[0]);
	if (ms.w)
		measure_changed(ms.w);
	return 1;
}

void measure_event(int code)
{
	int i;
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
		if ((i = chosen(U_MM, U_PT)) >= 0)
			ms.units = i;
		if (ms.w)
			measure_changed(ms.w);
		break;
	case EV_MEAS_MODE:
		if ((i = chosen(S_PAPER, S_LINE)) >= 0 && i != ms.scale) {
			ms.scale = i;
			/* A map or a plan is measured in metres more than in mm. */
			if (i == S_OWN || (i == S_RATIO && ms.ratio[0]))
				metres_for(i);
			if (i == S_RATIO || i == S_LINE)
				set_scale(i == S_LINE);
			ms.scale = i;
		}
		if (ms.w)
			measure_changed(ms.w);
		break;
	case EV_MEAS_SET:
		set_scale(1);
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
