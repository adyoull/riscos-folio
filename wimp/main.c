/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio: main program. Starts the Toolbox task, builds the icon bar icon,
 * menus and Info box from in-memory templates, and runs the Wimp_Poll loop.
 * Files arrive by dragging to the icon bar (Message_DataLoad), double-click
 * on a PDF or EPUB (Message_DataOpen) or on the command line. */
#include "app.h"
#include "worker.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <swis.h>
#include <unixlib/local.h>

/* File names are RISC OS names throughout: no UnixLib translation. */
int __riscosify_control = __RISCOSIFY_NO_PROCESS;
/* The C heap (MuPDF's store, page sprites) goes in its own dynamic area. */
const char *const __dynamic_da_name = "Folio Heap";
int __dynamic_da_max_size = 512 * 1024 * 1024;

app_state app;

void app_log(const char *fmt, ...)
{
	va_list ap;
	if (!app.log)
		return;
	va_start(ap, fmt);
	vfprintf(app.log, fmt, ap);
	va_end(ap);
	fputc('\n', app.log);
	fflush(app.log);
}

void app_read_mode(void)
{
	int xlim = 0, ylim = 0;
	_swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 4, &app.xeig);
	_swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 5, &app.yeig);
	_swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 9, &app.log2bpp);
	_swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 11, &xlim);
	_swix(OS_ReadModeVariable, _INR(0, 1) | _OUT(2), -1, 12, &ylim);
	app.screen_w = (xlim + 1) << app.xeig;
	app.screen_h = (ylim + 1) << app.yeig;
}

static void check(_kernel_oserror *e)
{
	if (e) {
		app_log("error: %s", e->errmess);
		tbx_report_error(e);
	}
}

/* ---- Objects ------------------------------------------------------------ */

/* A menu template: header fields then entries, in one block. */
typedef struct {
	tbx_menu m;
	tbx_menu_entry e[8];
} menu_block;

static ObjectId make_menu(menu_block *b, const char *title, int n, int show_event)
{
	ObjectId id = 0;
	b->m.flags = show_event ? 1 : 0;
	b->m.title = title;
	b->m.max_title = (int)strlen(title) + 1;
	b->m.show_event = show_event ? show_event : -1;
	b->m.hide_event = -1;
	b->m.num_entries = n;
	check(tbx_create(Class_Menu, 102, 0, b, (int)(sizeof b->m + n * sizeof b->e[0]), &id));
	return id;
}

static void entry(menu_block *b, int i, int comp, const char *text, int flags,
		int click_event, const char *help)
{
	tbx_menu_entry *e = &b->e[i];
	memset(e, 0, sizeof *e);
	e->flags = flags;
	e->component_id = comp;
	e->text = text;
	e->max_text = (int)strlen(text) + 1;
	e->click_event = click_event;
	e->help_message = help;
	e->max_entry_help = help ? (int)strlen(help) + 1 : 0;
}

static void create_objects(void)
{
	static menu_block ib, dm, em, vm, nm;
	static tbx_proginfo pi;
	static tbx_iconbar icon;

	/* Info box. The licence types a Prog Info box offers don't include the
	 * AGPL, so it goes in the author line. */
	memset(&pi, 0, sizeof pi);
	pi.purpose = "Reads PDF and ebook files";
	pi.author = "\xa9 Andrew Youll 2026. AGPL-3.0";
	pi.version = FOLIO_VERSION;
	check(tbx_create(Class_ProgInfo, 101, 0, &pi, sizeof pi, &app.proginfo));

	/* Icon bar menu: Info, Help..., Quit (Style Guide p46). */
	entry(&ib, 0, 0, "Info", MENU_SUBMENU, 0, "Move right to see information about this version of Folio.");
	entry(&ib, 1, 1, "Help...", 0, EV_HELP, "Click to read Folio's help.");
	entry(&ib, 2, 2, "Quit", 0, EV_QUIT, "Click to leave Folio.");
	app.iconbar_menu = make_menu(&ib, "Folio", 3, 0);
	check(menu_set_submenu(app.iconbar_menu, 0, app.proginfo));

	/* Document menu: View and Navigate submenus. */
	entry(&vm, 0, VIEW_DARK, "Dark page        ^N", 0, EV_DARK, "Click for a black page with light text. Pictures keep their colours.");
	entry(&vm, 1, VIEW_SEPIA, "Sepia page", MENU_DOTTED, EV_SEPIA, "Click for a sepia-tinted page.");
	entry(&vm, 2, VIEW_ZOOM_IN, "Zoom in", 0, EV_ZOOM_IN, "Click to make the pages bigger.");
	entry(&vm, 3, VIEW_ZOOM_OUT, "Zoom out", 0, EV_ZOOM_OUT, "Click to make the pages smaller.");
	entry(&vm, 4, VIEW_ZOOM_100, "Actual size", 0, EV_ZOOM_100, "Click to show the pages at their real size.");
	entry(&vm, 5, VIEW_FIT_WIDTH, "Fit width", MENU_DOTTED, EV_FIT_WIDTH, "Click to fit the pages to the window's width.");
	entry(&vm, 6, VIEW_TOOLBAR, "Toolbar", 0, EV_TOOLBAR, "Click to show or hide the toolbar.");
	entry(&vm, 7, VIEW_THUMBS, "Thumbnails", 0, EV_THUMBS, "Click to show or hide the page thumbnails at the left.");
	app.view_menu = make_menu(&vm, "View", 8, EV_VIEW_SHOWN);

	entry(&nm, 0, 1, "Next page    PageDown", 0, EV_NEXT, "Click to go to the next page.");
	entry(&nm, 1, 2, "Previous page  PageUp", 0, EV_PREV, "Click to go to the previous page.");
	entry(&nm, 2, 3, "First page       Home", 0, EV_FIRST, "Click to go to the first page.");
	entry(&nm, 3, 4, "Last page         End", MENU_DOTTED, EV_LAST, "Click to go to the last page.");
	entry(&nm, 4, NAV_CONTENTS, "Contents...", 0, EV_CONTENTS, "Click to see the document's contents list, and go to any part of it.");
	app.nav_menu = make_menu(&nm, "Navigate", 5, 0);

	entry(&em, 0, EDIT_COPY, "Copy            ^C", 0, EV_COPY, "Click to copy the selected text to the clipboard.");
	entry(&em, 1, EDIT_SELECT_ALL, "Select all      ^A", 0, EV_SELECT_ALL, "Click to select all the text on this page.");
	entry(&em, 2, EDIT_CLEAR, "Clear selection Esc", 0, EV_CLEAR_SEL, "Click to clear the selection.");
	app.edit_menu = make_menu(&em, "Edit", 3, EV_EDIT_SHOWN);

	entry(&dm, 0, 3, "Edit", MENU_SUBMENU, 0, "Move right for copying text.");
	entry(&dm, 1, 1, "View", MENU_SUBMENU, 0, "Move right for the view options.");
	entry(&dm, 2, 2, "Navigate", MENU_SUBMENU | MENU_DOTTED, 0, "Move right for moving through the document.");
	/* The context item (DESIGN §3.4): its text names the word when it's shown. */
	entry(&dm, 3, DOC_FIND, "Find...        F4", 0, EV_FIND, "Click to find text in the document.");
	entry(&dm, 4, DOC_DEFINE, "Define", MENU_FADED, EV_DEFINE, "Click to see what the selected word means (from Wiktionary; needs the network).");
	dm.e[4].max_text = 80;
	app.doc_menu = make_menu(&dm, "Folio", 5, EV_DOCMENU_SHOWN);
	check(menu_set_submenu(app.doc_menu, 3, app.edit_menu));
	check(menu_set_submenu(app.doc_menu, 1, app.view_menu));
	check(menu_set_submenu(app.doc_menu, 2, app.nav_menu));

	/* Icon bar icon. */
	memset(&icon, 0, sizeof icon);
	icon.position = -1;
	icon.sprite_name = "!folio";
	icon.max_sprite_name = 12;
	icon.help_message = "This is the Folio icon.|MDrag a PDF or ebook file here to read it.";
	icon.max_help = 100;
	check(tbx_create(Class_Iconbar, 100, 0, &icon, sizeof icon, &app.iconbar));
	check(iconbar_set_menu(app.iconbar, app.iconbar_menu));
	check(tbx_show(app.iconbar, 0, 0, NULL, 0, -1));
}

/* ---- Messages ----------------------------------------------------------- */

static int accepts_type(int type)
{
	return type == 0xadf || (app.epub_type >= 0 && type == app.epub_type);
}

static void ack_load(int *b)
{
	b[3] = b[2];                /* your_ref = my_ref */
	b[4] = Message_DataLoadAck;
	_swix(Wimp_SendMessage, _INR(0, 2), Wimp_EUserMessage, b, b[1]);
}

static int message(int *b)
{
	if (clip_message(b))
		return 0;
	switch (b[4]) {
	case Message_Quit:
		docwin_close_all();
		return 1;
	case Message_DataLoad:
		/* Dropped on the icon bar or a document window: open it. */
		ack_load(b);
		docwin_open((const char *)(b + 11));
		break;
	case Message_DataOpen:
		/* Double-click on a file: only types Folio reads. */
		if (accepts_type(b[10])) {
			ack_load(b);
			docwin_open((const char *)(b + 11));
		}
		break;
	case Message_PreQuit:
		/* Nothing to save in a reader yet. */
		break;
	case Message_ModeChange:
		app_read_mode();
		docwin_mode_changed();
		break;
	}
	return 0;
}

/* ---- Toolbox events ---------------------------------------------------- */

static docwin *window_for_event(void)
{
	/* Keyboard shortcuts come from the window itself. Menu entries come from
	 * the document menu or its submenus, which every window shares: they
	 * belong to the window the menu was last opened from (recorded when the
	 * document menu's show event arrives, whose parent is that window). */
	ObjectId self = app.idb.self_id;
	if (self == app.doc_menu || self == app.view_menu || self == app.nav_menu || self == app.edit_menu)
		return docwin_from_object(app.menu_window);
	return docwin_from_object(self);
}

static int toolbox_event(int *b)
{
	int code = b[2];
	docwin *w;
	app_log("event &%x: self &%x/%d parent &%x/%d ancestor &%x/%d", code,
		app.idb.self_id, app.idb.self_component, app.idb.parent_id,
		app.idb.parent_component, app.idb.ancestor_id, app.idb.ancestor_component);
	switch (code) {
	case Toolbox_Error:
		tbx_report((const char *)(b + 5));
		break;
	case EV_QUIT:
		docwin_close_all();
		return 1;
	case EV_HELP:
		_swix(OS_CLI, _IN(0), "Filer_Run <Folio$Dir>.!Help");
		break;
	case EV_DOCMENU_SHOWN:
		app.menu_window = app.idb.parent_id;
		if (!docwin_from_object(app.menu_window)) {
			/* Not named as the parent: use the window under the pointer,
			 * where Menu was just pressed. */
			int ptr[5];
			_swix(Wimp_GetPointerInfo, _IN(1), ptr);
			w = docwin_from_wimp(ptr[3]);
			app.menu_window = w ? docwin_object(w) : 0;
			app_log("  menu window from the pointer: &%x", app.menu_window);
		}
		if ((w = docwin_from_object(app.menu_window)) != NULL)
			docwin_doc_menu_shown(w);
		break;
	case EV_DICT_COPY:
		dict_copy();
		break;
	case EV_FIND_NEXT:
	case EV_FIND_PREV:
	case EV_FIND_CANCEL:
		find_event(code);
		break;
	case EV_EDIT_SHOWN:
		if ((w = window_for_event()) != NULL)
			docwin_edit_menu_shown(w);
		break;
	case EV_VIEW_SHOWN:
		if ((w = window_for_event()) != NULL)
			docwin_view_menu_shown(w);
		break;
	default:
		if (code >= EV_DARK && code < 0x200) {
			if ((w = window_for_event()) != NULL)
				docwin_event(w, code);
			else
				app_log("  no window for event &%x", code);
		}
		break;
	}
	return 0;
}

/* ---- Main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
	static const int messages[] = {
		Message_DataLoad, Message_DataOpen, Message_PreQuit, Message_ModeChange,
		Message_DataSaveAck, Message_DataLoadAck, 0xf /* ClaimEntity */, 0x10 /* DataRequest */, 0
	};
	int block[64];
	int quit = 0, busy = 0;
	_kernel_oserror *e;
	const char *scrap = getenv("Wimp$ScrapDir");

	if (scrap && *scrap) {
		char path[256];
		snprintf(path, sizeof path, "%s.FolioLog", scrap);
		app.log = fopen(path, "w");
	}
	app_log("Folio %s", FOLIO_VERSION);

	e = tbx_init("<Folio$Dir>", messages, &app.idb, &app.task, &app.sprites);
	if (e) {
		app_log("Toolbox_Initialise: %s", e->errmess);
		tbx_report_error(e);
		return 1;
	}
	app_read_mode();
	app_log("mode: eig %d,%d, %d bpp, screen %dx%d OS units", app.xeig, app.yeig,
		1 << app.log2bpp, app.screen_w, app.screen_h);
	if (app.log2bpp < 4)
		tbx_report("This test build of Folio needs a 32 thousand or more colour screen mode.");

	/* The EPUB filetype, if MimeMap knows one (format 3 = extension). */
	app.epub_type = -1;
	if (_swix(0x50b00 /* MimeMap_Translate */, _INR(0, 2) | _OUT(3), 3, "epub", 0, &app.epub_type))
		app.epub_type = -1;

	app.fc = folio_new(128 << 20);
	if (!app.fc) {
		tbx_report("Folio couldn't start MuPDF (not enough memory?).");
		return 1;
	}
	if (worker_init())
		app_log("no worker thread: pages will be drawn in the poll loop");
	create_objects();
	if (argc > 1)
		docwin_open(argv[1]);

	while (!quit) {
		int event;
		/* Null events only while there is background work. */
		e = _swix(Wimp_Poll, _INR(0, 1) | _OUT(0), busy ? 0 : 1, block, &event);
		if (e) {
			tbx_report_error(e);
			continue;
		}
		switch (event) {
		case Wimp_ENull:
			break;
		case Wimp_ERedrawWindow: {
			docwin *w = docwin_from_wimp(block[0]);
			if (w)
				docwin_redraw(w, block);
			else if (dict_owns(block[0]))
				dict_redraw(block);
			else if (contents_owns(block[0]))
				contents_redraw(block);
			else if (thumbs_from_wimp(block[0]))
				thumbs_redraw(thumbs_from_wimp(block[0]), block);
			else if (toolbar_tip_owns(block[0]))
				toolbar_tip_redraw(block);
			break;
		}
		case Wimp_EOpenWindow:
			/* The pop-up (the Toolbox opens document windows itself). */
			if (dict_owns(block[0]))
				_swix(Wimp_OpenWindow, _IN(1), block);
			else if (thumbs_from_wimp(block[0]))
				thumbs_open_request(thumbs_from_wimp(block[0]), block);
			break;
		case Wimp_ECloseWindow: {
			docwin *w = docwin_from_wimp(block[0]);
			if (w)
				docwin_close_request(w);
			break;
		}
		case Wimp_EPointerEntering:
		case Wimp_EPointerLeaving:
			if (docwin_from_toolbar(app.idb.self_id))
				toolbar_pointer(block[0], event == Wimp_EPointerEntering);
			break;
		case Wimp_EMouseClick: {
			toolbar_tip_hide();
			docwin *w = docwin_from_wimp(block[3]);
			if (w)
				docwin_click(w, block);
			else if (dict_owns(block[3]))
				dict_click(block);
			else if (contents_owns(block[3]))
				contents_click(block);
			else if (thumbs_from_wimp(block[3]))
				thumbs_click(thumbs_from_wimp(block[3]), block);
			else if ((w = docwin_from_toolbar(app.idb.self_id)) != NULL)
				docwin_toolbar_click(w, app.idb.self_component, block);
			break;
		}
		case Wimp_EKeyPressed: {
			/* Return or Escape in the toolbar's page field; other keys the
			 * window's shortcuts didn't take go on to other tasks. */
			docwin *w = docwin_from_toolbar(app.idb.self_id);
			if (!(w && docwin_toolbar_key(w, block[6])))
				_swix(Wimp_ProcessKey, _IN(0), block[6]);
			break;
		}
		case Wimp_EUserMessage:
		case Wimp_EUserMessageRecorded:
			quit = message(block);
			break;
		case Wimp_EToolboxEvent:
			quit = toolbox_event(block);
			break;
		}
		busy = docwin_background();
		busy |= dict_background();
		busy |= find_background();
		busy |= toolbar_background();
	}
	dict_quit();
	app_log("quit");
	return 0;
}
