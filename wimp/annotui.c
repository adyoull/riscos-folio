/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* The dialogues for annotations: the Note box (type or edit a note's text),
 * Save (the Toolbox SaveAs object: drag the file icon or click Save, Style
 * Guide p84) and Discard/Cancel/Save (the Toolbox DCS object, when a window
 * with unsaved changes is closed or Folio is asked to quit, SG p86).
 *
 * Folio saves the file itself (a "type 2" SaveAs client, User Interface
 * Toolbox p261): SaveAs_SaveToFile gives the path, docwin_save writes it,
 * and SaveAs_FileSaveCompleted reports how that went. */
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define Class_SaveAs 0x82bc0
#define Class_DCS    0x82a80

enum { N_TEXT = 1, N_DELETE, N_CANCEL, N_OK };

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

static struct {
	writable_gadget text;
	button_gadget del, cancel, ok;
} ng;

static struct {
	ObjectId note, saveas, dcs;
	/* The Note box: which window, page and annotation (-1: a new one at 'at'). */
	docwin *note_w;
	int note_page, note_index;
	folio_point note_at;
	docwin *save_w;          /* the window the Save box is for */
	docwin *dcs_w;           /* the window the DCS box is for */
	int close_after_save;    /* Save from the DCS box: close once saved */
} ui;

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

static void button(button_gadget *b, int id, int flags, int x0, const char *text, int event,
		const char *help)
{
	hdr(&b->h, 128, sizeof *b, id, x0, -164, x0 + 176, -96, help);
	b->h.flags = flags;
	b->text = text;
	b->max_text = (int)strlen(text) + 1;
	b->event = event;
}

static ObjectId create_note(void)
{
	static tbx_window t;
	static char title[32] = "Note";
	ObjectId id = 0;
	_kernel_oserror *e;
	memset(&ng, 0, sizeof ng);
	hdr(&ng.text.h, 512, sizeof ng.text, N_TEXT, 16, -80, 812, -24,
		"Type the note here.|MPress Return (or click OK) to keep it.");
	ng.text.text = "";
	ng.text.max_text = 256;
	ng.text.before = -1;
	ng.text.after = -1;
	/* Delete at the left; Cancel and OK (the default) at the right (SG p55). */
	button(&ng.del, N_DELETE, 0, 16, "Delete", EV_NOTE_DELETE, "Click to delete this note.");
	button(&ng.cancel, N_CANCEL, 2 /* Cancel */, 452, "Cancel", EV_NOTE_CANCEL,
		"Click to close this box without changing the note.");
	button(&ng.ok, N_OK, 1 /* default */, 636, "OK", EV_NOTE_OK, "Click to keep the note.");
	memset(&t, 0, sizeof t);
	t.flags = (1 << 1) | (1 << 2);
	t.help_message = "Use this box to write a note on the page.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.num_gadgets = 4;
	t.gadgets = &ng;
	t.default_focus = N_TEXT;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = 828;
	t.window.vis_y1 = 180;
	t.window.behind = -1;
	t.window.window_flags = 0x80000002u | (1u << 4) | (1u << 26);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 1;
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -180;
	t.window.work_x1 = 828;
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

/* Opens the Note box: index -1 for a new note at 'at' (text NULL), or the
 * note to edit and its text (UTF-8). */
void note_open(docwin *w, int page, int index, folio_point at, const char *text)
{
	int ptr[5], pos[2];
	char *shown;
	if (!ui.note && !(ui.note = create_note()))
		return;
	ui.note_w = w;
	ui.note_page = page;
	ui.note_index = index;
	ui.note_at = at;
	shown = app_desktop_text(text ? text : "");
	tbx_method(ui.note, 512 /* WritableField_SetValue */, N_TEXT, (int)(shown ? shown : ""), 0);
	free(shown);
	/* Delete only for an existing note. */
	tbx_method(ui.note, 65 /* Gadget_SetFlags */, N_DELETE, index >= 0 ? 0 : (int)(1u << 31), 0);
	window_set_title(ui.note, index >= 0 ? "Note" : "New note");
	_swix(Wimp_GetPointerInfo, _IN(1), ptr);
	pos[0] = ptr[0] - 64;
	pos[1] = ptr[1] + 64;
	tbx_show(ui.note, 0, 2, pos, 0, -1);
}

static void note_event(int code)
{
	docwin *w = ui.note_w;
	if (!w)
		return;
	if (code == EV_NOTE_OK) {
		char text[260];
		char *utf8;
		text[0] = 0;
		_swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, ui.note, 513, N_TEXT, text, (int)sizeof text);
		text[sizeof text - 1] = 0;
		utf8 = app_utf8_text(text);
		docwin_note_done(w, ui.note_page, ui.note_index, ui.note_at, utf8 ? utf8 : text);
		free(utf8);
	} else if (code == EV_NOTE_DELETE && ui.note_index >= 0) {
		docwin_delete_annot(w, ui.note_page, ui.note_index);
	}
	if (code != EV_NOTE_CANCEL)
		tbx_hide(ui.note);
	ui.note_w = NULL;
}

/* ---- Save --------------------------------------------------------------- */

void save_open(docwin *w, int close_after)
{
	static struct {
		int flags;
		const char *filename;
		int filetype;
		const char *title;
		int max_title;
		const char *window;
	} t;
	int ptr[5], pos[2];
	if (!ui.saveas) {
		_kernel_oserror *e;
		memset(&t, 0, sizeof t);
		t.flags = 4;                 /* no Selection option; Folio saves the file itself */
		t.filename = "Document";
		t.filetype = 0xadf;
		e = tbx_create(Class_SaveAs, 100, 0, &t, sizeof t, &ui.saveas);
		if (e) {
			tbx_report_error(e);
			ui.saveas = 0;
			return;
		}
	}
	ui.save_w = w;
	ui.close_after_save = close_after;
	tbx_method(ui.saveas, 3 /* SaveAs_SetFileName */, (int)docwin_path(w), 0, 0);
	_swix(Wimp_GetPointerInfo, _IN(1), ptr);
	pos[0] = ptr[0] - 64;
	pos[1] = ptr[1] + 64;
	tbx_show(ui.saveas, 0, 2, pos, 0, -1);
}

static void save_to_file(int *b)
{
	const char *path = (const char *)(b + 4);
	int ok = 0;
	if (ui.save_w)
		ok = docwin_save(ui.save_w, path) == 0;
	/* SaveAs_FileSaveCompleted: bit 0 = it worked (else the box stays). */
	_swix(Toolbox_ObjectMiscOp, _INR(0, 3), ok ? 1 : 0, ui.saveas, 12, path);
	if (ok && ui.close_after_save) {
		docwin *w = ui.save_w;
		ui.save_w = NULL;
		ui.close_after_save = 0;
		docwin_close(w);
	}
}

/* ---- Discard / Cancel / Save -------------------------------------------- */

void dcs_open(docwin *w)
{
	static struct {
		int flags;
		const char *title;
		int max_title;
		const char *message;
		int max_message;
		const char *window;
	} t;
	static char message[300];
	if (!ui.dcs) {
		_kernel_oserror *e;
		memset(&t, 0, sizeof t);
		t.message = "This document has unsaved changes.";
		t.max_message = sizeof message;
		e = tbx_create(Class_DCS, 100, 0, &t, sizeof t, &ui.dcs);
		if (e) {
			tbx_report_error(e);
			ui.dcs = 0;
			return;
		}
	}
	ui.dcs_w = w;
	snprintf(message, sizeof message, "'%s' has unsaved changes.", docwin_title(w));
	tbx_method(ui.dcs, 1 /* DCS_SetMessage */, (int)message, 0, 0);
	tbx_show(ui.dcs, 0, 0, NULL, 0, -1);
}

/* Toolbox events for these boxes. Returns 1 if it was one of theirs. */
int annotui_event(int *b)
{
	switch (b[2]) {
	case EV_NOTE_OK:
	case EV_NOTE_DELETE:
	case EV_NOTE_CANCEL:
		note_event(b[2]);
		return 1;
	case 0x82bc2:               /* SaveAs_SaveToFile */
		save_to_file(b);
		return 1;
	case 0x82bc1:               /* SaveAs_DialogueCompleted */
	case 0x82bc4:               /* SaveAs_SaveCompleted */
		return 1;
	case 0x82a81:               /* DCS_Discard */
		if (ui.dcs_w) {
			docwin *w = ui.dcs_w;
			ui.dcs_w = NULL;
			docwin_close(w);
		}
		return 1;
	case 0x82a82:               /* DCS_Save */
		if (ui.dcs_w) {
			docwin *w = ui.dcs_w;
			ui.dcs_w = NULL;
			save_open(w, 1);
		}
		return 1;
	case 0x82a84:               /* DCS_Cancel */
		ui.dcs_w = NULL;
		return 1;
	}
	return 0;
}

/* w is closing: forget it everywhere here. */
void annotui_forget(docwin *w)
{
	if (ui.note_w == w) {
		ui.note_w = NULL;
		tbx_hide(ui.note);
	}
	if (ui.save_w == w) {
		ui.save_w = NULL;
		ui.close_after_save = 0;
		tbx_hide(ui.saveas);
	}
	if (ui.dcs_w == w) {
		ui.dcs_w = NULL;
		tbx_hide(ui.dcs);
	}
}
