/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* tbx: a thin layer over the RISC OS Toolbox and the Wimp calls Folio uses.
 *
 * Objects are created from templates built in memory (Toolbox_CreateObject
 * with flag bit 0), with real pointers instead of relocated references, so
 * no Res file objects are needed; the Res file holds only the header the
 * Toolbox wants to find. Attached objects (menus, submenus, the Info box)
 * are joined up at run time with the object ids (Iconbar_SetMenu,
 * Window_SetMenu, Menu_SetSubMenuShow).
 *
 * Formats and SWI numbers: User Interface Toolbox manual (PRMs/ToolBox.pdf):
 * Toolbox_Initialise p32, CreateObject p19, ShowObject p21, the id block
 * p13, Toolbox events p12; Iconbar templates p167, Menu p203, Prog Info
 * p242, Window p329-332. */
#ifndef TBX_H
#define TBX_H

#include <kernel.h>
#include <stdint.h>

typedef unsigned int ObjectId;
typedef int ComponentId;

/* ---- SWIs not in swis.h ------------------------------------------------ */
#define Toolbox_CreateObject       0x44ec0
#define Toolbox_DeleteObject       0x44ec1
#define Toolbox_ShowObject         0x44ec3
#define Toolbox_HideObject         0x44ec4
#define Toolbox_GetObjectState     0x44ec5
#define Toolbox_ObjectMiscOp       0x44ec6
#define Toolbox_SetClientHandle    0x44ec7
#define Toolbox_GetClientHandle    0x44ec8
#define Toolbox_Initialise         0x44ecf

/* ---- Classes (= the module's SWI base) and their template versions ---- */
#define Class_Window    0x82880
#define Class_Menu      0x828c0
#define Class_Iconbar   0x82900
#define Class_ProgInfo  0x82b40

/* ---- Toolbox events ------------------------------------------------------ */
#define Toolbox_Error              0x44ec0
#define Window_AboutToBeShown      0x82880
#define Window_HasBeenHidden       0x82890
#define Menu_AboutToBeShown        0x828c0
#define Menu_HasBeenHidden         0x828c1
#define Menu_Selection             0x828c3
#define Iconbar_Clicked            0x82900

/* ---- Wimp event codes ---------------------------------------------------- */
enum {
	Wimp_ENull = 0, Wimp_ERedrawWindow = 1, Wimp_EOpenWindow = 2,
	Wimp_ECloseWindow = 3, Wimp_EPointerLeaving = 4, Wimp_EPointerEntering = 5,
	Wimp_EMouseClick = 6, Wimp_EUserDrag = 7, Wimp_EKeyPressed = 8,
	Wimp_EMenuSelection = 9, Wimp_EScrollRequest = 10, Wimp_ELoseCaret = 11,
	Wimp_EGainCaret = 12, Wimp_EPollWordNonZero = 13, Wimp_EUserMessage = 17,
	Wimp_EUserMessageRecorded = 18, Wimp_EUserMessageAcknowledge = 19,
	Wimp_EToolboxEvent = 0x200
};

/* ---- Wimp messages ------------------------------------------------------- */
enum {
	Message_Quit = 0, Message_DataSave = 1, Message_DataSaveAck = 2,
	Message_DataLoad = 3, Message_DataLoadAck = 4, Message_DataOpen = 5,
	Message_PreQuit = 8, Message_ModeChange = 0x400c1
};

/* ---- Templates ----------------------------------------------------------- */
typedef struct {
	int class_id;
	int flags;
	int version;
	char name[12];
	int total_size;
	void *body;        /* real pointer when created from memory */
	int body_size;
} tbx_header;

typedef struct {
	int flags;
	int position;
	int priority;
	const char *sprite_name;
	int max_sprite_name;
	const char *text;
	int max_text_len;
	const char *menu;
	int select_event;
	int adjust_event;
	const char *select_show;
	const char *adjust_show;
	const char *help_message;
	int max_help;
} tbx_iconbar;

typedef struct {
	int flags;
	ComponentId component_id;
	const char *text;
	int max_text;
	const char *click_show;
	const char *submenu_show;
	int submenu_event;
	int click_event;
	const char *help_message;
	int max_entry_help;
} tbx_menu_entry;

/* Menu entry flags (Toolbox p171). */
#define MENU_TICKED     (1 << 0)
#define MENU_DOTTED     (1 << 1)
#define MENU_FADED      (1 << 8)
#define MENU_SUBMENU    (1 << 10)

typedef struct {
	int flags;
	const char *title;
	int max_title;
	const char *help_message;
	int max_help;
	int show_event;
	int hide_event;
	int num_entries;
	/* followed by num_entries tbx_menu_entry */
} tbx_menu;

typedef struct {
	int flags;
	const char *title;
	int max_title;
	const char *purpose;
	const char *author;
	int licence_type;
	const char *version;
	const char *window;
	const char *uri;
	int event;
} tbx_proginfo;

typedef struct {
	int flags;
	int wimp_key_code;
	int key_event;
	const char *key_show;
} tbx_shortcut;

typedef struct {
	int vis_x0, vis_y0, vis_x1, vis_y1;
	int scroll_x, scroll_y;
	int behind;
	unsigned int window_flags;
	unsigned char title_fore, title_back, work_fore, work_back;
	unsigned char scroll_outer, scroll_inner, title_focus, filler;
	int work_x0, work_y0, work_x1, work_y1;
	unsigned int title_flags;
	unsigned int work_flags;
	void *sprite_area;
	short min_width, min_height;
	char *title_text;
	const char *title_validation;
	int title_buflen;
	int num_icons;
} tbx_wimp_window;

typedef struct {
	int flags;
	const char *help_message;
	int max_help;
	const char *pointer_shape;
	int max_pointer_shape;
	int pointer_x_hot, pointer_y_hot;
	const char *menu;
	int num_shortcuts;
	tbx_shortcut *shortcuts;
	int num_gadgets;
	void *gadgets;
	ComponentId default_focus;
	int show_event, hide_event;
	const char *internal_bl, *internal_tl, *external_bl, *external_tl;
	tbx_wimp_window window;
} tbx_window;

/* The id block filled in on every Wimp_Poll (Toolbox p32). */
typedef struct {
	ObjectId ancestor_id;
	ComponentId ancestor_component;
	ObjectId parent_id;
	ComponentId parent_component;
	ObjectId self_id;
	ComponentId self_component;
} tbx_idblock;

/* ---- Calls --------------------------------------------------------------- */

/* Starts the task. dir is the resources directory ("<Folio$Dir>").
 * messages: Wimp message numbers wanted, 0-terminated. */
_kernel_oserror *tbx_init(const char *dir, const int *messages, tbx_idblock *idb,
		int *task, void **sprite_area);
/* Creates an object from an in-memory template. */
_kernel_oserror *tbx_create(int class_id, int version, int flags, void *body,
		int body_size, ObjectId *id);
/* flags bit 0: don't delete attached objects (shared menus stay). */
_kernel_oserror *tbx_delete(ObjectId id, int flags);
_kernel_oserror *tbx_show(ObjectId id, int flags, int type, const void *data,
		ObjectId parent, ComponentId parent_component);
_kernel_oserror *tbx_hide(ObjectId id);
_kernel_oserror *tbx_set_handle(ObjectId id, void *handle);
_kernel_oserror *tbx_get_handle(ObjectId id, void **handle);
/* Toolbox_ObjectMiscOp with up to 3 more arguments (R3-R5). */
_kernel_oserror *tbx_method(ObjectId id, int method, int r3, int r4, int r5);
_kernel_oserror *tbx_method_out(ObjectId id, int method, int r3, int *r0_out);

/* Wrappers of common methods. */
#define menu_set_tick(m, c, on)       tbx_method((m), 0, (c), (on), 0)
#define menu_set_fade(m, c, on)       tbx_method((m), 2, (c), (on), 0)
#define menu_set_submenu(m, c, sub)   tbx_method((m), 8, (c), (int)(sub), 0)
#define menu_set_click_show(m, c, o, transient) tbx_method((m), 12, (c), (int)(o), (transient))
#define iconbar_set_menu(i, m)        tbx_method((i), 1, (int)(m), 0, 0)
#define window_set_menu(w, m)         tbx_method((w), 3, (int)(m), 0, 0)
#define window_set_title(w, t)        tbx_method((w), 11, (int)(t), 0, 0)
#define proginfo_set_version(p, v)    tbx_method((p), 1, (int)(v), 0, 0)
_kernel_oserror *window_get_wimp_handle(ObjectId w, int *handle);

/* Wimp error box (OK button). */
void tbx_report(const char *message);
void tbx_report_error(const _kernel_oserror *e);

#endif
