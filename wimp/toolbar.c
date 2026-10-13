/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* The document window's toolbar: a Toolbox internal top-left toolbar
 * (User Interface Toolbox p334-336), one per window, with sprite buttons for
 * things also on the menus (Style Guide: a toolbar is a short cut, never
 * the only way), a page number to type into, and the zoom.
 *
 *   [Thumbnails][Contents] | [Up][page] of N [Down] | [-] 100% [+] [Fit] [Two] |
 *   [Dark] | [Find][Define] | [Highlight][Underline][Strike][Note] | [Read aloud]
 *
 * Read aloud is there only with the Speech plugin (it's the last gadget, so
 * the template simply has one fewer without it).
 *
 * Document windows open at least as wide as all the buttons. If the reader
 * makes one narrower, a pair of scroll arrows appears at the toolbar's right
 * end (a small Wimp child window of the toolbar) and the toolbar scrolls
 * sideways; the mouse wheel over the toolbar scrolls it too.
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
#define TB_W     1500
#define SCR_W    80        /* the scroll arrows at the right, when it doesn't fit */
#define SCR_STEP 192       /* how far one click on an arrow scrolls */

enum {
	T_THUMBS = 1, T_CONTENTS, T_PREV, T_PAGE, T_OF, T_NEXT, T_ZOOMOUT, T_ZOOM, T_ZOOMIN,
	T_FITW, T_TWOUP, T_DARK, T_FIND, T_DEFINE, T_HILITE, T_ULINE, T_STRIKE, T_NOTE, T_SPEAK, T_COUNT
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
	button_gadget zoomin, fitw, twoup, dark, find, define, hilite, uline, strike, note, speak;
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

/* Each gadget's header, by component id, and its tooltip. */
static gadget_hdr *gadgets[T_COUNT];
static const char *const tips[T_COUNT] = {
	NULL,
	"Thumbnails", "Contents", "Previous page (Page Up)",
	"Page number: type one and press Return", NULL, "Next page (Page Down)",
	"Zoom out", "Scale", "Zoom in", "Fit width", "Two pages side by side", "Dark page (Ctrl-N)", "Find (F4)",
	"Define the selected word", "Highlight the selection",
	"Underline the selection", "Strike out the selection", "Add a note", "Read aloud (click again to stop)"
};

/* How wide the buttons are, all together. */
static int content_w = TB_W;

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
	button(&g.fitw, T_FITW, x, "R5;Stb_fitw", "Click to fit the pages to the window's width."); x += BTN + 4;
	button(&g.twoup, T_TWOUP, x, "R5;Stb_twoup", "Click to show two pages side by side; click again for one."); x += BTN + 20;
	button(&g.dark, T_DARK, x, "R5;Stb_dark", "Click for a black page with light text (^N); click again for white."); x += BTN + 20;
	button(&g.find, T_FIND, x, "R5;Stb_find", "Click to find text (F4)."); x += BTN + 4;
	button(&g.define, T_DEFINE, x, "R5;Stb_define", "Click to see what the selected word means."); x += BTN + 20;
	button(&g.hilite, T_HILITE, x, "R5;Stb_hilite", "Click to highlight the selected text (PDFs).|MSave the document (F3) to keep it."); x += BTN + 4;
	button(&g.uline, T_ULINE, x, "R5;Stb_uline", "Click to underline the selected text (PDFs)."); x += BTN + 4;
	button(&g.strike, T_STRIKE, x, "R5;Stb_strike", "Click to strike out the selected text (PDFs)."); x += BTN + 4;
	button(&g.note, T_NOTE, x, "R5;Stb_note", "Click to add a note at the selection, or at the top of the page (PDFs)."); x += BTN + 20;
	button(&g.speak, T_SPEAK, x, "R5;Stb_speak", "Click to hear the document read aloud from here (or from the selection); click again to stop.");
	/* The last gadget's right edge (Note's without the plugin), and a margin. */
	content_w = (speech_available() ? g.speak.h.x1 : g.note.h.x1) + 8;
	gadgets[T_THUMBS] = &g.thumbs.h; gadgets[T_CONTENTS] = &g.contents.h;
	gadgets[T_PREV] = &g.prev.h; gadgets[T_PAGE] = &g.page.h; gadgets[T_OF] = &g.of.h;
	gadgets[T_NEXT] = &g.next.h; gadgets[T_ZOOMOUT] = &g.zoomout.h; gadgets[T_ZOOM] = &g.zoom.h;
	gadgets[T_ZOOMIN] = &g.zoomin.h; gadgets[T_FITW] = &g.fitw.h; gadgets[T_TWOUP] = &g.twoup.h;
	gadgets[T_DARK] = &g.dark.h;
	gadgets[T_FIND] = &g.find.h; gadgets[T_DEFINE] = &g.define.h; gadgets[T_HILITE] = &g.hilite.h;
	gadgets[T_ULINE] = &g.uline.h; gadgets[T_STRIKE] = &g.strike.h; gadgets[T_NOTE] = &g.note.h;
	gadgets[T_SPEAK] = speech_available() ? &g.speak.h : NULL;
	/* Shaded until there's something to mark up (toolbar_update). */
	g.hilite.h.flags = g.uline.h.flags = g.strike.h.flags = g.note.h.flags = (int)(1u << 31);
}

int toolbar_height(void)
{
	return TB_H;
}

int toolbar_width(void)
{
	if (content_w == TB_W)
		build_template();
	return content_w;
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
	t.num_gadgets = speech_available() ? T_COUNT - 1 : T_COUNT - 2;
	t.gadgets = &g;
	t.default_focus = -1;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = TB_W;    /* toolbar_fit() makes it as wide as its window */
	t.window.vis_y1 = TB_H;
	t.window.behind = -1;
	/* No furniture; auto-redraw; Scroll_Requests (bit 8: the mouse wheel
	 * over it scrolls it sideways); new format. */
	t.window.window_flags = 0x80000000u | (1u << 4) | (1u << 8);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 1;
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -TB_H;
	/* Wide enough to span any window, and to scroll the last button clear
	 * of the arrows. */
	t.window.work_x1 = app.screen_w > content_w + SCR_W ? app.screen_w : content_w + SCR_W;
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

/* What each toolbar shows now, so only what changes is set: every Toolbox
 * method redraws its gadget, even with the same value, and while text is
 * dragged over the toolbar is updated on every step (test12 flickered). */
typedef struct {
	ObjectId tb;
	char page[24], of[24], zoom[24];
	int dark, thumbs, two, define, markup, note, speak;
	int tb_wimp;            /* the toolbar's Wimp window (once fitted) */
	int scroller;           /* the arrows' Wimp window, or 0 */
	int scx;                /* how far it's scrolled */
	/* The last fit that the Wimp didn't take as asked (e.g. the window runs
	 * off the screen, so the pane is clipped): the parent's x0 and width,
	 * and the toolbar's state it gave. Not asked again until something
	 * changes (test29 reopened it on every poll, slowing reading aloud). */
	int fit_x0, fit_want, fit_got_x0, fit_got_x1, fit_got_scx;
	int logged_want;        /* the width last logged */
} shown_state;

static shown_state shown[16];

static shown_state *state_for(ObjectId tb)
{
	int i, free_i = -1;
	for (i = 0; i < 16; i++) {
		if (shown[i].tb == tb)
			return &shown[i];
		if (!shown[i].tb && free_i < 0)
			free_i = i;
	}
	i = free_i >= 0 ? free_i : 0;    /* (more than 16 windows: share, and set more often) */
	memset(&shown[i], 0, sizeof shown[i]);
	shown[i].tb = tb;
	shown[i].dark = shown[i].thumbs = shown[i].two = shown[i].speak = shown[i].define = shown[i].markup = shown[i].note = -1;
	return &shown[i];
}

void toolbar_forget(ObjectId tb)
{
	int i;
	for (i = 0; i < 16; i++)
		if (shown[i].tb == tb) {
			if (shown[i].scroller) {
				int b[1];
				b[0] = shown[i].scroller;
				_swix(Wimp_DeleteWindow, _IN(1), b);
			}
			memset(&shown[i], 0, sizeof shown[i]);
		}
}

static void set_if(ObjectId tb, int comp, int method, char *was, size_t size, const char *now)
{
	if (strcmp(was, now)) {
		snprintf(was, size, "%s", now);
		set_text(tb, comp, method, now);
	}
}

static void flags_if(ObjectId tb, int comp, int *was, int shaded)
{
	if (*was != shaded) {
		*was = shaded;
		tbx_method(tb, 65 /* Gadget_SetFlags */, comp, shaded ? (int)(1u << 31) : 0, 0);
	}
}

void toolbar_update(ObjectId tb, int page, int pages, int exact, float zoom,
		int dark, int thumbs, int two, int has_selection, int can_annotate, int speaking)
{
	char s[24];
	int caret[6], wh = 0;
	shown_state *st;
	if (!tb)
		return;
	st = state_for(tb);
	/* Not while the reader is typing a page number there. */
	_swix(Wimp_GetCaretPosition, _IN(1), caret);
	window_get_wimp_handle(tb, &wh);
	snprintf(s, sizeof s, "%d", page + 1);
	if (caret[0] != wh)
		set_if(tb, T_PAGE, 512 /* WritableField_SetValue */, st->page, sizeof st->page, s);
	else
		st->page[0] = 0;     /* set again once the caret has gone */
	snprintf(s, sizeof s, exact ? "of %d" : "of %d+", pages);
	set_if(tb, T_OF, 448 /* DisplayField_SetValue */, st->of, sizeof st->of, s);
	snprintf(s, sizeof s, "%d%%", (int)(zoom * 100 + 0.5f));
	set_if(tb, T_ZOOM, 448, st->zoom, sizeof st->zoom, s);
	if (st->dark != dark) {
		st->dark = dark;
		set_selected(tb, T_DARK, dark);
	}
	if (st->thumbs != thumbs) {
		st->thumbs = thumbs;
		set_selected(tb, T_THUMBS, thumbs);
	}
	if (st->two != two) {
		st->two = two;
		set_selected(tb, T_TWOUP, two);
	}
	if (speech_available() && st->speak != speaking) {
		st->speak = speaking;
		set_selected(tb, T_SPEAK, speaking);
	}
	/* Markup needs a selection in a PDF; a note just a PDF. */
	flags_if(tb, T_DEFINE, &st->define, !has_selection);
	if (st->markup != (can_annotate && has_selection)) {
		int shaded = !(can_annotate && has_selection);
		st->markup = !shaded;
		tbx_method(tb, 65, T_HILITE, shaded ? (int)(1u << 31) : 0, 0);
		tbx_method(tb, 65, T_ULINE, shaded ? (int)(1u << 31) : 0, 0);
		tbx_method(tb, 65, T_STRIKE, shaded ? (int)(1u << 31) : 0, 0);
	}
	flags_if(tb, T_NOTE, &st->note, !can_annotate);
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
	case T_TWOUP: return EV_TWO_PAGES;
	case T_DARK: return EV_DARK;
	case T_FIND: return EV_FIND;
	case T_DEFINE: return EV_DEFINE;
	case T_HILITE: return EV_HIGHLIGHT;
	case T_ULINE: return EV_UNDERLINE;
	case T_STRIKE: return EV_STRIKE;
	case T_NOTE: return EV_NOTE;
	case T_SPEAK: return EV_READ_ALOUD;
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

/* ---- Tooltips ------------------------------------------------------------- */
/* RISC OS has interactive help (the gadgets' help messages, shown by !Help);
 * these are the short labels many readers expect as well. When the pointer
 * rests on a toolbar gadget for half a second, a small box with its name
 * appears just below it; it goes when the pointer moves off or clicks. */

#define TIP_DELAY 50       /* cs */
#define TIP_H     44

static struct {
	int wimp;               /* the tip window */
	int tb_wimp;            /* toolbar the pointer is in, or 0 */
	int comp;               /* gadget under the pointer, or -1 */
	unsigned since;
	int shown;
	const char *text;
	int w;
} tip;

static unsigned now_cs(void)
{
	unsigned t;
	_swix(OS_ReadMonotonicTime, _OUT(0), &t);
	return t;
}

static int create_tip_window(void)
{
	int b[23], h = 0;
	memset(b, 0, sizeof b);
	b[2] = 400; b[3] = TIP_H;
	b[6] = -1;
	/* No furniture, not moveable; kept in front (bit 23); new format. */
	b[7] = (int)(0x80000000u | (1u << 23));
	b[8] = 7 | (2 << 8) | (7 << 16) | (255u << 24);
	b[9] = 3 | (1 << 8) | (12 << 16);
	b[10] = 0; b[11] = -TIP_H; b[12] = 2000; b[13] = 0;
	b[15] = 0;                  /* work area: ignore clicks */
	b[16] = 1;
	if (_swix(Wimp_CreateWindow, _IN(1) | _OUT(0), b, &h))
		return 0;
	return h;
}

void toolbar_tip_hide(void)
{
	if (tip.shown) {
		int b[1];
		b[0] = tip.wimp;
		_swix(Wimp_CloseWindow, _IN(1), b);
		tip.shown = 0;
	}
}

/* The gadget under screen point x,y in toolbar window wh, or -1. */
static int gadget_at(int wh, int x, int y, int *gx0, int *gy0)
{
	int st[9], wx, wy, i;
	st[0] = wh;
	if (_swix(Wimp_GetWindowState, _IN(1), st))
		return -1;
	wx = x - (st[1] - st[5]);
	wy = y - (st[4] - st[6]);
	for (i = 1; i < T_COUNT; i++) {
		gadget_hdr *h = gadgets[i];
		if (h && wx >= h->x0 && wx < h->x1 && wy >= h->y0 && wy < h->y1) {
			*gx0 = (st[1] - st[5]) + h->x0;
			*gy0 = (st[4] - st[6]) + h->y0;
			return i;
		}
	}
	return -1;
}

static int is_scroller(int wh);

void toolbar_pointer(int wimp_handle, int entering)
{
	if (is_scroller(wimp_handle))
		return;             /* the arrows: no tooltips there */
	if (entering) {
		tip.tb_wimp = wimp_handle;
		tip.comp = -1;
	} else if (wimp_handle == tip.tb_wimp) {
		tip.tb_wimp = 0;
		toolbar_tip_hide();
	}
}

int toolbar_tip_owns(int wimp_handle)
{
	return tip.wimp && wimp_handle == tip.wimp;
}

/* Null polls while the pointer is in a toolbar. Returns 1 while tracking. */
int toolbar_background(void)
{
	int ptr[5], comp, gx0 = 0, gy0 = 0;
	if (!tip.tb_wimp)
		return 0;
	_swix(Wimp_GetPointerInfo, _IN(1), ptr);
	if (ptr[3] != tip.tb_wimp && ptr[3] != tip.wimp) {
		tip.tb_wimp = 0;
		toolbar_tip_hide();
		return 0;
	}
	comp = ptr[3] == tip.tb_wimp ? gadget_at(tip.tb_wimp, ptr[0], ptr[1], &gx0, &gy0) : tip.comp;
	if (comp != tip.comp || ptr[2]) {
		/* Moved to another gadget, or a button is down: start again. */
		if (comp != tip.comp)
			toolbar_tip_hide();
		tip.comp = comp;
		tip.since = now_cs();
		if (ptr[2])
			toolbar_tip_hide();
		return 1;
	}
	if (comp > 0 && !tip.shown && tips[comp] && (int)(now_cs() - tip.since) >= TIP_DELAY) {
		int b[8], tw = 0;
		if (!tip.wimp && !(tip.wimp = create_tip_window()))
			return 1;
		tip.text = tips[comp];
		_swix(Wimp_TextOp, _INR(0, 2) | _OUT(0), 1, tip.text, 0, &tw);
		tip.w = tw + 24;
		b[0] = tip.wimp;
		b[1] = gx0;
		b[4] = gy0 - 8;           /* just below the gadget */
		b[2] = b[4] - TIP_H;
		b[3] = b[1] + tip.w;
		if (b[3] > app.screen_w) {
			b[1] -= b[3] - app.screen_w;
			b[3] = app.screen_w;
		}
		b[5] = 0;
		b[6] = 0;
		b[7] = -1;
		_swix(Wimp_OpenWindow, _IN(1), b);
		tip.shown = 1;
	}
	return 1;
}

void toolbar_tip_redraw(int *b)
{
	int more = 0;
	b[0] = tip.wimp;
	_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), b, &more);
	while (more) {
		int ox = b[1] - b[5], oy = b[4] - b[6];
		_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), 0xd0ffff00u, 0, 0);   /* pale yellow */
		_swix(OS_Plot, _INR(0, 2), 4, ox, oy - TIP_H);
		_swix(OS_Plot, _INR(0, 2), 96 + 5, ox + tip.w - 1, oy - 1);
		_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), 0x40404000u, 0, 0);
		_swix(OS_Plot, _INR(0, 2), 4, ox, oy - TIP_H);
		_swix(OS_Plot, _INR(0, 2), 5, ox + tip.w - 2, oy - TIP_H);
		_swix(OS_Plot, _INR(0, 2), 5, ox + tip.w - 2, oy - 2);
		_swix(OS_Plot, _INR(0, 2), 5, ox, oy - 2);
		_swix(OS_Plot, _INR(0, 2), 5, ox, oy - TIP_H);
		if (tip.text) {
			_swix(Wimp_TextOp, _INR(0, 2), 0, 0x00000000u, 0xd0ffff00u);
			_swix(Wimp_TextOp, _INR(0, 5), 2 | (1u << 28), tip.text, -1, -1, ox + 12, oy - TIP_H / 2);
		}
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}

/* ---- Width and scrolling -------------------------------------------------- */

#define TASK 0x4b534154

/* How far the toolbar can scroll when it's vis_w wide. */
static int max_scroll(int vis_w)
{
	return vis_w < content_w ? content_w + SCR_W - vis_w : 0;
}

static shown_state *state_for_wimp(int wh)
{
	int i;
	for (i = 0; wh && i < 16; i++)
		if (shown[i].tb && shown[i].tb_wimp == wh)
			return &shown[i];
	return NULL;
}

static shown_state *state_for_scroller(int wh)
{
	int i;
	for (i = 0; wh && i < 16; i++)
		if (shown[i].tb && shown[i].scroller == wh)
			return &shown[i];
	return NULL;
}

static int create_scroller(void)
{
	int b[23], h = 0;
	memset(b, 0, sizeof b);
	b[2] = SCR_W; b[3] = TB_H;
	b[6] = -1;
	/* No furniture, not moveable; new format. */
	b[7] = (int)0x80000000u;
	b[8] = 7 | (2 << 8) | (7 << 16) | (255u << 24);   /* we draw it all */
	b[9] = 3 | (1 << 8) | (12 << 16);
	b[10] = 0; b[11] = -TB_H; b[12] = SCR_W; b[13] = 0;
	b[15] = 2 << 12;            /* clicks, repeating while held */
	b[16] = 1;
	if (_swix(Wimp_CreateWindow, _IN(1) | _OUT(0), b, &h))
		return 0;
	return h;
}

/* Reads a toolbar's state; *parent is -1 for a top-level pane. */
static int tb_state(int wh, int *st, int *parent, unsigned *nest)
{
	st[0] = wh;
	*parent = -1;
	*nest = 0;
	if (_swix(Wimp_GetWindowState, _INR(1, 2) | _OUTR(3, 4), st, TASK, parent, nest)) {
		*parent = -1;
		if (_swix(Wimp_GetWindowState, _IN(1), st))
			return -1;
	}
	return 0;
}

static void tb_reopen(int *st, int parent, unsigned nest)
{
	if (parent != -1)
		_swix(Wimp_OpenWindow, _INR(1, 4), st, TASK, parent, nest & ~1u);
	else
		_swix(Wimp_OpenWindow, _IN(1), st);
}

/* The arrows: open at the toolbar's right end (a child of the toolbar,
 * linked to its right edge) while it's narrower than its buttons, else
 * closed. st is the toolbar's state. */
static void place_scroller(shown_state *s, const int *st)
{
	int b[9], open = 0;
	int clipped = st[3] - st[1] < content_w;
	if (s->scroller) {
		b[0] = s->scroller;
		if (!_swix(Wimp_GetWindowState, _IN(1), b))
			open = (b[8] & (1 << 16)) != 0;
	}
	if (!clipped) {
		if (open) {
			b[0] = s->scroller;
			_swix(Wimp_CloseWindow, _IN(1), b);
		}
		return;
	}
	if (!s->scroller && !(s->scroller = create_scroller()))
		return;
	if (open && b[3] == st[3] && b[1] == st[3] - SCR_W && b[4] == st[4] && b[2] == st[2])
		return;
	b[0] = s->scroller;
	b[1] = st[3] - SCR_W;
	b[2] = st[2];
	b[3] = st[3];
	b[4] = st[4];
	b[5] = 0;
	b[6] = 0;
	b[7] = -1;
	/* Left and right edges to the parent's right; bottom to its bottom,
	 * top to its top; scroll offsets likewise. */
	if (_swix(Wimp_OpenWindow, _INR(1, 4), b, TASK, s->tb_wimp,
			(2u << 16) | (1u << 18) | (2u << 20) | (2u << 22) | (2u << 24) | (2u << 26)))
		app_log("toolbar: can't open the scroll arrows (needs the nested Window Manager)");
	else
		_swix(Wimp_ForceRedraw, _INR(0, 4), s->scroller, 0, -TB_H, SCR_W, 0);
}

/* Makes the toolbar exactly as wide as its window's visible area. The
 * Toolbox keeps a toolbar at its template's size and doesn't trim it to the
 * window (test9: it ran past the right edge), so after each open it's
 * reopened at the window's width, scrolled to where the reader left it.
 * Read with the nested form of Wimp_GetWindowState: as a Wimp child it's
 * reopened with the same parent and edge links; as a top-level pane, in
 * place. */
void toolbar_fit(ObjectId tb, int parent_wimp)
{
	int st[9], ps[9], wh = 0, parent = -1, want, scx;
	unsigned nest = 0;
	shown_state *s;
	if (!tb || window_get_wimp_handle(tb, &wh) || !wh)
		return;
	if (tb_state(wh, st, &parent, &nest))
		return;
	if (!(st[8] & (1 << 16)))
		return;                   /* not showing */
	ps[0] = parent_wimp;
	if (_swix(Wimp_GetWindowState, _IN(1), ps))
		return;
	s = state_for(tb);
	s->tb_wimp = wh;
	want = ps[3] - ps[1];
	if (want < 64)
		return;
	scx = s->scx;
	if (scx > max_scroll(want))
		scx = max_scroll(want);
	if (scx < 0)
		scx = 0;
	if (st[1] != ps[1] || st[3] - st[1] != want || st[5] != scx) {
		if (s->fit_want == want && s->fit_x0 == ps[1] && s->fit_got_x0 == st[1] &&
				s->fit_got_x1 == st[3] && s->fit_got_scx == st[5]) {
			place_scroller(s, st);
			return;           /* asked already: this is what the Wimp allows */
		}
		if (st[3] - st[1] != want && want != s->logged_want && (s->logged_want = want) != 0)
			app_log("toolbar: %d wide (%s)%s", want, parent != -1 ? "child" : "pane",
				want < content_w ? ", scrolls" : "");
		st[1] = ps[1];
		st[3] = ps[1] + want;
		st[5] = scx;
		s->scx = scx;
		tb_reopen(st, parent, nest);
		if (tb_state(wh, st, &parent, &nest))
			return;
		if (st[1] != ps[1] || st[3] - st[1] != want || st[5] != scx) {
			s->fit_x0 = ps[1];
			s->fit_want = want;
			s->fit_got_x0 = st[1];
			s->fit_got_x1 = st[3];
			s->fit_got_scx = st[5];
		} else {
			s->fit_want = 0;
		}
	}
	place_scroller(s, st);
}

/* An Open_Window_Request for the toolbar's window (b, a Wimp_OpenWindow
 * block): the usual pane way, so a drag of the window by its title bar is
 * a plain Wimp_OpenWindow each step. If the toolbar is a top-level pane,
 * it's opened first, along the top of where the window is going and in
 * the window's place in the stack, and b[7] is set so the window goes
 * just behind it; the caller then opens the window. Returns the pane's
 * handle (0: no pane to move: a Wimp child moves with its parent). */
int toolbar_open_with(ObjectId tb, int *b)
{
	int st[9], wh = 0, parent = -1, h, want, scx;
	unsigned nest = 0;
	shown_state *s;
	if (!tb || window_get_wimp_handle(tb, &wh) || !wh)
		return 0;
	if (tb_state(wh, st, &parent, &nest) || parent != -1 || !(st[8] & (1 << 16)))
		return 0;
	s = state_for(tb);
	s->tb_wimp = wh;
	h = st[4] - st[2];
	want = b[3] - b[1];
	scx = s->scx;
	if (scx > max_scroll(want))
		scx = max_scroll(want);
	if (scx < 0)
		scx = 0;
	st[1] = b[1];
	st[3] = b[3];
	st[4] = b[4];
	st[2] = b[4] - h;
	st[5] = scx;
	/* In the window's place in the stack (its own if the window is to stay
	 * just behind it, as during a drag). */
	if (b[7] != wh)
		st[7] = b[7];          /* else st[7] (from its state) keeps its place */
	_swix(Wimp_OpenWindow, _IN(1), st);
	b[7] = wh;
	return wh;
}

/* After the window opened (it may have been moved by the Wimp, e.g. kept
 * on the screen): puts the pane back along its top if it isn't. */
void toolbar_follow(ObjectId tb, int pane, int parent_wimp)
{
	int st[9], ps[9];
	if (!pane)
		return;
	ps[0] = parent_wimp;
	st[0] = pane;
	if (_swix(Wimp_GetWindowState, _IN(1), ps) || _swix(Wimp_GetWindowState, _IN(1), st))
		return;
	if (st[1] == ps[1] && st[4] == ps[4] && st[3] - st[1] == ps[3] - ps[1])
		return;
	{
		int h = st[4] - st[2];
		st[1] = ps[1];
		st[3] = ps[3];
		st[4] = ps[4];
		st[2] = ps[4] - h;
		_swix(Wimp_OpenWindow, _IN(1), st);
	}
	(void)tb;
}

/* Scrolls a toolbar sideways by dx (clamped). */
static void scroll_by(shown_state *s, int dx)
{
	int st[9], parent, max;
	unsigned nest;
	if (tb_state(s->tb_wimp, st, &parent, &nest))
		return;
	max = max_scroll(st[3] - st[1]);
	dx += st[5];
	if (dx > max)
		dx = max;
	if (dx < 0)
		dx = 0;
	if (dx == st[5])
		return;
	toolbar_tip_hide();
	st[5] = s->scx = dx;
	tb_reopen(st, parent, nest);
	if (s->scroller)
		_swix(Wimp_ForceRedraw, _INR(0, 4), s->scroller, 0, -TB_H, SCR_W, 0);
}

int toolbar_scroller_owns(int wimp_handle)
{
	return state_for_scroller(wimp_handle) != NULL;
}

static int is_scroller(int wh)
{
	return toolbar_scroller_owns(wh);
}

/* Left half scrolls left, right half right; Adjust the other way (as on a
 * scroll bar's arrows). */
void toolbar_scroller_click(int *b)
{
	shown_state *s = state_for_scroller(b[3]);
	int st[9], dir;
	if (!s || !(b[2] & 5))
		return;
	st[0] = s->scroller;
	if (_swix(Wimp_GetWindowState, _IN(1), st))
		return;
	dir = b[0] - st[1] < SCR_W / 2 ? -1 : 1;
	if (b[2] & 1)
		dir = -dir;
	scroll_by(s, dir * SCR_STEP);
}

/* The mouse wheel over a toolbar (a Scroll_Request): 1 if it was one. */
int toolbar_scroll_request(int *b)
{
	shown_state *s = state_for_wimp(b[0]);
	int dx;
	if (!s)
		return 0;
	/* Sideways if the wheel says so; otherwise up is left, down is right. */
	dx = b[8] ? b[8] : -b[9];
	if (dx)
		scroll_by(s, (dx > 0 ? 1 : -1) * SCR_STEP / 2);
	return 1;
}

void toolbar_scroller_redraw(int *b)
{
	shown_state *s = state_for_scroller(b[0]);
	int more = 0, st[9], scx = 0, max = 0;
	if (s) {
		st[0] = s->tb_wimp;
		if (!_swix(Wimp_GetWindowState, _IN(1), st)) {
			scx = st[5];
			max = max_scroll(st[3] - st[1]);
		}
	}
	_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), b, &more);
	while (more) {
		int ox = b[1] - b[5], oy = b[4] - b[6], half = SCR_W / 2, mid = oy - TB_H / 2, i;
		/* The toolbar's grey, a shadow line at the left, and two arrows,
		 * greyed out at the ends. */
		_swix(Wimp_SetColour, _IN(0), 1);
		_swix(OS_Plot, _INR(0, 2), 4, ox, oy - TB_H);
		_swix(OS_Plot, _INR(0, 2), 96 + 5, ox + SCR_W - 1, oy - 1);
		_swix(Wimp_SetColour, _IN(0), 4);
		_swix(OS_Plot, _INR(0, 2), 4, ox, oy - TB_H);
		_swix(OS_Plot, _INR(0, 2), 5, ox, oy - 1);
		_swix(Wimp_SetColour, _IN(0), 0);
		_swix(OS_Plot, _INR(0, 2), 4, ox + 2, oy - TB_H);
		_swix(OS_Plot, _INR(0, 2), 5, ox + 2, oy - 1);
		for (i = 0; i < 2; i++) {
			int cx = ox + 4 + half * i + (half - 4) / 2;
			int dir = i ? 1 : -1;
			_swix(Wimp_SetColour, _IN(0), (i ? scx < max : scx > 0) ? 7 : 3);
			_swix(OS_Plot, _INR(0, 2), 4, cx - dir * 10, mid + 16);
			_swix(OS_Plot, _INR(0, 2), 4, cx - dir * 10, mid - 16);
			_swix(OS_Plot, _INR(0, 2), 80 + 5, cx + dir * 10, mid);
		}
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}
