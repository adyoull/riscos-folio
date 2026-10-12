/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Export page as Draw... (document menu): the page being read as a Draw
 * file (core/drawfile.c), through a Toolbox SaveAs box of its own, type
 * &AFF. Drag the icon to a directory, or to Draw or another program (the
 * Toolbox does the data transfer protocol; Folio writes the file itself). */
#include "app.h"
#include "worker.h"
#include <stdio.h>
#include <string.h>
#include <swis.h>

#define Class_SaveAs 0x82bc0

static struct {
	ObjectId saveas;
	docwin *w;
	int page;
} ex;

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
	}
	ex.w = w;
	ex.page = docwin_current_page(w);
	snprintf(name, sizeof name, "Page%d", ex.page + 1);
	tbx_method(ex.saveas, 3 /* SaveAs_SetFileName */, (int)name, 0, 0);
	_swix(Wimp_GetPointerInfo, _IN(1), ptr);
	pos[0] = ptr[0] - 64;
	pos[1] = ptr[1] + 64;
	tbx_show(ex.saveas, 0, 2, pos, 0, -1);
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
		ok = folio_loc_from_number(doc, ex.page, &loc) == 0 && folio_export_draw(doc, loc, path) == 0;
		core_unlock();
		_swix(Hourglass_Off, 0);
		if (ok)
			_swix(OS_File, _INR(0, 2), 18, path, 0xaff);
		app_log("export: page %d to %s%s%s", ex.page + 1, path, ok ? "" : ": ", ok ? "" : folio_error(app.fc));
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
