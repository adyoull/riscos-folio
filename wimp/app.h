/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio's Wimp front end: shared state. */
#ifndef APP_H
#define APP_H

#include "tbx.h"
#include "folio.h"
#include <stdio.h>

#define FOLIO_VERSION_NUM "0.0.1"
#define FOLIO_VERSION FOLIO_VERSION_NUM "-test7 (10-Oct-2026)"

/* Client Toolbox event codes (0x1-0xffff are the client's). */
enum {
	EV_QUIT = 0x100, EV_HELP,
	EV_DARK = 0x110, EV_SEPIA, EV_ZOOM_IN, EV_ZOOM_OUT, EV_ZOOM_100, EV_FIT_WIDTH,
	EV_NEXT = 0x120, EV_PREV, EV_FIRST, EV_LAST, EV_LINE_DOWN, EV_LINE_UP,
	EV_CLOSE = 0x130,
	EV_VIEW_SHOWN = 0x140, EV_DOCMENU_SHOWN, EV_EDIT_SHOWN,
	EV_COPY = 0x150, EV_SELECT_ALL, EV_CLEAR_SEL, EV_DEFINE, EV_FIND, EV_CONTENTS,
	EV_DICT_COPY = 0x200,  /* the pop-up's, not a window's */
	EV_FIND_NEXT = 0x210, EV_FIND_PREV, EV_FIND_CANCEL
};

/* View menu entries (component ids). */
enum { EDIT_COPY = 1, EDIT_SELECT_ALL, EDIT_CLEAR };
/* Document menu entries. */
enum { DOC_FIND = 5, DOC_DEFINE = 4 };
/* Navigate menu entries. */
enum { NAV_CONTENTS = 5 };
enum { VIEW_DARK = 1, VIEW_SEPIA, VIEW_ZOOM_IN, VIEW_ZOOM_OUT, VIEW_ZOOM_100, VIEW_FIT_WIDTH };

typedef struct {
	tbx_idblock idb;
	folio_ctx *fc;
	ObjectId iconbar, iconbar_menu, proginfo;
	ObjectId doc_menu, edit_menu, view_menu, nav_menu;
	int task;                   /* our Wimp task handle */
	ObjectId menu_window;       /* the window the document menu was opened from */
	int xeig, yeig, log2bpp;
	int screen_w, screen_h;     /* OS units */
	int epub_type;              /* RISC OS filetype for EPUB, or -1 */
	FILE *log;
} app_state;

extern app_state app;

void app_log(const char *fmt, ...);
void app_read_mode(void);

/* docwin.c */
typedef struct docwin docwin;
docwin *docwin_open(const char *path);
void docwin_close(docwin *w);
void docwin_close_all(void);   /* on quit: saves reading positions */
docwin *docwin_from_object(ObjectId id);
docwin *docwin_from_wimp(int handle);
ObjectId docwin_object(docwin *w);
void docwin_redraw(docwin *w, int *block);
void docwin_event(docwin *w, int code);
void docwin_view_menu_shown(docwin *w);
void docwin_edit_menu_shown(docwin *w);
/* Background work for every window; returns 1 if more remains. */
int docwin_background(void);
void docwin_mode_changed(void);
void docwin_close_request(docwin *w);
void docwin_click(docwin *w, int *block);

void docwin_doc_menu_shown(docwin *w);
/* For the Contents and Find windows (go_to and show_hit: not with the core lock held). */
folio_doc *docwin_doc(docwin *w);
const char *docwin_title(docwin *w);
int docwin_wimp(docwin *w);
int docwin_current_page(docwin *w);
int docwin_is_counting(docwin *w);
void docwin_go_to(docwin *w, int page);
/* Selects a found hit (quads on page) and scrolls it into view. */
void docwin_show_hit(docwin *w, int page, const folio_quad *q, int n);

/* contents.c */
void contents_open(docwin *w);
void contents_forget(docwin *w);       /* w is closing */
void contents_page_changed(docwin *w); /* also when counting ends */
int contents_owns(int wimp_handle);
void contents_redraw(int *block);
void contents_click(int *block);

/* find.c */
void find_open(docwin *w);
void find_forget(docwin *w);
void find_event(int code);
int find_background(void);             /* 1 while searching */

/* dict.c: Define "word" */
void dict_define(const char *word, const char *lang, folio_page_mode mode, int x, int y);
int dict_owns(int wimp_handle);
void dict_redraw(int *block);
void dict_click(int *block);
void dict_copy(void);
int dict_background(void);   /* 1 while a lookup is in progress */
void dict_quit(void);

/* clip.c */
void clip_set_text(const char *utf8);
/* UTF-8 to the desktop's alphabet (Latin-1 unless it's UTF-8); malloc'd. */
char *app_desktop_text(const char *utf8);
int clip_message(int *block);

#endif
