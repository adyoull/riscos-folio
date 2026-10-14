/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* A small question or notice box: a title, a few lines of text, and one or
 * two buttons (e.g. "Open this link? ... Cancel / Open"). It doesn't stop
 * the desktop (unlike an error box): the answer comes back through a
 * function given when it's shown. One box at a time; a new question
 * replaces one not answered (which counts as Cancel). */
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define LINES 6
#define LINE_CHARS 64

enum { A_LINE0 = 0, A_CANCEL = LINES, A_OK };

typedef struct {
	int flags, type;
	int x0, y0, x1, y1;
	ComponentId id;
	const char *help;
	int max_help;
} gadget_hdr;

typedef struct {
	gadget_hdr h;
	int icon_flags;
	char *value;
	int max_value;
	const char *validation;
	int max_validation;
} text_gadget;

typedef struct {
	gadget_hdr h;
	const char *text;
	int max_text;
	const char *click_show;
	int event;
} button_gadget;

static struct {
	text_gadget line[LINES];
	button_gadget cancel, ok;
} g;

static struct {
	ObjectId obj;
	char text[LINES][LINE_CHARS + 1];
	char ok_label[24], cancel_label[24];
	ask_done_fn *done;
	void *data;
} ak;

#define BOX_W 1120
#define BOX_H (32 + LINES * 44 + 24 + 68 + 24)

static void hdr(gadget_hdr *h, int type, int size, int id, int x0, int y0, int x1, int y1)
{
	h->flags = 0;
	h->type = (size << 16) | type;
	h->x0 = x0; h->y0 = y0; h->x1 = x1; h->y1 = y1;
	h->id = id;
	h->help = NULL;
	h->max_help = 0;
}

static ObjectId create_window(void)
{
	static tbx_window t;
	static char title[64] = "Folio";
	ObjectId id = 0;
	int i, by = -(32 + LINES * 44 + 24);
	memset(&g, 0, sizeof g);
	for (i = 0; i < LINES; i++) {
		text_gadget *l = &g.line[i];
		hdr(&l->h, 960, sizeof *l, A_LINE0 + i, 24, -32 - (i + 1) * 44, BOX_W - 24, -32 - i * 44);
		/* Plain text, vertically centred, no border. */
		l->icon_flags = 1 | (1 << 4) | (1 << 8) | (7 << 24) | (1 << 28);
		l->value = ak.text[i];
		l->max_value = LINE_CHARS + 1;
		l->validation = NULL;
		l->max_validation = 0;
	}
	hdr(&g.cancel.h, 128, sizeof g.cancel, A_CANCEL, BOX_W - 424, by - 68, BOX_W - 232, by);
	g.cancel.h.flags = 2 | (1 << 2);      /* Escape; local (Folio hides the box) */
	g.cancel.text = ak.cancel_label;
	g.cancel.max_text = sizeof ak.cancel_label;
	g.cancel.event = EV_ASK_CANCEL;
	hdr(&g.ok.h, 128, sizeof g.ok, A_OK, BOX_W - 216, by - 68, BOX_W - 24, by);
	g.ok.h.flags = 1 | (1 << 2);          /* Return; local */
	g.ok.text = ak.ok_label;
	g.ok.max_text = sizeof ak.ok_label;
	g.ok.event = EV_ASK_OK;

	memset(&t, 0, sizeof t);
	t.flags = (1 << 1) | (1 << 2);
	t.num_gadgets = LINES + 2;
	t.gadgets = &g;
	t.default_focus = -2;                  /* the window itself (for Return and Escape) */
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = BOX_W;
	t.window.vis_y1 = BOX_H;
	t.window.behind = -1;
	t.window.window_flags = 0x80000002u | (1u << 4) | (1u << 26);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 1;
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -BOX_H;
	t.window.work_x1 = BOX_W;
	t.window.title_flags = 0x00000119;
	t.window.work_flags = 0;
	t.window.sprite_area = (void *)1;
	t.window.title_text = title;
	t.window.title_validation = "";
	t.window.title_buflen = sizeof title;
	if (tbx_create(Class_Window, 102, 0, &t, sizeof t, &id))
		return 0;
	return id;
}

/* Words into lines of up to LINE_CHARS; '\n' starts a new line. A long word
 * (a web address) is broken where it must be. */
static void wrap(const char *s)
{
	int line = 0, len = 0;
	memset(ak.text, 0, sizeof ak.text);
	while (*s && line < LINES) {
		const char *e = s;
		int wl;
		if (*s == '\n') {
			line++;
			len = 0;
			s++;
			continue;
		}
		while (*e && *e != ' ' && *e != '\n')
			e++;
		wl = (int)(e - s);
		if (len && len + 1 + wl > LINE_CHARS) {
			line++;
			len = 0;
			if (line >= LINES)
				break;
		}
		while (wl > 0 && line < LINES) {
			int room = LINE_CHARS - len - (len ? 1 : 0), take = wl < room ? wl : room;
			if (take <= 0) {
				line++;
				len = 0;
				continue;
			}
			if (len)
				ak.text[line][len++] = ' ';
			memcpy(ak.text[line] + len, s, take);
			len += take;
			s += take;
			wl -= take;
			if (wl > 0) {
				line++;
				len = 0;
			}
		}
		while (*s == ' ')
			s++;
	}
}

void ask_show(const char *title, const char *desktop_text, const char *ok, const char *cancel,
		ask_done_fn *done, void *data)
{
	int i, pos[2];
	ask_done_fn *was = ak.done;
	void *was_data = ak.data;
	ak.done = NULL;
	if (was)
		was(was_data, 0);               /* the last one unanswered: Cancel */
	if (!ak.obj && !(ak.obj = create_window())) {
		if (done)
			done(data, 0);
		return;
	}
	ak.done = done;
	ak.data = data;
	wrap(desktop_text);
	for (i = 0; i < LINES; i++)
		tbx_method(ak.obj, 962 /* Button_SetValue */, A_LINE0 + i, (int)ak.text[i], 0);
	snprintf(ak.ok_label, sizeof ak.ok_label, "%s", ok);
	tbx_method(ak.obj, 128 /* ActionButton_SetText */, A_OK, (int)ak.ok_label, 0);
	if (cancel) {
		snprintf(ak.cancel_label, sizeof ak.cancel_label, "%s", cancel);
		tbx_method(ak.obj, 128, A_CANCEL, (int)ak.cancel_label, 0);
	}
	/* One button only: Cancel is faded and stands for nothing. */
	{
		int fl = 0;
		tbx_method_out(ak.obj, 64 /* Gadget_GetFlags */, A_CANCEL, &fl);
		fl = cancel ? fl & ~(int)(1u << 31) : fl | (int)(1u << 31);
		tbx_method(ak.obj, 65 /* Gadget_SetFlags */, A_CANCEL, fl, 0);
	}
	window_set_title(ak.obj, title);
	pos[0] = (app.screen_w - BOX_W) / 2;
	pos[1] = (app.screen_h + BOX_H) / 2 + 100;
	tbx_show(ak.obj, 0, 2, pos, 0, -1);
}

void ask_event(int code)
{
	ask_done_fn *done = ak.done;
	void *data = ak.data;
	if (code == EV_ASK_OK || code == EV_ASK_CANCEL) {
		ak.done = NULL;
		if (ak.obj)
			tbx_hide(ak.obj);
		if (done)
			done(data, code == EV_ASK_OK);
	}
}

/* A window going away: its question can't be answered now. */
void ask_forget(void *data)
{
	if (ak.done && ak.data == data) {
		ak.done = NULL;
		if (ak.obj)
			tbx_hide(ak.obj);
	}
}
