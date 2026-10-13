/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* The password boxes (PDFs):
 *
 *  - Open: asks for the password of a document that needs one, and tries
 *    again (saying so) if it isn't right.
 *  - Restricted: a PDF whose author doesn't allow printing or copying
 *    (Choices > Obey document restrictions): says so, and lifts it with the
 *    owner password, then does what was asked.
 *  - Password (the document menu): shows whether the document has a password,
 *    and sets a new one or takes it off. The change is made by saving (a
 *    full save, AES-256), so the Save box opens straight after.
 *
 * The writable fields hide what's typed (WritableField flag bit 4, User
 * Interface Toolbox p426). Passwords are typed in the desktop's alphabet and
 * handed to the core as UTF-8; every copy is wiped once used. */
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define PW_MAX 128

typedef struct {
	int flags, type;
	int x0, y0, x1, y1;
	ComponentId id;
	const char *help;
	int max_help;
} gadget_hdr;

typedef struct { gadget_hdr h; const char *label; } label_gadget;
typedef struct {
	gadget_hdr h;
	char *text;
	int max_text;
	const char *allowable;
	int max_allowable;
	ComponentId before, after;
} writable_gadget;
typedef struct { gadget_hdr h; char *text; int max_text; } display_gadget;
typedef struct {
	gadget_hdr h;
	const char *text;
	int max_text;
	const char *click_show;
	int event;
} button_gadget;

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

static void label(label_gadget *g, int id, int x0, int y0, int x1, int y1, const char *text)
{
	hdr(&g->h, 320, sizeof *g, id, x0, y0, x1, y1, NULL);
	g->h.flags = 1 | (1 << 1);   /* no box, right-justified */
	g->label = text;
}

static void writable(writable_gadget *g, int id, int x0, int y0, int x1, int y1, char *buf,
		int before, int after, const char *help)
{
	hdr(&g->h, 512, sizeof *g, id, x0, y0, x1, y1, help);
	g->h.flags = 1 << 4;         /* shows '-' for each character typed */
	buf[0] = 0;
	g->text = buf;
	g->max_text = PW_MAX;
	g->before = before;
	g->after = after;
}

static void display(display_gadget *g, int id, int x0, int y0, int x1, int y1, char *buf, int size,
		const char *help)
{
	hdr(&g->h, 448, sizeof *g, id, x0, y0, x1, y1, help);
	buf[0] = 0;
	g->text = buf;
	g->max_text = size;
}

static void button(button_gadget *b, int id, int flags, int x0, int x1, int y0, int y1,
		const char *text, int event, const char *help)
{
	hdr(&b->h, 128, sizeof *b, id, x0, y0, x1, y1, help);
	b->h.flags = flags;
	b->text = text;
	b->max_text = (int)strlen(text) + 1;
	b->event = event;
}

static ObjectId make_window(tbx_window *t, char *title, int title_size, void *gadgets, int n,
		int focus, int w, int h, const char *help)
{
	ObjectId id = 0;
	_kernel_oserror *e;
	memset(t, 0, sizeof *t);
	t->flags = (1 << 1) | (1 << 2);   /* the Toolbox opens and closes it */
	t->help_message = help;
	t->max_help = (int)strlen(help) + 1;
	t->num_gadgets = n;
	t->gadgets = gadgets;
	t->default_focus = focus;
	t->show_event = -1;
	t->hide_event = -1;
	t->window.vis_x1 = w;
	t->window.vis_y1 = h;
	t->window.behind = -1;
	/* Moveable, auto-redraw, title; no close icon (a dialogue: Cancel). */
	t->window.window_flags = 0x80000002u | (1u << 4) | (1u << 26);
	t->window.title_fore = 7;
	t->window.title_back = 2;
	t->window.work_fore = 7;
	t->window.work_back = 1;
	t->window.scroll_outer = 3;
	t->window.scroll_inner = 1;
	t->window.title_focus = 12;
	t->window.work_y0 = -h;
	t->window.work_x1 = w;
	t->window.title_flags = 0x00000119;
	t->window.sprite_area = (void *)1;
	t->window.title_text = title;
	t->window.title_validation = "";
	t->window.title_buflen = title_size;
	e = tbx_create(Class_Window, 102, 0, t, sizeof *t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	return id;
}

/* Reads a writable field (desktop alphabet) into buf. */
static void get_field(ObjectId obj, int id, char *buf, int size)
{
	buf[0] = 0;
	_swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, obj, 513, id, buf, size);
	buf[size - 1] = 0;
}

static void set_text(ObjectId obj, int id, const char *text)
{
	tbx_method(obj, id >= 0x100 ? 448 : 512, id & 0xff, (int)text, 0);
}

static void wipe(char *buf, int size)
{
	volatile char *p = buf;
	while (size-- > 0)
		*p++ = 0;
}

/* Desktop text to UTF-8, wiping the original. malloc'd (wipe and free it). */
static char *to_utf8(char *desktop)
{
	char *u = app_utf8_text(desktop);
	wipe(desktop, (int)strlen(desktop));
	return u;
}

static void free_secret(char *s)
{
	if (s) {
		wipe(s, (int)strlen(s));
		free(s);
	}
}

static void centre_show(ObjectId obj, int w, int h)
{
	int pos[2];
	pos[0] = (app.screen_w - w) / 2;
	pos[1] = (app.screen_h + h) / 2;
	tbx_show(obj, 0, 2, pos, 0, -1);
}

static const char *leaf(const char *path)
{
	const char *dot = strrchr(path, '.'), *colon = strrchr(path, ':');
	const char *p = dot > colon ? dot : colon;
	return p ? p + 1 : path;
}

/* ---- Open ---------------------------------------------------------------- */

#define OPEN_W 760
#define OPEN_H 236
/* Display fields have ids from 0x100 here (set_text tells them apart). */
enum { O_MSG = 0x100, O_LABEL = 1, O_PW = 2, O_CANCEL = 3, O_OK = 4 };

static struct {
	display_gadget msg;
	label_gadget label;
	writable_gadget pw;
	button_gadget cancel, ok;
} og;

static struct {
	ObjectId obj;
	char path[256];
	char msg[120];
	char pw[PW_MAX];
} op;

static ObjectId create_open(void)
{
	static tbx_window t;
	static char title[40] = "Password";
	display(&og.msg, O_MSG & 0xff, 16, -76, OPEN_W - 16, -24, op.msg, sizeof op.msg,
		"This says which document needs a password.");
	label(&og.label, O_LABEL, 16, -144, 240, -96, "Password");
	writable(&og.pw, O_PW, 256, -148, OPEN_W - 16, -92, op.pw, -1, -1,
		"Type the document's password.|MPress Return (or click Open) to open it.");
	button(&og.cancel, O_CANCEL, 2 /* Cancel: Escape */, OPEN_W - 392, OPEN_W - 216, -220, -164,
		"Cancel", EV_PWOPEN_CANCEL, "Click to leave the document closed.");
	button(&og.ok, O_OK, 1 /* default: Return */, OPEN_W - 200, OPEN_W - 16, -220, -164,
		"Open", EV_PWOPEN_OK, "Click to open the document with this password.");
	return make_window(&t, title, sizeof title, &og, 5, O_PW, OPEN_W, OPEN_H,
		"Folio needs the document's password to open it.");
}

void passui_ask_open(const char *path, int wrong)
{
	if (!op.obj && !(op.obj = create_open()))
		return;
	snprintf(op.path, sizeof op.path, "%s", path);
	if (wrong)
		snprintf(op.msg, sizeof op.msg, "That isn't the password for '%.60s'. Try again.", leaf(path));
	else
		snprintf(op.msg, sizeof op.msg, "'%.70s' needs a password to open it.", leaf(path));
	set_text(op.obj, O_MSG, op.msg);
	set_text(op.obj, O_PW, "");
	centre_show(op.obj, OPEN_W, OPEN_H);
}

static void open_event(int code)
{
	char typed[PW_MAX], path[256];
	char *utf8;
	if (code != EV_PWOPEN_OK) {
		set_text(op.obj, O_PW, "");
		tbx_hide(op.obj);
		return;
	}
	get_field(op.obj, O_PW, typed, sizeof typed);
	set_text(op.obj, O_PW, "");
	tbx_hide(op.obj);
	snprintf(path, sizeof path, "%s", op.path);
	utf8 = to_utf8(typed);
	/* Asks again (wrong = 1) if it isn't right. */
	docwin_open_password(path, utf8 ? utf8 : "");
	free_secret(utf8);
	wipe(typed, sizeof typed);
}

/* ---- Password (set or remove) ------------------------------------------- */

#define SEC_W 900
#define SEC_H 436
enum {
	S_STATE = 0x100, S_STATUS = 0x101,
	S_OWNER_L = 2, S_OWNER = 3, S_NEW_L = 4, S_NEW = 5, S_CONF_L = 6, S_CONF = 7,
	S_REMOVE = 8, S_CANCEL = 9, S_SET = 10
};

static struct {
	display_gadget state;
	label_gadget owner_l;
	writable_gadget owner;
	label_gadget new_l;
	writable_gadget newpw;
	label_gadget conf_l;
	writable_gadget conf;
	display_gadget status;
	button_gadget remove, cancel, set;
} sg;

static struct {
	ObjectId obj;
	docwin *w;
	char title[120];
	char state[120], status[120];
	char owner[PW_MAX], newpw[PW_MAX], conf[PW_MAX];
	int need_owner;
} sp;

static ObjectId create_security(void)
{
	static tbx_window t;
	int x = 300;
	display(&sg.state, S_STATE & 0xff, 16, -72, SEC_W - 16, -24, sp.state, sizeof sp.state,
		"This says whether the document has a password now.");
	label(&sg.owner_l, S_OWNER_L, 16, -140, x - 16, -92, "Owner password");
	writable(&sg.owner, S_OWNER, x, -144, SEC_W - 16, -88, sp.owner, -1, S_NEW,
		"This document's security can only be changed with its owner password: type it here.");
	label(&sg.new_l, S_NEW_L, 16, -204, x - 16, -156, "New password");
	writable(&sg.newpw, S_NEW, x, -208, SEC_W - 16, -152, sp.newpw, S_OWNER, S_CONF,
		"Type the password the document will need to open it.");
	label(&sg.conf_l, S_CONF_L, 16, -268, x - 16, -220, "Type it again");
	writable(&sg.conf, S_CONF, x, -272, SEC_W - 16, -216, sp.conf, S_NEW, -1,
		"Type the new password again, to be sure it's what you meant.|MPress Return to set it.");
	display(&sg.status, S_STATUS & 0xff, 16, -336, SEC_W - 16, -288, sp.status, sizeof sp.status,
		"This says what will happen when you save the document.");
	/* Set and Remove are local: the box stays open to say what's wrong. */
	button(&sg.remove, S_REMOVE, 4 /* local */, 16, 316, -420, -356, "Remove password", EV_PWREMOVE,
		"Click to take the password off. The document is saved without one.");
	button(&sg.cancel, S_CANCEL, 2 /* Cancel: Escape */, SEC_W - 432, SEC_W - 256, -420, -356,
		"Cancel", EV_PWCANCEL, "Click to close this box and change nothing.");
	button(&sg.set, S_SET, 1 | 4 /* default: Return; local */, SEC_W - 240, SEC_W - 16, -420, -356,
		"Set password", EV_PWSET, "Click to set the new password. The document is saved with it.");
	return make_window(&t, sp.title, sizeof sp.title, &sg, 11, S_NEW, SEC_W, SEC_H,
		"Use this box to put a password on the document, or take one off.");
}

static void clear_fields(void)
{
	set_text(sp.obj, S_OWNER, "");
	set_text(sp.obj, S_NEW, "");
	set_text(sp.obj, S_CONF, "");
}

static void fade(int id, int on)
{
	tbx_method(sp.obj, 65 /* Gadget_SetFlags */, id, on ? (int)(1u << 31) : 0, 0);
}

/* Fills in the box from the document's state. */
static void security_refresh(void)
{
	folio_security s;
	if (docwin_get_security(sp.w, &s))
		return;
	if (!s.encrypted)
		snprintf(sp.state, sizeof sp.state, "This document has no password.");
	else if (s.needs_password)
		snprintf(sp.state, sizeof sp.state, "This document needs a password to open it (%s).", s.method);
	else
		snprintf(sp.state, sizeof sp.state, "Anyone can open this document; it has an owner password (%s).",
			s.method);
	if (s.change == FOLIO_PW_SET)
		snprintf(sp.status, sizeof sp.status, "A new password will be put on when it's next saved.");
	else if (s.change == FOLIO_PW_REMOVE)
		snprintf(sp.status, sizeof sp.status, "Its password will be taken off when it's next saved.");
	else
		snprintf(sp.status, sizeof sp.status, "Folio uses AES-256. The change is made by saving.");
	set_text(sp.obj, S_STATE, sp.state);
	set_text(sp.obj, S_STATUS, sp.status);
	sp.need_owner = s.encrypted && !s.owner;
	fade(S_OWNER, !sp.need_owner);
	fade(S_OWNER_L, !sp.need_owner);
	fade(S_REMOVE, !s.encrypted && s.change != FOLIO_PW_SET);
	tbx_method(sp.obj, 13 /* Window_SetDefaultFocus */, sp.need_owner ? S_OWNER : S_NEW, 0, 0);
}

void passui_security(docwin *w)
{
	if (!sp.obj && !(sp.obj = create_security()))
		return;
	sp.w = w;
	snprintf(sp.title, sizeof sp.title, "Password for %.90s", leaf(docwin_path(w)));
	window_set_title(sp.obj, sp.title);
	clear_fields();
	security_refresh();
	centre_show(sp.obj, SEC_W, SEC_H);
}

static void status(const char *msg)
{
	snprintf(sp.status, sizeof sp.status, "%s", msg);
	set_text(sp.obj, S_STATUS, sp.status);
}

/* The owner password, if this document needs it: 1 if given and right. */
static int owner_ok(void)
{
	char typed[PW_MAX];
	char *utf8;
	int r;
	if (!sp.need_owner)
		return 1;
	get_field(sp.obj, S_OWNER, typed, sizeof typed);
	if (!typed[0]) {
		status("Type the document's owner password first.");
		return 0;
	}
	utf8 = to_utf8(typed);
	r = docwin_check_owner(sp.w, utf8 ? utf8 : "");
	free_secret(utf8);
	wipe(typed, sizeof typed);
	if (r == 1)
		return 1;
	set_text(sp.obj, S_OWNER, "");
	status(r == 0 ? "That isn't the owner password." : "Folio couldn't check the owner password.");
	return 0;
}

static void security_event(int code)
{
	docwin *w = sp.w;
	char a[PW_MAX], b[PW_MAX];
	char *utf8;
	int rc;
	if (!w || code == EV_PWCANCEL) {
		clear_fields();
		tbx_hide(sp.obj);
		sp.w = NULL;
		return;
	}
	if (!owner_ok())
		return;
	if (code == EV_PWSET) {
		get_field(sp.obj, S_NEW, a, sizeof a);
		get_field(sp.obj, S_CONF, b, sizeof b);
		if (!a[0] || !b[0]) {
			wipe(a, sizeof a);
			wipe(b, sizeof b);
			status("Type the new password in both boxes.");
			return;
		}
		if (strcmp(a, b)) {
			wipe(a, sizeof a);
			wipe(b, sizeof b);
			set_text(sp.obj, S_CONF, "");
			status("The two passwords aren't the same. Type it again.");
			return;
		}
		wipe(b, sizeof b);
		utf8 = to_utf8(a);
		rc = docwin_set_password(w, utf8 ? utf8 : "");
		free_secret(utf8);
		wipe(a, sizeof a);
	} else {
		rc = docwin_set_password(w, NULL);
	}
	if (rc) {
		char msg[300];
		snprintf(msg, sizeof msg, "Folio couldn't change the password: %s.", folio_error(app.fc));
		tbx_report(msg);
		return;
	}
	clear_fields();
	tbx_hide(sp.obj);
	sp.w = NULL;
	/* The change is made by saving: offer that at once (only if there is a
	 * change left: removing a password just set undoes it). */
	{
		folio_security s;
		if (docwin_get_security(w, &s) == 0 && s.change != FOLIO_PW_KEEP)
			save_open(w, 0);
	}
}

/* ---- Restricted ------------------------------------------------------------ */

#define RES_W 900
#define RES_H 300
enum { R_MSG = 0x100, R_HINT = 0x101, R_LABEL = 1, R_PW = 2, R_CANCEL = 3, R_OK = 4 };

static struct {
	display_gadget msg, hint;
	label_gadget label;
	writable_gadget pw;
	button_gadget cancel, ok;
} rg;

static struct {
	ObjectId obj;
	docwin *w;
	int event;                 /* what to do once allowed */
	char msg[140], hint[140];
	char pw[PW_MAX];
} rs;

static ObjectId create_restricted(void)
{
	static tbx_window t;
	static char title[40] = "Not allowed";
	display(&rg.msg, R_MSG & 0xff, 16, -76, RES_W - 16, -24, rs.msg, sizeof rs.msg,
		"This says what the document's author doesn't allow.");
	display(&rg.hint, R_HINT & 0xff, 16, -140, RES_W - 16, -88, rs.hint, sizeof rs.hint,
		"The owner password lifts the document's restrictions.");
	label(&rg.label, R_LABEL, 16, -208, 300, -160, "Owner password");
	writable(&rg.pw, R_PW, 316, -212, RES_W - 16, -156, rs.pw, -1, -1,
		"Type the document's owner password.|MPress Return (or click Allow) to go ahead.");
	button(&rg.cancel, R_CANCEL, 2 /* Cancel: Escape */, RES_W - 392, RES_W - 216, -284, -228,
		"Cancel", EV_RESTRICT_CANCEL, "Click to close this box and do nothing.");
	button(&rg.ok, R_OK, 1 | 4 /* default: Return; local */, RES_W - 200, RES_W - 16, -284, -228,
		"Allow", EV_RESTRICT_OK, "Click to lift the restrictions with this owner password.");
	return make_window(&t, title, sizeof title, &rg, 6, R_PW, RES_W, RES_H,
		"The document's author doesn't allow this. Its owner password lifts that.");
}

void passui_restricted(docwin *w, const char *what, int event)
{
	if (!rs.obj && !(rs.obj = create_restricted()))
		return;
	rs.w = w;
	rs.event = event;
	snprintf(rs.msg, sizeof rs.msg, "The author of '%.50s' doesn't allow %s.", leaf(docwin_path(w)), what);
	snprintf(rs.hint, sizeof rs.hint, "Type its owner password to allow it (or turn off Choices > Obey document restrictions).");
	set_text(rs.obj, R_MSG, rs.msg);
	set_text(rs.obj, R_HINT, rs.hint);
	set_text(rs.obj, R_PW, "");
	centre_show(rs.obj, RES_W, RES_H);
}

static void restricted_event(int code)
{
	char typed[PW_MAX];
	char *utf8;
	int r;
	docwin *w = rs.w;
	if (code != EV_RESTRICT_OK || !w) {
		set_text(rs.obj, R_PW, "");
		tbx_hide(rs.obj);
		rs.w = NULL;
		return;
	}
	get_field(rs.obj, R_PW, typed, sizeof typed);
	set_text(rs.obj, R_PW, "");
	utf8 = to_utf8(typed);
	r = docwin_check_owner(w, utf8 ? utf8 : "");
	free_secret(utf8);
	wipe(typed, sizeof typed);
	if (r != 1) {
		snprintf(rs.hint, sizeof rs.hint, "%s", r == 0 ? "That isn't the owner password. Try again." :
			"Folio couldn't check the owner password.");
		set_text(rs.obj, R_HINT, rs.hint);
		return;
	}
	tbx_hide(rs.obj);
	rs.w = NULL;
	docwin_event(w, rs.event);       /* now allowed */
}

/* ---- Shared --------------------------------------------------------------- */

int passui_event(int code)
{
	switch (code) {
	case EV_PWOPEN_OK:
	case EV_PWOPEN_CANCEL:
		open_event(code);
		return 1;
	case EV_PWSET:
	case EV_PWREMOVE:
	case EV_PWCANCEL:
		security_event(code);
		return 1;
	case EV_RESTRICT_OK:
	case EV_RESTRICT_CANCEL:
		restricted_event(code);
		return 1;
	}
	return 0;
}

void passui_forget(docwin *w)
{
	if (rs.w == w) {
		set_text(rs.obj, R_PW, "");
		tbx_hide(rs.obj);
		rs.w = NULL;
	}
	if (sp.w == w) {
		clear_fields();
		tbx_hide(sp.obj);
		sp.w = NULL;
	}
}
