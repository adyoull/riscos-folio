/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio's Wimp front end: shared state. */
#ifndef APP_H
#define APP_H

#include "tbx.h"
#include "folio.h"
#include <stdio.h>

#define FOLIO_VERSION_NUM "0.0.1"
#define FOLIO_VERSION FOLIO_VERSION_NUM "-test53 (20-Oct-2026)"

/* Client Toolbox event codes (0x1-0xffff are the client's). */
enum {
	EV_QUIT = 0x100, EV_HELP, EV_BOOKSHELF,
	EV_DARK = 0x110, EV_SEPIA, EV_ZOOM_IN, EV_ZOOM_OUT, EV_ZOOM_100, EV_FIT_WIDTH,
	EV_TOOLBAR, EV_THUMBS, EV_TWO_PAGES, EV_COVER_ALONE, EV_DEFAULT_ZOOM,
	EV_NEXT = 0x120, EV_PREV, EV_FIRST, EV_LAST, EV_LINE_DOWN, EV_LINE_UP, EV_BACK,
	EV_CLOSE = 0x130,
	EV_VIEW_SHOWN = 0x140, EV_DOCMENU_SHOWN, EV_EDIT_SHOWN, EV_NAV_SHOWN,
	EV_COPY = 0x150, EV_SELECT_ALL, EV_CLEAR_SEL, EV_DEFINE, EV_FIND, EV_CONTENTS,
	EV_HIGHLIGHT = 0x160, EV_UNDERLINE, EV_STRIKE, EV_NOTE, EV_UNDO, EV_REDO, EV_DEL_ANNOT, EV_SAVE,
	EV_ADD_SHELF, EV_PASSWORD, EV_READ_ALOUD, EV_SPEAK_SEL, EV_PRINT, EV_EXPORT_DRAW,
	EV_OCR_PAGE = 0x170, EV_OCR_ALL, EV_GOTO, EV_DOCINFO,
	EV_DICT_COPY = 0x200,  /* the pop-up's, not a window's */
	EV_FIND_NEXT = 0x210, EV_FIND_PREV, EV_FIND_CANCEL,
	EV_NOTE_OK = 0x220, EV_NOTE_DELETE, EV_NOTE_CANCEL,
	EV_SH_OPEN = 0x230, EV_SH_REMOVE, EV_SH_FILER, EV_SH_RECENT, EV_SH_TITLE, EV_SH_AUTHOR,
	EV_SH_FOLDERS, EV_SH_REFRESH, EV_SH_MISSING,
	EV_PWOPEN_OK = 0x240, EV_PWOPEN_CANCEL, EV_PWSET, EV_PWREMOVE, EV_PWCANCEL,
	EV_RESTRICT_OK, EV_RESTRICT_CANCEL,
	EV_SPEED_SLOW = 0x250, EV_SPEED_NORMAL, EV_SPEED_FAST, EV_SPEED_SHOWN,
	EV_OCR_AUTO = 0x260, EV_OCR_ACCURATE, EV_OCR_FAST, EV_OCR_SHOWN,
	EV_GOTO_OK = 0x270, EV_GOTO_CANCEL,
	EV_CHOICES = 0x280, EV_CHOICES_DEFAULT, EV_CHOICES_CANCEL, EV_CHOICES_SAVE,
	EV_ASK_OK = 0x290, EV_ASK_CANCEL,
	EV_FILL_FORM = 0x2a0,   /* not a menu entry: filling in a form (docwin_allows) */
	EV_FULL_SCREEN = 0x2b0, EV_SAVE_PAGES, EV_SAVE_PICTURES,
	EV_PAGES_OK = 0x2c0, EV_PAGES_CANCEL,
	EV_CREATE_PDF = 0x2d0, EV_CP_UP, EV_CP_DOWN, EV_CP_REMOVE, EV_CP_SORT, EV_CP_CLEAR, EV_CP_CREATE,
	EV_SCAN = 0x2e0, EV_SC_FIND, EV_SC_SCAN, EV_SC_STOP
};

/* View menu entries (component ids). */
enum { EDIT_COPY = 1, EDIT_SELECT_ALL, EDIT_CLEAR, EDIT_UNDO, EDIT_REDO, EDIT_HILITE,
	EDIT_ULINE, EDIT_STRIKE, EDIT_NOTE, EDIT_DELANNOT };
/* Document menu entries. */
enum { DOC_FIND = 5, DOC_DEFINE = 4, DOC_SAVE = 6, DOC_SHELF = 7, DOC_PASSWORD = 8,
	DOC_READ = 9, DOC_SPEAKSEL = 10, DOC_SPEED = 11, DOC_PRINT = 12, DOC_EXPORT_DRAW = 13,
	DOC_OCR = 14, DOC_INFO = 15, DOC_SAVE_PAGES = 16, DOC_SAVE_PICTURES = 17 };
/* Text recognition menu entries. */
enum { OCR_PAGE = 1, OCR_ALL, OCR_AUTO, OCR_ACCURATE, OCR_FAST };
enum { SPEED_SLOW = 1, SPEED_NORMAL, SPEED_FAST };
/* Navigate menu entries. */
enum { NAV_CONTENTS = 5, NAV_BACK = 6, NAV_GOTO = 7 };
enum { VIEW_DARK = 1, VIEW_SEPIA, VIEW_ZOOM_IN, VIEW_ZOOM_OUT, VIEW_ZOOM_100, VIEW_FIT_WIDTH,
	VIEW_TOOLBAR, VIEW_THUMBS, VIEW_TWO_PAGES, VIEW_COVER_ALONE, VIEW_DEFAULT_ZOOM, VIEW_FULL_SCREEN };

/* The Choices (choices.c; <Choices$Write>.Folio.Choices). */
typedef struct {
	float zoom;                 /* new documents open at this scale */
	folio_page_mode mode;       /* ... with this page colour */
	int cols;                   /* ... one or two pages across */
	int toolbar;                /* ... with the toolbar */
	int speed;                  /* reading aloud: SPEED_* */
	int ocr_fast;               /* text recognition: Fast rather than Accurate */
	int ocr_auto;               /* recognise scanned pages while reading */
	int obey_restrictions;      /* a PDF's "no printing / no copying" */
	int ask_links;              /* ask before following links out of a document */
	int underline_links;        /* draw a line under links */
	int warn_content;           /* say when a PDF has scripts, attachments, actions */
	int run_js;                 /* let forms' JavaScript run */
} choices;

typedef struct {
	tbx_idblock idb;
	folio_ctx *fc;
	ObjectId iconbar, iconbar_menu, proginfo;
	ObjectId doc_menu, edit_menu, view_menu, nav_menu, speed_menu;
	int task;                   /* our Wimp task handle */
	ObjectId menu_window;       /* the window the document menu was opened from */
	int xeig, yeig, log2bpp;
	int screen_w, screen_h;     /* OS units */
	int epub_type;              /* RISC OS filetype for EPUB, or -1 */
	FILE *log;
	void *sprites;              /* <Folio$Dir>.Sprites, loaded by the Toolbox (1 = none) */
	choices ch;                 /* the Choices (choices.c) */
	ObjectId ocr_menu;
	int speech_urgent;          /* reading aloud is close to running out: background
	                             * work (drawing pages ahead, thumbnails, measuring,
	                             * text recognition) waits, as the worker thread
	                             * shares the machine with the voice */
} app_state;

extern app_state app;

void app_log(const char *fmt, ...);
void app_read_mode(void);

/* docwin.c */
typedef struct docwin docwin;
docwin *docwin_open(const char *path);
void docwin_open_page(docwin *w, int page);   /* 0-based */
/* The same with a password (UTF-8; NULL for none). If the document needs
 * one, the password box asks for it and opens it (returning NULL now). */
docwin *docwin_open_password(const char *path, const char *password);
int docwin_get_security(docwin *w, folio_security *s);
int docwin_check_owner(docwin *w, const char *utf8);     /* 1, 0 or -1 */
int docwin_set_password(docwin *w, const char *utf8);    /* NULL: remove; 0 or -1 */
void docwin_close(docwin *w);
void docwin_close_all(void);   /* on quit: saves reading positions */
docwin *docwin_from_object(ObjectId id);
docwin *docwin_from_wimp(int handle);
ObjectId docwin_object(docwin *w);
void docwin_redraw(docwin *w, int *block);
void docwin_event(docwin *w, int code);
void docwin_view_menu_shown(docwin *w);
/* Choices: the default scale (<Choices$Write>.Folio.Choices). */
void docwin_edit_menu_shown(docwin *w);
void docwin_nav_menu_shown(docwin *w);
/* Background work for every window; returns 1 if more remains. */
int docwin_background(void);
void docwin_user_drag_done(void);
void docwin_mode_changed(void);
void docwin_close_request(docwin *w);
void docwin_click(docwin *w, int *block);

void docwin_doc_menu_shown(docwin *w);
/* For the Contents and Find windows (go_to and show_hit: not with the core lock held). */
folio_doc *docwin_doc(docwin *w);
const char *docwin_title(docwin *w);
/* Text after the title (e.g. " (printing page 2 of 9)"); "" for none. */
void docwin_set_note(docwin *w, const char *note);
int docwin_wimp(docwin *w);
int docwin_current_page(docwin *w);
int docwin_is_counting(docwin *w);
void docwin_go_to(docwin *w, int page);
/* Selects a found hit (quads on page) and scrolls it into view. */
void docwin_show_hit(docwin *w, int page, const folio_quad *q, int n);

docwin *docwin_from_toolbar(ObjectId id);
int docwin_pages(docwin *w);
/* A document Folio made (a directory of pictures as a PDF), in the scrap
 * directory: shown as unsaved until it's saved somewhere. */
void docwin_mark_new(docwin *w);
void docwin_ocr_all(docwin *w);       /* a new scan: recognise every page */
int docwin_reading_dir(docwin *w);
void docwin_page_pt(docwin *w, int page, float *pw, float *ph);   /* size in points */        /* +1 forwards, -1 backwards */
void docwin_text_changed(docwin *w, int page);   /* recognised */
int docwin_ocr_wait(docwin *w, int page);  /* reading aloud: wait for OCR */
void docwin_ocr_menu_shown(docwin *w);   /* w may be NULL */
void docwin_ocr_settings_changed(void);
void docwin_refresh_toolbar(docwin *w);
int docwin_read_start(docwin *w, int *page, folio_point *a, int *end_page, folio_point *b);
void docwin_set_spoken(docwin *w, const int *pages, const folio_quad *q, int n);
const char *docwin_path(docwin *w);
int docwin_save(docwin *w, const char *path);            /* 0 if saved */
void docwin_note_done(docwin *w, int page, int index, folio_point at, const char *utf8);
void docwin_delete_annot(docwin *w, int page, int index);
docwin *docwin_first_modified(void);
void docwin_toolbar_click(docwin *w, int component, int *block);
int docwin_toolbar_key(docwin *w, int key);   /* 1 if taken */

/* toolbar.c */
int toolbar_height(void);
int toolbar_width(void);           /* all the buttons, side by side */
int toolbar_scroller_owns(int wimp_handle);
void toolbar_scroller_click(int *block);
void toolbar_scroller_redraw(int *block);
int toolbar_scroll_request(int *block);   /* 1 if it was a toolbar's */
ObjectId toolbar_create(docwin *w);
void toolbar_attach(ObjectId win, ObjectId tb, int on);
void toolbar_fit(ObjectId tb, int parent_wimp);
int toolbar_open_with(ObjectId tb, int *open_block);   /* pane first; 0 if no pane */
void toolbar_follow(ObjectId tb, int pane, int parent_wimp);
void docwin_open_request(docwin *w, int *block);
void toolbar_update(ObjectId tb, int page, int pages, int exact, float zoom,
		int dark, int thumbs, int two, int has_selection, int can_annotate, int speaking);
int toolbar_event_for(int component);
void toolbar_forget(ObjectId tb);   /* it's being deleted */
int toolbar_typed_page(ObjectId tb);
int toolbar_is_page_field(int component);
void toolbar_pointer(int wimp_handle, int entering);   /* Pointer_Entering/Leaving */
int toolbar_background(void);                          /* 1 while tracking for a tooltip */
int toolbar_tip_owns(int wimp_handle);
void toolbar_tip_redraw(int *block);
void toolbar_tip_hide(void);

/* thumbs.c: the thumbnail drawer */
typedef struct thumbs thumbs;
thumbs *thumbs_new(docwin *w);
void thumbs_free(thumbs *t);
thumbs *thumbs_from_wimp(int handle);
int thumbs_width(void);
int thumbs_shown(thumbs *t);
void thumbs_show(thumbs *t, int on, int top);
void thumbs_reopen(thumbs *t, int top);
void thumbs_layout(thumbs *t, float page_w, float page_h, int pages, folio_page_mode mode);
void thumbs_set_current(thumbs *t, int page, int scroll);
int thumbs_wanted(thumbs *t, int busy_page, int *px_w, int *px_h, float *ppp, int *gen);
void thumbs_take(thumbs *t, int page, int gen, int *area, int ok);
void thumbs_forget(thumbs *t, int page);   /* -1: every page */
void thumbs_redraw(thumbs *t, int *block);
void thumbs_click(thumbs *t, int *block);
void thumbs_open_request(thumbs *t, int *block);

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

/* docinfo.c: Document info */
void docinfo_open(docwin *w);
void docinfo_forget(docwin *w);
int docinfo_owns(int wimp_handle);
void docinfo_redraw(int *block);

/* gotopage.c: Go to page (F5) */
void gotopage_open(docwin *w);
void gotopage_forget(docwin *w);
void gotopage_event(int code);
void docwin_jump(docwin *w, int page);   /* remembered for Back */
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
/* The desktop's alphabet to UTF-8; malloc'd. */
char *app_utf8_text(const char *desktop);

/* passui.c: the password boxes */
void passui_ask_open(const char *path, int wrong);
void passui_security(docwin *w);
int passui_event(int code);              /* 1 if it was theirs */
/* "The author doesn't allow <what>": the owner password lifts it, and then
 * event is sent to w again. */
void passui_restricted(docwin *w, const char *what, int event);

/* print.c: the Print dialogue and printing. */
/* export.c: Export page as Draw... */
void export_open(docwin *w);
int export_event(int *block);            /* 1 if it was its SaveAs box's */
/* pagesave.c: Save pages as PDF... / Save page pictures... (pictures = 1);
 * a directory of pictures dropped on Folio made into a new PDF (returns 1
 * if it dealt with it). */
void pagesave_open(docwin *w, int pictures);
void pagesave_event(int code);
int pagesave_saveas_event(int *block);
void pagesave_forget(docwin *w);
int pagesave_folder(const char *dir);
/* Shared with combine.c: the pictures (and PDFs) in a directory, in name
 * order (Page2 before Page10); a new file in the scrap directory; which
 * files are pictures or PDFs (type -1: none, by the name's ending). */
int pagesave_list(const char *dir, int pdfs, char ***out);
void pagesave_free_list(char **names, int n);
void pagesave_new_path(const char *leaf, char *out, size_t size);
int pagesave_is_pdf(int type, const char *leaf);
int pagesave_is_picture(int type, const char *leaf);
int pagesave_natural(const char *a, const char *b);
/* combine.c: the Create PDF window (icon bar menu): files dragged into it,
 * in order, joined into one new PDF. */
/* scanui.c: Scan... (icon bar menu): scanning from a network scanner
 * (eSCL), moved on a step at a time from the poll loop. */
void scanui_open(void);
void scanui_event(int code);
int scanui_background(unsigned *wake);
void combine_open(void);
void combine_open_request(int *block);
int combine_owns(int wimp);
void combine_redraw(int *block);
void combine_click(int *block);
int combine_data_load(int *block);       /* 1 if it was dropped on it */
void combine_event(int code);
void export_forget(docwin *w);

void print_open(docwin *w);
int print_event(int *block);             /* 1 if it was the Print dialogue's */
void print_forget(docwin *w);
int print_background(void);              /* 1 while a job is running */
void print_quit(void);
int print_busy_for(docwin *w);           /* printing from this window */

/* ocrui.c: text recognition (the OCR plugin). */
typedef struct ocr_win ocr_win;
int ocr_plugin_init(void);                      /* finds the plugin: 1 if there */
int ocr_available(void);
int ocr_has_accurate(void);
int ocr_has_fast(void);
ocr_win *ocr_win_new(docwin *w);         /* core lock held; NULL without OCR */
void ocr_win_close(ocr_win *o);          /* core lock held: saves its text */
void ocr_win_before_reopen(ocr_win *o);  /* around saving the PDF (lock held) */
void ocr_win_after_reopen(ocr_win *o);
int ocr_win_next(ocr_win *o);            /* worker idle: gives it a page; 1 if so */
int ocr_win_running(ocr_win *o);
int ocr_win_wait(ocr_win *o, int page);  /* reading aloud: 1 = wait for page */
void ocr_win_this_page(ocr_win *o);
void ocr_win_all(ocr_win *o);            /* on or off */
int ocr_win_all_on(ocr_win *o);
void ocr_settings_changed(ocr_win *o);
void passui_forget(docwin *w);

/* speech.c: reading aloud (with the Speech plugin) */
int speech_init(void);                   /* finds the plugin: 1 if there */
int speech_available(void);
void speech_read(docwin *w, int selection_only);
void speech_stop(void);
docwin *speech_window(void);             /* the window being read, or NULL */
void speech_forget(docwin *w);           /* w is closing */
/* 1 while it needs null events now (the voice being made); else 0, with
 * *wake set to when it wants one (0: never) while the sound plays. */
int speech_background(unsigned *wake);
void speech_set_speed(int speed);        /* SPEED_* */
int speech_speed(void);
void speech_quit(void);

/* bookshelf.c */
#include "worker.h"
void bookshelf_init(void);
void bookshelf_quit(void);
void bookshelf_open(void);
int bookshelf_owns(int wimp_handle);
void bookshelf_redraw(int *block);
void bookshelf_click(int *block);
int bookshelf_key(int *block);
void bookshelf_open_request(int *block);
void bookshelf_close(int wimp_handle);
void bookshelf_event(int code);
int bookshelf_data_load(int *block);      /* 1 if it was for the bookshelf */
int bookshelf_background(void);
int bookshelf_owns_job(const render_job *r);
void bookshelf_take(render_job *r);
int bookshelf_has(const char *path);
void bookshelf_toggle(const char *path);
void bookshelf_note(const char *path, int page, int pages, const char *title, const char *author);
void ocr_win_take(ocr_win *o, const render_job *r);   /* a JOB_OCR result */

/* annotui.c: the Note, Save and Discard/Cancel/Save boxes */
void note_open(docwin *w, int page, int index, folio_point at, const char *text);
void save_open(docwin *w, int close_after);
void dcs_open(docwin *w);
int annotui_event(int *block);           /* 1 if it was theirs */
void annotui_forget(docwin *w);
int clip_message(int *block);

/* choices.c */
void choices_defaults(choices *c);
void choices_load(void);
void choices_save(void);
void choices_apply(const choices *c);   /* use these now */
void choices_open(void);
void choices_event(int code);

/* fontsetup.c: which fontconfig setup (before folio_new). */
void fontsetup(void);

/* ask.c: a question or notice that doesn't stop the desktop. */
typedef void ask_done_fn(void *data, int ok);
void ask_show(const char *title, const char *desktop_text, const char *ok, const char *cancel,
		ask_done_fn *done, void *data);
void ask_event(int code);
void ask_forget(void *data);

/* formui.c: filling in PDF forms. */
int formui_click(docwin *w, int page, folio_point p, int x, int y);
int formui_key(int *block);
int formui_owns(int handle);
void formui_lose_caret(int handle);
int formui_menu_selection(int *block);
void formui_menus_deleted(int tree);
void formui_commit(docwin *w);
void formui_raise(docwin *w);
int formui_on_field(docwin *w, int page, folio_point p);
void formui_forget(docwin *w);

/* docwin.c, for formui.c and security. */
int is_docwin(docwin *w);
float docwin_zoom(docwin *w);
void docwin_page_origin(docwin *w, int page, int *sx, int *sy);   /* screen, top left */
int docwin_form_set_text(docwin *w, int page, int index, const char *utf8);
int docwin_form_toggle(docwin *w, int page, int index);
int docwin_form_choose(docwin *w, int page, int index, int option);
int docwin_form_press(docwin *w, int page, int index);
void docwin_ask_permission(docwin *w, int event);
int docwin_allows(docwin *w, int event);
/* Follows a link out of the document (a web page, email or file), asking
 * first if Choices say so (always for one a script asks for). */
void docwin_follow_uri(docwin *w, const char *uri, int from_script);

#endif
