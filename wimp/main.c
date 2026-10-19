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
/* The size "Folio Heap" reserves. A dynamic area takes its whole maximum
 * of the logical address space when it's made, used or not, and RISC OS has
 * only so much for every program (AddrSpace's findings: Chris's Pi 4 had
 * 446 MB left with 63 areas). The kernel also caps a new area (DALimit:
 * 128 MB on most machines), and UnixLib 5.0.3.3 adds "Folio Heap 2", "3"...
 * only when one fills; big blocks (page pictures, the voice's weights) come
 * from ARMEABISupport's mmap areas of their own size anyway. So ask for
 * 128 MB, as Matinee does, not the 512 MB of test1 to test33. What goes in
 * the heap and mmaps: MuPDF's store (64 MB), page pictures (48 MB, more only
 * for pages on screen), thumbnails (16 MB), the voice while reading aloud
 * (about 65 MB), text recognition while it runs (up to about 50 MB). */
int __dynamic_da_max_size = 128 * 1024 * 1024;

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
	tbx_menu_entry e[20];
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
	static menu_block ib, dm, em, vm, nm, sm, om;
	int ndm;
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
	entry(&ib, 1, 3, "Bookshelf...", 0, EV_BOOKSHELF, "Click to see your bookshelf.");
	entry(&ib, 2, 5, "Create PDF...", 0, EV_CREATE_PDF, "Click to open a window to drag PDFs, pictures and other documents into, in order, and join them into one new PDF.");
	entry(&ib, 3, 4, "Choices...", 0, EV_CHOICES, "Click to change Folio's choices.");
	entry(&ib, 4, 1, "Help...", 0, EV_HELP, "Click to read Folio's help.");
	entry(&ib, 5, 2, "Quit", 0, EV_QUIT, "Click to leave Folio.");
	app.iconbar_menu = make_menu(&ib, "Folio", 6, 0);
	check(menu_set_submenu(app.iconbar_menu, 0, app.proginfo));

	/* Document menu: View and Navigate submenus. */
	entry(&vm, 0, VIEW_DARK, "Dark page        ^N", 0, EV_DARK, "Click for a black page with light text. Pictures keep their colours.");
	entry(&vm, 1, VIEW_SEPIA, "Sepia page", MENU_DOTTED, EV_SEPIA, "Click for a sepia-tinted page.");
	entry(&vm, 2, VIEW_ZOOM_IN, "Zoom in", 0, EV_ZOOM_IN, "Click to make the pages bigger.");
	entry(&vm, 3, VIEW_ZOOM_OUT, "Zoom out", 0, EV_ZOOM_OUT, "Click to make the pages smaller.");
	entry(&vm, 4, VIEW_ZOOM_100, "Actual size", 0, EV_ZOOM_100, "Click to show the pages at their real size.");
	entry(&vm, 5, VIEW_FIT_WIDTH, "Fit width", 0, EV_FIT_WIDTH, "Click to fit the pages to the window's width.");
	entry(&vm, 6, VIEW_DEFAULT_ZOOM, "Use this scale for new documents", MENU_DOTTED, EV_DEFAULT_ZOOM, "Click to open documents at this window's scale from now on (ticked when this is that scale).");
	entry(&vm, 7, VIEW_TWO_PAGES, "Two pages", 0, EV_TWO_PAGES, "Click to show the pages two side by side, or one at a time.");
	entry(&vm, 8, VIEW_COVER_ALONE, "First page alone", MENU_DOTTED, EV_COVER_ALONE, "Click to show the first page on its own, so facing pages pair up as in a book.");
	entry(&vm, 9, VIEW_TOOLBAR, "Toolbar", 0, EV_TOOLBAR, "Click to show or hide the toolbar.");
	entry(&vm, 10, VIEW_THUMBS, "Thumbnails", MENU_DOTTED, EV_THUMBS, "Click to show or hide the page thumbnails at the left.");
	entry(&vm, 11, VIEW_FULL_SCREEN, "Full screen      F11", 0, EV_FULL_SCREEN, "Click to read with the document filling the screen. F11 or Escape returns to the desktop.");
	app.view_menu = make_menu(&vm, "View", 12, EV_VIEW_SHOWN);

	entry(&nm, 0, 1, "Next page    PageDown", 0, EV_NEXT, "Click to go to the next page.");
	entry(&nm, 1, 2, "Previous page  PageUp", 0, EV_PREV, "Click to go to the previous page.");
	entry(&nm, 2, 3, "First page       Home", 0, EV_FIRST, "Click to go to the first page.");
	entry(&nm, 3, 4, "Last page         End", 0, EV_LAST, "Click to go to the last page.");
	entry(&nm, 4, NAV_GOTO, "Go to page...      F5", MENU_DOTTED, EV_GOTO, "Click to type a page number to go to.");
	entry(&nm, 5, NAV_CONTENTS, "Contents...", MENU_DOTTED, EV_CONTENTS, "Click to see the document's contents list, and go to any part of it.");
	entry(&nm, 6, NAV_BACK, "Back          ^Left", 0, EV_BACK, "Click to go back to where you were before following a link or going to a page.");
	app.nav_menu = make_menu(&nm, "Navigate", 7, EV_NAV_SHOWN);

	/* Undo and Redo first (Style Guide p40; DESIGN §3.8). */
	entry(&em, 0, EDIT_UNDO, "Undo              ^Z", 0, EV_UNDO, "Click to undo the last change to the notes and markings.");
	entry(&em, 1, EDIT_REDO, "Redo              ^Y", MENU_DOTTED, EV_REDO, "Click to redo the change you undid.");
	entry(&em, 2, EDIT_COPY, "Copy              ^C", 0, EV_COPY, "Click to copy the selected text to the clipboard.");
	entry(&em, 3, EDIT_SELECT_ALL, "Select all        ^A", 0, EV_SELECT_ALL, "Click to select all the text on this page.");
	entry(&em, 4, EDIT_CLEAR, "Clear selection  Esc", MENU_DOTTED, EV_CLEAR_SEL, "Click to clear the selection.");
	entry(&em, 5, EDIT_HILITE, "Highlight", 0, EV_HIGHLIGHT, "Click to highlight the selected text (PDFs).");
	entry(&em, 6, EDIT_ULINE, "Underline", 0, EV_UNDERLINE, "Click to underline the selected text (PDFs).");
	entry(&em, 7, EDIT_STRIKE, "Strike out", 0, EV_STRIKE, "Click to strike out the selected text (PDFs).");
	entry(&em, 8, EDIT_NOTE, "Note...", 0, EV_NOTE, "Click to add a note at the selection, or at the top of the page (PDFs).");
	entry(&em, 9, EDIT_DELANNOT, "Delete annotation Del", 0, EV_DEL_ANNOT, "Click to delete the annotation you clicked on.");
	app.edit_menu = make_menu(&em, "Edit", 10, EV_EDIT_SHOWN);

	entry(&dm, 0, DOC_SAVE, "Save...        F3", 0, EV_SAVE, "Click to save the document with its notes and markings (PDFs).");
	entry(&dm, 1, DOC_PRINT, "Print...    Print", 0, EV_PRINT, "Click to print the document (with !Printers loaded).");
	entry(&dm, 2, DOC_EXPORT_DRAW, "Export page as Draw...", 0, EV_EXPORT_DRAW, "Click to save the page you're reading as a Draw file: its text as outlines, each picture as a sprite of its own.");
	entry(&dm, 3, DOC_SAVE_PAGES, "Save pages as PDF...", 0, EV_SAVE_PAGES, "Click to make a new PDF of some of the pages, such as 1-10. A PDF's pages are copied just as they are.");
	entry(&dm, 4, DOC_SAVE_PICTURES, "Save page pictures...", 0, EV_SAVE_PICTURES, "Click to save pages as picture files in a directory: a scanned page's own JPEG, unchanged; other pages drawn as PNG.");
	entry(&dm, 5, DOC_PASSWORD, "Password...", 0, EV_PASSWORD, "Click to put a password on the document, or take one off (PDFs).");
	entry(&dm, 6, DOC_INFO, "Document info...", MENU_DOTTED, EV_DOCINFO, "Click to see the document's properties: title, author, dates, page size, security and more.");
	entry(&dm, 7, 3, "Edit", MENU_SUBMENU, 0, "Move right for undo, copying, highlighting and notes.");
	entry(&dm, 8, 1, "View", MENU_SUBMENU, 0, "Move right for the view options.");
	entry(&dm, 9, 2, "Navigate", MENU_SUBMENU | MENU_DOTTED, 0, "Move right for moving through the document.");
	/* The context item (DESIGN §3.4): its text names the word when it's shown. */
	entry(&dm, 10, DOC_FIND, "Find...        F4", 0, EV_FIND, "Click to find text in the document.");
	entry(&dm, 11, DOC_DEFINE, "Define", MENU_FADED | MENU_DOTTED, EV_DEFINE, "Click to see what the selected word means (from Wiktionary; needs the network).");
	dm.e[11].max_text = 80;
	entry(&dm, 12, DOC_SHELF, "On bookshelf", 0, EV_ADD_SHELF, "Click to put this document on the bookshelf, or take it off.");
	ndm = 13;
	/* Text recognition: only with the OCR plugin. */
	if (ocr_available()) {
		entry(&dm, ndm++, DOC_OCR, "Text recognition", MENU_SUBMENU, 0, "Move right to read the text in scanned pages, so it can be found, selected, copied and read aloud.");
		entry(&om, 0, OCR_PAGE, "Recognise this page", 0, EV_OCR_PAGE, "Click to read the text in the picture of this page (even if it has text of its own).");
		entry(&om, 1, OCR_ALL, "Recognise all pages", MENU_DOTTED, EV_OCR_ALL, "Click to read the text in every scanned page, in the background; click again to stop.");
		entry(&om, 2, OCR_AUTO, "Scanned pages as I read", MENU_DOTTED, EV_OCR_AUTO, "Click to choose whether scanned pages are read as you come to them.");
		entry(&om, 3, OCR_ACCURATE, "Accurate", 0, EV_OCR_ACCURATE, "Click to recognise text with the larger data: fewer mistakes, about twice as long.");
		entry(&om, 4, OCR_FAST, "Fast", 0, EV_OCR_FAST, "Click to recognise text with the smaller data: quicker, a few more mistakes.");
		app.ocr_menu = make_menu(&om, "Text", 5, EV_OCR_SHOWN);
	}
	/* Reading aloud: only with the Speech plugin. */
	if (speech_available()) {
		dm.e[ndm - 1].flags |= MENU_DOTTED;
		entry(&dm, ndm++, DOC_READ, "Read aloud", 0, EV_READ_ALOUD, "Click to hear the document read aloud from here (or from the selection); click again to stop.");
		entry(&dm, ndm++, DOC_SPEAKSEL, "Speak selection", 0, EV_SPEAK_SEL, "Click to hear just the selected text.");
		entry(&dm, ndm++, DOC_SPEED, "Reading speed", MENU_SUBMENU, 0, "Move right to choose how fast it reads.");
		entry(&sm, 0, SPEED_SLOW, "Slower", 0, EV_SPEED_SLOW, "Click to read more slowly.");
		entry(&sm, 1, SPEED_NORMAL, "Normal", 0, EV_SPEED_NORMAL, "Click to read at a comfortable pace for listening.");
		entry(&sm, 2, SPEED_FAST, "Faster", 0, EV_SPEED_FAST, "Click to read more quickly (the voice's own speed).");
		app.speed_menu = make_menu(&sm, "Speed", 3, EV_SPEED_SHOWN);
	}
	app.doc_menu = make_menu(&dm, "Folio", ndm, EV_DOCMENU_SHOWN);
	if (app.speed_menu)
		check(menu_set_submenu(app.doc_menu, DOC_SPEED, app.speed_menu));
	if (app.ocr_menu)
		check(menu_set_submenu(app.doc_menu, DOC_OCR, app.ocr_menu));
	check(menu_set_submenu(app.doc_menu, 3, app.edit_menu));
	check(menu_set_submenu(app.doc_menu, 1, app.view_menu));
	check(menu_set_submenu(app.doc_menu, 2, app.nav_menu));

	/* Icon bar icon. */
	memset(&icon, 0, sizeof icon);
	icon.position = -1;
	icon.sprite_name = "!folio";
	icon.max_sprite_name = 12;
	icon.help_message = "This is the Folio icon.|MClick Select to see your bookshelf. Drag a PDF or ebook file here to read it, or a directory of pictures to make a PDF.";
	icon.flags = 1 << 5;            /* an event for Select clicks (Toolbox p152) */
	icon.select_event = EV_BOOKSHELF;
	icon.max_help = (int)strlen(icon.help_message) + 1;
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

/* Opens a document. The page to open it at, if any: page (1-based, from
 * -page on the command line), or the system variable Folio$OpenPage, which
 * is then unset. So, from an Obey file or a program:
 *     Set Folio$OpenPage 12
 *     Filer_Run SDFS::Disc.$.Docs.Manual/pdf
 * opens the manual at page 12, whether Folio is running yet or not. */
static void open_file(const char *path, int page)
{
	const char *v = getenv("Folio$OpenPage");
	docwin *w;
	if (v && *v) {
		if (page <= 0)
			page = atoi(v);
		_swix(OS_SetVarVal, _INR(0, 4), "Folio$OpenPage", 0, -1, 0, 0);
	}
	w = docwin_open(path);
	if (w && page > 0)
		docwin_open_page(w, page - 1);
}

static int message(int *b)
{
	if (clip_message(b))
		return 0;
	switch (b[4]) {
	case 0x400c9:                 /* Message_MenusDeleted */
		formui_menus_deleted(b[5]);
		break;
	case Message_Quit:
		docwin_close_all();
		return 1;
	case Message_DataLoad:
		/* Dropped on the Bookshelf: put it there. */
		if (bookshelf_data_load(b)) {
			ack_load(b);
			break;
		}
		/* Dropped in the Create PDF window: added to its list. */
		if (combine_data_load(b)) {
			ack_load(b);
			break;
		}
		/* A directory of pictures: a new PDF of them. */
		if (b[10] == 0x1000) {
			ack_load(b);
			pagesave_folder((const char *)(b + 11));
			break;
		}
		/* Dropped on the icon bar or a document window: open it. */
		ack_load(b);
		open_file((const char *)(b + 11), 0);
		break;
	case Message_DataOpen:
		/* Double-click on a file: only types Folio reads. */
		if (accepts_type(b[10])) {
			ack_load(b);
			open_file((const char *)(b + 11), 0);
		}
		break;
	case Message_PreQuit: {
		/* Unsaved changes: object (acknowledge the message) and ask
		 * (PRM Message_PreQuit; Style Guide p86). */
		docwin *w = docwin_first_modified();
		if (w) {
			b[3] = b[2];
			_swix(Wimp_SendMessage, _INR(0, 2), Wimp_EUserMessageAcknowledge, b, b[1]);
			dcs_open(w);
		}
		break;
	}
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
	if (self == app.doc_menu || self == app.view_menu || self == app.nav_menu || self == app.edit_menu ||
			(self == app.ocr_menu && self))
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
	if (export_event(b) || pagesave_saveas_event(b) || annotui_event(b) || passui_event(code) || print_event(b))
		return 0;
	switch (code) {
	case Toolbox_Error:
		tbx_report((const char *)(b + 5));
		break;
	case EV_QUIT:
		/* Unsaved changes: ask first (Style Guide p86). */
		if ((w = docwin_first_modified()) != NULL) {
			dcs_open(w);
			break;
		}
		docwin_close_all();
		return 1;
	case EV_BOOKSHELF:
		bookshelf_open();
		break;
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
	case EV_SH_OPEN: case EV_SH_REMOVE: case EV_SH_FILER: case EV_SH_RECENT: case EV_SH_TITLE:
	case EV_SH_AUTHOR: case EV_SH_FOLDERS: case EV_SH_REFRESH: case EV_SH_MISSING:
		bookshelf_event(code);
		break;
	case EV_FIND_NEXT:
	case EV_FIND_PREV:
	case EV_FIND_CANCEL:
		find_event(code);
		break;
	case EV_GOTO_OK:
	case EV_GOTO_CANCEL:
		gotopage_event(code);
		break;
	case EV_PAGES_OK:
	case EV_PAGES_CANCEL:
		pagesave_event(code);
		break;
	case EV_CREATE_PDF: case EV_CP_UP: case EV_CP_DOWN: case EV_CP_REMOVE: case EV_CP_SORT:
	case EV_CP_CLEAR: case EV_CP_CREATE:
		combine_event(code);
		break;
	case EV_SPEED_SLOW:
	case EV_SPEED_NORMAL:
	case EV_SPEED_FAST:
		/* Kept with the choices. */
		app.ch.speed = code == EV_SPEED_SLOW ? SPEED_SLOW : code == EV_SPEED_FAST ? SPEED_FAST :
			SPEED_NORMAL;
		speech_set_speed(app.ch.speed);
		choices_save();
		break;
	case EV_SPEED_SHOWN:
		menu_set_tick(app.speed_menu, SPEED_SLOW, speech_speed() == SPEED_SLOW);
		menu_set_tick(app.speed_menu, SPEED_NORMAL, speech_speed() == SPEED_NORMAL);
		menu_set_tick(app.speed_menu, SPEED_FAST, speech_speed() == SPEED_FAST);
		break;
	case EV_CHOICES:
		choices_open();
		break;
	case EV_ASK_OK:
	case EV_ASK_CANCEL:
		ask_event(code);
		break;
	case EV_CHOICES_DEFAULT:
	case EV_CHOICES_CANCEL:
	case EV_CHOICES_SAVE:
		choices_event(code);
		break;
	case EV_OCR_AUTO:
		app.ch.ocr_auto = !app.ch.ocr_auto;
		choices_save();
		break;
	case EV_OCR_ACCURATE:
	case EV_OCR_FAST:
		app.ch.ocr_fast = code == EV_OCR_FAST;
		choices_save();
		docwin_ocr_settings_changed();
		break;
	case EV_OCR_SHOWN:
		menu_set_tick(app.ocr_menu, OCR_AUTO, app.ch.ocr_auto);
		menu_set_tick(app.ocr_menu, OCR_ACCURATE, ocr_has_accurate() && (!app.ch.ocr_fast || !ocr_has_fast()));
		menu_set_tick(app.ocr_menu, OCR_FAST, ocr_has_fast() && (app.ch.ocr_fast || !ocr_has_accurate()));
		menu_set_fade(app.ocr_menu, OCR_ACCURATE, !ocr_has_accurate());
		menu_set_fade(app.ocr_menu, OCR_FAST, !ocr_has_fast());
		docwin_ocr_menu_shown(docwin_from_object(app.menu_window));
		break;
	case EV_EDIT_SHOWN:
		if ((w = window_for_event()) != NULL)
			docwin_edit_menu_shown(w);
		break;
	case EV_VIEW_SHOWN:
		if ((w = window_for_event()) != NULL)
			docwin_view_menu_shown(w);
		break;
	case EV_NAV_SHOWN:
		if ((w = window_for_event()) != NULL)
			docwin_nav_menu_shown(w);
		break;
	default:
		/* A document window's: its menus' entries (below &200), and those
		 * numbered later (test49's Save pages and Save page pictures, and
		 * Full screen, were outside the range and did nothing). */
		if ((code >= EV_DARK && code < 0x200) || code == EV_FULL_SCREEN || code == EV_SAVE_PAGES ||
				code == EV_SAVE_PICTURES) {
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
		Message_DataSaveAck, Message_DataLoadAck, 0xf /* ClaimEntity */, 0x10 /* DataRequest */,
		0x400c9 /* MenusDeleted */, 0
	};
	int block[64];
	int quit = 0, busy = 0;
	unsigned wake = 0;
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
	choices_load();
	app_log("mode: eig %d,%d, %d bpp, screen %dx%d OS units", app.xeig, app.yeig,
		1 << app.log2bpp, app.screen_w, app.screen_h);
	if (app.log2bpp < 4)
		tbx_report("This test build of Folio needs a 32 thousand or more colour screen mode.");

	/* The EPUB filetype, if MimeMap knows one (format 3 = extension). */
	app.epub_type = -1;
	if (_swix(0x50b00 /* MimeMap_Translate */, _INR(0, 2) | _OUT(3), 3, "epub", 0, &app.epub_type))
		app.epub_type = -1;

	fontsetup();
	app.fc = folio_new(64 << 20);
	if (!app.fc) {
		tbx_report("Folio couldn't start MuPDF (not enough memory?).");
		return 1;
	}
	if (worker_init())
		app_log("no worker thread: pages will be drawn in the poll loop");
	speech_init();
	speech_set_speed(app.ch.speed);
	ocr_plugin_init();
	create_objects();
	bookshelf_init();
	/* !Folio [-page N] [file] */
	{
		int i, page = 0;
		for (i = 1; i < argc; i++) {
			if (!strcmp(argv[i], "-page") && i + 1 < argc)
				page = atoi(argv[++i]);
			else
				open_file(argv[i], page);
		}
	}

	while (!quit) {
		int event;
		/* Null events only while there is background work. */
		/* Null events only while there is background work; reading aloud
		 * that's only waiting for the sound to play asks for one later
		 * (Wimp_PollIdle), so other tasks get the machine meanwhile. */
		if (!busy && wake) {
			unsigned now;
			_swix(OS_ReadMonotonicTime, _OUT(0), &now);
			if ((int)(wake - now) <= 0)
				wake = now + 1;
			e = _swix(Wimp_PollIdle, _INR(0, 2) | _OUT(0), 0, block, wake, &event);
		} else {
			e = _swix(Wimp_Poll, _INR(0, 1) | _OUT(0), busy ? 0 : 1, block, &event);
		}
		if (e) {
			tbx_report_error(e);
			continue;
		}
		/* A check on how often Folio is polled: more than 200 events in a
		 * second (other than null events) means something keeps it busy,
		 * e.g. a window being opened over and over. */
		{
			static unsigned t0, n_ev, n_open, n_redraw;
			unsigned now;
			_swix(OS_ReadMonotonicTime, _OUT(0), &now);
			if (event != Wimp_ENull) {
				n_ev++;
				n_open += event == Wimp_EOpenWindow;
				n_redraw += event == Wimp_ERedrawWindow;
			}
			if (now - t0 >= 100) {
				if (n_ev > 200)
					app_log("events: %u in %u cs (%u open requests, %u redraws)", n_ev, now - t0,
						n_open, n_redraw);
				t0 = now;
				n_ev = n_open = n_redraw = 0;
			}
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
			else if (docinfo_owns(block[0]))
				docinfo_redraw(block);
			else if (combine_owns(block[0]))
				combine_redraw(block);
			else if (thumbs_from_wimp(block[0]))
				thumbs_redraw(thumbs_from_wimp(block[0]), block);
			else if (toolbar_tip_owns(block[0]))
				toolbar_tip_redraw(block);
			else if (toolbar_scroller_owns(block[0]))
				toolbar_scroller_redraw(block);
			else if (bookshelf_owns(block[0]))
				bookshelf_redraw(block);
			break;
		}
		case Wimp_EOpenWindow:
			if (formui_owns(block[0]))
				_swix(Wimp_OpenWindow, _IN(1), block);
			else if (docwin_from_wimp(block[0]))
				docwin_open_request(docwin_from_wimp(block[0]), block);
			else if (dict_owns(block[0]))
				_swix(Wimp_OpenWindow, _IN(1), block);
			else if (thumbs_from_wimp(block[0]))
				thumbs_open_request(thumbs_from_wimp(block[0]), block);
			else if (combine_owns(block[0]))
				combine_open_request(block);
			else if (bookshelf_owns(block[0]))
				bookshelf_open_request(block);
			break;
		case Wimp_ECloseWindow: {
			docwin *w = docwin_from_wimp(block[0]);
			if (w)
				docwin_close_request(w);
			else if (bookshelf_owns(block[0]))
				bookshelf_close(block[0]);
			break;
		}
		case Wimp_ELoseCaret:
			formui_lose_caret(block[0]);
			break;
		case Wimp_EMenuSelection:
			formui_menu_selection(block);
			break;
		case Wimp_EUserDrag:
			/* A selection drag's button was let go. */
			docwin_user_drag_done();
			break;
		case Wimp_EScrollRequest:
			/* The mouse wheel over a toolbar. */
			toolbar_scroll_request(block);
			break;
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
			else if (combine_owns(block[3]))
				combine_click(block);
			else if (thumbs_from_wimp(block[3]))
				thumbs_click(thumbs_from_wimp(block[3]), block);
			else if (bookshelf_owns(block[3]))
				bookshelf_click(block);
			else if (toolbar_scroller_owns(block[3]))
				toolbar_scroller_click(block);
			else if ((w = docwin_from_toolbar(app.idb.self_id)) != NULL)
				docwin_toolbar_click(w, app.idb.self_component, block);
			break;
		}
		case Wimp_EKeyPressed: {
			/* Return or Escape in the toolbar's page field; other keys the
			 * window's shortcuts didn't take go on to other tasks. */
			docwin *w = docwin_from_toolbar(app.idb.self_id);
			if (formui_key(block))
				break;
			if (bookshelf_key(block))
				break;
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
		busy |= bookshelf_background();
		busy |= print_background();
		busy |= speech_background(&wake);
	}
	print_quit();
	speech_quit();
	dict_quit();
	bookshelf_quit();
	app_log("quit");
	return 0;
}
