/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* The document window's toolbar: a Toolbox internal top-left toolbar
 * (User Interface Toolbox p334-336), one per window, with sprite buttons for
 * things also on the menus (Style Guide: a toolbar is a short cut, never
 * the only way), a page number to type into, and the zoom.
 *
 *   [Thumbnails][Contents] | [Up][page] of N [Down] | [-] 100% [+] [Fit] |
 *   [Dark] | [Find][Define] | [Highlight][Underline][Strike][Note]
 *
 * The annotation tools are shaded until annotations are written. Button
 * gadgets report clicks as Wimp mouse clicks with the id block's component
 * set (p366); Return in the page field arrives as a key press. */
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define TB_H     76
#define BTN      60
#define TB_W     1340

enum {
	T_THUMBS = 1, T_CONTENTS, T_PREV, T_PAGE, T_OF, T_NEXT, T_ZOOMOUT, T_ZOOM, T_ZOOMIN,
	T_FITW, T_DARK, T_FIND, T_DEFINE, T_HILITE, T_ULINE, T_STRIKE, T_NOTE, T_COUNT
};

typedef struct {
	int flags, type;
	int x0, y0, x1, y1;
	ComponentId id;
	const char *help;
	int max_help;
} gadget_hdr;

typedef struct {
	gadget_hdr h;
	unsigned button_flags;
	const char *value;
	int max_value;
	const char *validation;
	int max_validation;
} button_gadget;

typedef struct {
	gadget_hdr h;
	const char *text;
	int max_text;
	const char *allowable;
	int max_allowable;
	ComponentId before, after;
} writable_gadget;

typedef struct { gadget_hdr h; const char *text; int max_text; } display_gadget;

/* The template's gadgets, in order, packed one after another. */
static struct {
	button_gadget thumbs, contents, prev;
	writable_gadget page;
	display_gadget of;
	button_gadget next, zoomout;
	display_gadget zoom;
	button_gadget zoomin, fitw, dark, find, define, hilite, uline, strike, note;
} g;

static void hdr(gadget_hdr *h, int type, int size, int id, int x0, int x1, const char *help)
{
	h->flags = 0;
	h->type = (size << 16) | type;
	h->x0 = x0;
	h->x1 = x1;
	h->y0 = -TB_H + 8;
	h->y1 = -8;
	h->id = id;
	h->help = help;
	h->max_help = help ? (int)strlen(help) + 1 : 0;
}

/* A sprite button: text+sprite icon, no text, 3D border (R5), click type. */
static void button(button_gadget *b, int id, int x, const char *validation, const char *help)
{
	hdr(&b->h, 960, sizeof *b, id, x, x + BTN, help);
	b->button_flags = 1 | 2 | 4 | 8 | 0x10 | 0x20 | 0x100 | (3u << 12) | (7u << 24) | (1u << 28);
	b->value = "";
	b->max_value = 1;
	b->validation = validation;
	b->max_validation = (int)strlen(validation) + 1;
}

static void build_template(void)
{
	int x = 8;
	memset(&g, 0, sizeof g);
	button(&g.thumbs, T_THUMBS, x, "R5;Stb_thumbs", "Click to show or hide the page thumbnails at the left."); x += BTN + 4;
	button(&g.contents, T_CONTENTS, x, "R5;Stb_contents", "Click to see the contents list."); x += BTN + 20;
	button(&g.prev, T_PREV, x, "R5;Stb_prev", "Click to go to the previous page (Page Up)."); x += BTN + 4;
	hdr(&g.page.h, 512, sizeof g.page, T_PAGE, x, x + 104,
		"This is the page you're reading.|MType a page number and press Return to go there.");
	g.page.h.y0 += 4;
	g.page.h.y1 -= 4;
	g.page.text = "1";
	g.page.max_text = 8;
	g.page.allowable = "0-9";
	g.page.max_allowable = 4;
	g.page.before = -1;
	g.page.after = -1;
	x += 104 + 4;
	hdr(&g.of.h, 448, sizeof g.of, T_OF, x, x + 120, "This is the number of pages.");
	g.of.h.y0 += 4;
	g.of.h.y1 -= 4;
	g.of.text = "of 1";
	g.of.max_text = 16;
	x += 120 + 4;
	button(&g.next, T_NEXT, x, "R5;Stb_next", "Click to go to the next page (Page Down)."); x += BTN + 20;
	button(&g.zoomout, T_ZOOMOUT, x, "R5;Stb_zoomout", "Click to make the pages smaller."); x += BTN + 4;
	hdr(&g.zoom.h, 448, sizeof g.zoom, T_ZOOM, x, x + 112, "This is the view's scale.");
	g.zoom.h.y0 += 4;
	g.zoom.h.y1 -= 4;
	g.zoom.text = "100%";
	g.zoom.max_text = 8;
	x += 112 + 4;
	button(&g.zoomin, T_ZOOMIN, x, "R5;Stb_zoomin", "Click to make the pages bigger."); x += BTN + 4;
	button(&g.fitw, T_FITW, x, "R5;Stb_fitw", "Click to fit the pages to the window's width."); x += BTN + 20;
	button(&g.dark, T_DARK, x, "R5;Stb_dark", "Click for a black page with light text (^N); click again for white."); x += BTN + 20;
	button(&g.find, T_FIND, x, "R5;Stb_find", "Click to find text (F4)."); x += BTN + 4;
	button(&g.define, T_DEFINE, x, "R5;Stb_define", "Click to see what the selected word means."); x += BTN + 20;
	button(&g.hilite, T_HILITE, x, "R5;Stb_hilite", "Highlighting comes in a later test build."); x += BTN + 4;
	button(&g.uline, T_ULINE, x, "R5;Stb_uline", "Underlining comes in a later test build."); x += BTN + 4;
	button(&g.strike, T_STRIKE, x, "R5;Stb_strike", "Striking out comes in a later test build."); x += BTN + 4;
	button(&g.note, T_NOTE, x, "R5;Stb_note", "Notes come in a later test build.");
	/* Not yet: shaded (bit 31). */
	g.hilite.h.flags = g.uline.h.flags = g.strike.h.flags = g.note.h.flags = (int)(1u << 31);
}

int toolbar_height(void)
{
	return TB_H;
}

ObjectId toolbar_create(docwin *w)
{
	static tbx_window t;
	static int built;
	ObjectId id = 0;
	_kernel_oserror *e;
	if (!built) {
		build_template();
		built = 1;
	}
	memset(&t, 0, sizeof t);
	t.flags = 1 << 4;                       /* a toolbar */
	t.help_message = "This is the toolbar.|MYou can hide it from the View menu.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.num_gadgets = T_COUNT - 1;
	t.gadgets = &g;
	t.default_focus = -1;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = TB_W;
	t.window.vis_y1 = TB_H;
	t.window.behind = -1;
	/* No furniture; auto-redraw; new format. */
	t.window.window_flags = 0x80000000u | (1u << 4);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 1;
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -TB_H;
	t.window.work_x1 = TB_W;
	t.window.title_flags = 0;
	t.window.work_flags = 0;
	t.window.sprite_area = app.sprites;     /* the toolbar's icons: <Folio$Dir>.Sprites */
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		app_log("toolbar: %s", e->errmess);
		return 0;
	}
	tbx_set_handle(id, w);
	return id;
}

/* Shows or hides it on the document window (Window_SetToolBars, p336). */
void toolbar_attach(ObjectId win, ObjectId tb, int on)
{
	_kernel_oserror *e = _swix(Toolbox_ObjectMiscOp, _INR(0, 6), 2 /* internal tl */, win, 18,
		0, on ? tb : 0, 0, 0);
	if (e)
		app_log("toolbar: SetToolBars: %s", e->errmess);
}

static void set_text(ObjectId tb, int comp, int method, const char *s)
{
	tbx_method(tb, method, comp, (int)s, 0);
}

static void set_selected(ObjectId tb, int comp, int on)
{
	/* Button_SetFlags: clear and EOR words as Wimp_SetIconState; bit 21 = selected. */
	tbx_method(tb, 961, comp, 1 << 21, on ? 1 << 21 : 0);
}

void toolbar_update(ObjectId tb, int page, int pages, int exact, float zoom,
		int dark, int thumbs, int has_selection)
{
	char s[24];
	int caret[6], wh = 0;
	if (!tb)
		return;
	/* Not while the reader is typing a page number there. */
	_swix(Wimp_GetCaretPosition, _IN(1), caret);
	window_get_wimp_handle(tb, &wh);
	if (caret[0] != wh) {
		snprintf(s, sizeof s, "%d", page + 1);
		set_text(tb, T_PAGE, 512 /* WritableField_SetValue */, s);
	}
	snprintf(s, sizeof s, exact ? "of %d" : "of %d+", pages);
	set_text(tb, T_OF, 448 /* DisplayField_SetValue */, s);
	snprintf(s, sizeof s, "%d%%", (int)(zoom * 100 + 0.5f));
	set_text(tb, T_ZOOM, 448, s);
	set_selected(tb, T_DARK, dark);
	set_selected(tb, T_THUMBS, thumbs);
	tbx_method(tb, 65 /* Gadget_SetFlags */, T_DEFINE, has_selection ? 0 : (int)(1u << 31), 0);
}

/* A click on a toolbar button: the matching document window event. */
int toolbar_event_for(int component)
{
	switch (component) {
	case T_THUMBS: return EV_THUMBS;
	case T_CONTENTS: return EV_CONTENTS;
	case T_PREV: return EV_PREV;
	case T_NEXT: return EV_NEXT;
	case T_ZOOMOUT: return EV_ZOOM_OUT;
	case T_ZOOMIN: return EV_ZOOM_IN;
	case T_FITW: return EV_FIT_WIDTH;
	case T_DARK: return EV_DARK;
	case T_FIND: return EV_FIND;
	case T_DEFINE: return EV_DEFINE;
	}
	return 0;
}

/* The page number typed into the page field (1-based), or 0. */
int toolbar_typed_page(ObjectId tb)
{
	char s[16];
	s[0] = 0;
	_swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, tb, 513 /* WritableField_GetValue */, T_PAGE,
		s, (int)sizeof s);
	s[sizeof s - 1] = 0;
	return atoi(s);
}

int toolbar_is_page_field(int component)
{
	return component == T_PAGE;
}
