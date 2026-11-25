/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Export page as Draw... (document menu): the page being read as a Draw
 * file (core/drawfile.c), through a Toolbox SaveAs box of its own, type
 * &AFF. Drag the icon to a directory, or to Draw or another program (the
 * Toolbox does the data transfer protocol; Folio writes the file itself).
 * Below the usual parts, Folio adds a choice of resolution for pictures:
 * as in the PDF, 150 dpi, or 90 dpi (for the screen: Draw shows a picture
 * by skipping pixels, so a fine scan's thin lines break up; shrunk with
 * averaging, they stay whole). Kept in the Choices. */
#include "app.h"
#include "worker.h"
#include <stdio.h>
#include <string.h>
#include <swis.h>

#define Class_SaveAs 0x82bc0

static struct {
	ObjectId saveas;
	ObjectId win;                /* its window, with our gadgets added */
	int added;                   /* height added at the bottom (OS units) */
	ComponentId radio[3];
	docwin *w;
	int page;
} ex;

static const int dpis[3] = { 0, 150, 90 };

typedef struct {
	int flags, type;
	int x0, y0, x1, y1;
	ComponentId id;
	const char *help;
	int max_help;
} gadget_hdr;
typedef struct { gadget_hdr h; const char *label; } label_gadget;
typedef struct { gadget_hdr h; int group; const char *label; int max_label; int event; } radio_gadget;

/* The Pictures choice, added to the SaveAs box's window (Window_AddGadget)
 * under what's there, which is made taller to fit. */
static void add_choice(void)
{
	static const char *const names[3] = { "As in the PDF", "150 dpi", "90 dpi (screen)" };
	static const char help[] = "Choose the resolution for the page's pictures. Draw shows a fine "
		"picture (a 300 dpi scan) by skipping pixels, so its thin lines break up; at 90 dpi they "
		"stay whole, but zoomed in or printed it's coarser.";
	int box[4], k, y;
	label_gadget l;
	if (_swix(Toolbox_ObjectMiscOp, _INR(0, 2) | _OUT(0), 0, ex.saveas, 0 /* SaveAs_GetWindowID */, &ex.win) ||
			_swix(Toolbox_ObjectMiscOp, _INR(0, 3), 0, ex.win, 16 /* Window_GetExtent */, box))
		return;
	/* Under what shows: the window's height as made, not its extent. */
	{
		int st[9], h = 0;
		_swix(Toolbox_ObjectMiscOp, _INR(0, 2) | _OUT(0), 0, ex.win, 0 /* Window_GetWimpHandle */, &h);
		st[0] = h;
		if (h && !_swix(Wimp_GetWindowState, _IN(1), st) && st[4] - st[2] > 0)
			box[1] = box[3] - (st[4] - st[2]);
	}
	y = box[1] - 4;
	memset(&l, 0, sizeof l);
	l.h.type = (sizeof l << 16) | 320;
	l.h.flags = 1;                               /* no box */
	l.h.x0 = 16; l.h.x1 = box[2] - 16;
	l.h.y0 = y - 44; l.h.y1 = y;
	l.h.id = 0x100;
	l.label = "Pictures:";
	tbx_method(ex.win, 1 /* Window_AddGadget */, (int)&l, 0, 0);
	y -= 44;
	for (k = 0; k < 3; k++) {
		radio_gadget r;
		memset(&r, 0, sizeof r);
		r.h.type = (sizeof r << 16) | 384;
		r.h.x0 = 40; r.h.x1 = box[2] - 16;
		r.h.y0 = y - 44; r.h.y1 = y;
		r.h.id = 0x101 + k;
		r.h.help = help;
		r.h.max_help = sizeof help;
		r.group = 1;
		r.label = names[k];
		r.max_label = (int)strlen(names[k]) + 1;
		r.event = 0;
		ex.radio[k] = -1;
		_swix(Toolbox_ObjectMiscOp, _INR(0, 3) | _OUT(0), 0, ex.win, 1 /* Window_AddGadget */, &r, &ex.radio[k]);
		y -= 44;
	}
	ex.added = box[1] - (y - 12);
	box[1] = y - 12;
	tbx_method(ex.win, 15 /* Window_SetExtent */, (int)box, 0, 0);
}

static int chosen_dpi(void)
{
	int k, on;
	for (k = 0; k < 3; k++) {
		on = 0;
		if (ex.radio[k] >= 0)
			tbx_method_out(ex.win, 389 /* RadioButton_GetState */, ex.radio[k], &on);
		if (on)
			return dpis[k];
	}
	return app.ch.draw_dpi;
}

void export_open(docwin *w)
{
	static struct {
		int flags;
		const char *filename;
		int filetype;
		const char *title;
		int max_title;
		const char *window;
	} t;
	static char name[32];
	int ptr[5], pos[2];
	if (!ex.saveas) {
		_kernel_oserror *e;
		memset(&t, 0, sizeof t);
		t.flags = 4;                 /* no Selection option; Folio writes the file itself */
		t.filename = "Page";
		t.filetype = 0xaff;
		t.title = "Export as Draw";
		t.max_title = 32;
		e = tbx_create(Class_SaveAs, 100, 0, &t, sizeof t, &ex.saveas);
		if (e) {
			tbx_report_error(e);
			ex.saveas = 0;
			return;
		}
		add_choice();
	}
	ex.w = w;
	ex.page = docwin_current_page(w);
	snprintf(name, sizeof name, "Page%d", ex.page + 1);
	tbx_method(ex.saveas, 3 /* SaveAs_SetFileName */, (int)name, 0, 0);
	_swix(Wimp_GetPointerInfo, _IN(1), ptr);
	pos[0] = ptr[0] - 64;
	pos[1] = ptr[1] + 64;
	{
		int k;
		for (k = 0; k < 3; k++)
			if (ex.radio[k] >= 0 && dpis[k] == app.ch.draw_dpi)
				tbx_method(ex.win, 388 /* RadioButton_SetState */, ex.radio[k], 1, 0);
	}
	tbx_show(ex.saveas, 0, 2, pos, 0, -1);
	/* Open as tall as its extent now is (the Toolbox keeps the size it
	 * was made with). */
	if (ex.added) {
		int st[9], h = 0;
		_swix(Toolbox_ObjectMiscOp, _INR(0, 2) | _OUT(0), 0, ex.win, 0 /* Window_GetWimpHandle */, &h);
		st[0] = h;
		if (h && !_swix(Wimp_GetWindowState, _IN(1), st)) {
			int box[4];
			if (!_swix(Toolbox_ObjectMiscOp, _INR(0, 3), 0, ex.win, 16, box) && st[4] - st[2] < box[3] - box[1]) {
				st[2] = st[4] - (box[3] - box[1]);
				_swix(Wimp_OpenWindow, _IN(1), st);
			}
		}
	}
}

static void save_to_file(int *b)
{
	const char *path = (const char *)(b + 4);
	folio_loc loc;
	int ok = 0;
	if (ex.w) {
		folio_doc *doc = docwin_doc(ex.w);
		_swix(Hourglass_On, 0);
		core_now();
		int dpi = chosen_dpi();
		if (dpi != app.ch.draw_dpi) {
			app.ch.draw_dpi = dpi;
			choices_save();
		}
		ok = folio_loc_from_number(doc, ex.page, &loc) == 0 && folio_export_draw_dpi(doc, loc, path, dpi) == 0;
		core_unlock();
		_swix(Hourglass_Off, 0);
		if (ok)
			_swix(OS_File, _INR(0, 2), 18, path, 0xaff);
		app_log("export: page %d (pictures %d dpi) to %s%s%s", ex.page + 1, dpi, path, ok ? "" : ": ", ok ? "" : folio_error(app.fc));
		if (!ok) {
			char m[300];
			snprintf(m, sizeof m, "The page couldn't be exported: %s", folio_error(app.fc));
			tbx_report(m);
		}
	}
	/* SaveAs_FileSaveCompleted: bit 0 = it worked. */
	_swix(Toolbox_ObjectMiscOp, _INR(0, 3), ok ? 1 : 0, ex.saveas, 12, path);
}

/* SaveAs events for this box (the Save box's are annotui's). */
int export_event(int *b)
{
	if (!ex.saveas || app.idb.self_id != ex.saveas)
		return 0;
	if (b[2] == 0x82bc2)           /* SaveAs_SaveToFile */
		save_to_file(b);
	return 1;
}

void export_forget(docwin *w)
{
	if (ex.w == w) {
		ex.w = NULL;
		if (ex.saveas)
			tbx_hide(ex.saveas);
	}
}
