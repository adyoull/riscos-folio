/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio's Wimp front end: shared state. */
#ifndef APP_H
#define APP_H

#include "tbx.h"
#include "folio.h"
#include <stdio.h>

#define FOLIO_VERSION_NUM "0.0.1"
#define FOLIO_VERSION FOLIO_VERSION_NUM "-test5 (10-Oct-2026)"

/* Client Toolbox event codes (0x1-0xffff are the client's). */
enum {
	EV_QUIT = 0x100, EV_HELP,
	EV_DARK = 0x110, EV_SEPIA, EV_ZOOM_IN, EV_ZOOM_OUT, EV_ZOOM_100, EV_FIT_WIDTH,
	EV_NEXT = 0x120, EV_PREV, EV_FIRST, EV_LAST, EV_LINE_DOWN, EV_LINE_UP,
	EV_CLOSE = 0x130,
	EV_VIEW_SHOWN = 0x140, EV_DOCMENU_SHOWN, EV_EDIT_SHOWN,
	EV_COPY = 0x150, EV_SELECT_ALL, EV_CLEAR_SEL, EV_DEFINE,
	EV_DICT_COPY = 0x200   /* the pop-up's, not a window's */
};

/* View menu entries (component ids). */
enum { EDIT_COPY = 1, EDIT_SELECT_ALL, EDIT_CLEAR };
/* Document menu entries. */
enum { DOC_DEFINE = 4 };
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
int clip_message(int *block);

#endif
