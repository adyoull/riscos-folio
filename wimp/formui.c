/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Filling in PDF forms in a document window.
 *
 * A Select click on a field:
 * - a text field opens a small writable window exactly over it (a child of
 *   the document window, so it scrolls with the page). Return or Tab keeps
 *   what's typed (Tab then goes on to the page's next text field, Shift-Tab
 *   back); Escape leaves the field as it was; clicking elsewhere keeps it.
 *   A multi-line field has a line of its own for each line it shows: Return
 *   and the arrow keys move between them.
 * - a check box or radio button is ticked or cleared;
 * - a combo or list box shows its choices as a menu at the pointer;
 * - a push button runs the form's script (only with JavaScript on in
 *   Choices); a signature can't be changed.
 * Each change is one step of undo (Ctrl-Z), as for notes, and the window's
 * title shows * until the PDF is saved. */
#include "app.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define MAX_LINES 8
#define LINE_LEN 256

static struct {
	docwin *w;               /* the window with a field being typed in, or NULL */
	int page, index;
	folio_field f;
	int handle;              /* the editing window */
	int lines;
	char buf[MAX_LINES][LINE_LEN];
	char valid[32];
	/* The choices menu. */
	docwin *menu_w;
	int menu_page, menu_index, menu_n;
	char menu_text[32][64];
} fe;

/* The menu block (Wimp_CreateMenu): a header and up to 32 items. */
static int menu[7 + 6 * 32];

static int alerts_pending(docwin *w);

/* UTF-8 to the desktop's alphabet and back (clip.c), into a buffer. */
static void utf8_to_latin1(const char *utf8, char *out, size_t size)
{
	char *d = app_desktop_text(utf8);
	const char *src = d ? d : utf8;
	size_t n = strlen(src);
	if (n >= size)
		n = size - 1;
	memcpy(out, src, n);
	out[n] = 0;
	free(d);
}

static void latin1_to_utf8(const char *desktop, char *out, size_t size)
{
	char *u = app_utf8_text(desktop);
	const char *src = u ? u : desktop;
	size_t n = strlen(src);
	if (n >= size)
		n = size - 1;
	memcpy(out, src, n);
	out[n] = 0;
	free(u);
}

/* The field's box on the screen (OS units). */
static void field_screen_box(docwin *w, int page, const folio_rect *r, int box[4])
{
	int sx, sy;
	float k = 2.5f * docwin_zoom(w);
	docwin_page_origin(w, page, &sx, &sy);
	box[0] = sx + (int)(r->x0 * k);
	box[1] = sy - (int)(r->y1 * k);
	box[2] = sx + (int)(r->x1 * k + 0.5f);
	box[3] = sy - (int)(r->y0 * k);
}

static void close_editor(void)
{
	if (fe.handle) {
		int b[1] = { fe.handle };
		_swix(Wimp_DeleteWindow, _IN(1), b);
		fe.handle = 0;
	}
	fe.w = NULL;
}

/* Keeps what's typed, if it changed. */
static void commit_now(void);
/* Keeps what's typed (and says why, in the log). */
static void commit_why(const char *why)
{
	if (fe.w)
		app_log("form: '%s' closed: %s", fe.f.name, why);
	commit_now();
}
#define commit() commit_why(__func__)

static void commit_now(void)
{
	char value[MAX_LINES * LINE_LEN];
	docwin *w = fe.w;
	int i, rc;
	if (!w)
		return;
	value[0] = 0;
	for (i = 0; i < fe.lines; i++) {
		if (i)
			strcat(value, "\n");
		strcat(value, fe.buf[i]);
	}
	/* Trailing empty lines of a multi-line field aren't kept. */
	for (i = (int)strlen(value); i > 0 && value[i - 1] == '\n'; i--)
		value[i - 1] = 0;
	close_editor();
	{
		char utf8[sizeof value * 3];
		latin1_to_utf8(value, utf8, sizeof utf8);
		if (!strcmp(utf8, fe.f.value))
			return;
		rc = docwin_form_set_text(w, fe.page, fe.index, utf8);
	}
	if (rc == 1)
		tbx_report("The form didn't take that: it doesn't fit what this field expects.");
	alerts_pending(w);
}

void formui_forget(docwin *w)
{
	if (fe.w == w)
		close_editor();
	if (fe.menu_w == w)
		fe.menu_w = NULL;
}

void formui_commit(docwin *w)
{
	if (fe.w == w)
		commit();
}

int formui_owns(int handle)
{
	return fe.handle && handle == fe.handle;
}

/* Opens the editor over text field f on page. */
static void edit(docwin *w, int page, const folio_field *f)
{
	int box[4], wb[23 + 8 * MAX_LINES], h, lh, i, n;
	char latin[MAX_LINES * LINE_LEN];
	char *p, *line;
	close_editor();
	fe.w = w;
	fe.page = page;
	fe.index = f->index;
	fe.f = *f;
	field_screen_box(w, page, &f->rect, box);
	h = box[3] - box[1];
	if (h < 40)
		box[1] = box[3] - (h = 40);
	/* Lines: as many as fit, at the desktop font's height. */
	lh = 44;
	fe.lines = f->multiline ? h / lh : 1;
	if (fe.lines < 1)
		fe.lines = 1;
	if (fe.lines > MAX_LINES)
		fe.lines = MAX_LINES;
	if (!f->multiline)
		lh = h;
	/* The value, line by line (Latin-1 in the icons). */
	utf8_to_latin1(f->value, latin, sizeof latin);
	memset(fe.buf, 0, sizeof fe.buf);
	line = latin;
	for (i = 0; i < fe.lines && line; i++) {
		p = strchr(line, '\n');
		if (p)
			*p = 0;
		snprintf(fe.buf[i], LINE_LEN, "%.255s", line);
		line = p ? p + 1 : NULL;
	}
	/* K: tell us of Return, Tab and the arrows; D: password fields. */
	snprintf(fe.valid, sizeof fe.valid, "Ktar%s", f->password ? ";D*" : "");
	memset(wb, 0, sizeof wb);
	wb[0] = box[0];
	wb[1] = box[1];
	wb[2] = box[2];
	wb[3] = box[3];
	wb[4] = 0;                       /* scroll x */
	wb[5] = 0;                       /* scroll y */
	wb[6] = -1;                      /* behind: the top */
	wb[7] = 0x80000000u | (1u << 4); /* new format; the Wimp redraws it */
	wb[8] = 0x00070207;              /* title fg 7, bg 2 (no title); work fg 7, bg 0 */
	wb[9] = 0x000c0103;              /* scroll outer/inner, title focus */
	wb[10] = 0;                      /* work area: x0 */
	wb[11] = -(box[3] - box[1]);     /* y0 */
	wb[12] = box[2] - box[0];        /* x1 */
	wb[13] = 0;                      /* y1 */
	wb[14] = 0;                      /* title icon flags: none */
	wb[15] = 10 << 12;               /* work area button type: click */
	wb[16] = 1;                      /* sprite area: Wimp's */
	wb[17] = 0;                      /* min size */
	/* title data wb[18..20]: empty */
	n = fe.lines;
	wb[21] = n;
	for (i = 0; i < n; i++) {
		int *ic = &wb[22 + i * 8];
		int y1 = -i * lh, y0 = y1 - lh;
		ic[0] = 0;
		ic[1] = y0;
		ic[2] = box[2] - box[0];
		ic[3] = y1;
		/* text, vcentred, filled, indirected, writable; fg 7 bg 0 */
		ic[4] = 1 | (1 << 4) | (1 << 5) | (1 << 8) | (15 << 12) | (7 << 24) | (0 << 28);
		ic[5] = (int)fe.buf[i];
		ic[6] = (int)fe.valid;
		ic[7] = f->max_len > 0 && f->max_len < LINE_LEN ? f->max_len + 1 : LINE_LEN;
	}
	{
		_kernel_oserror *e = _swix(Wimp_CreateWindow, _IN(1) | _OUT(0), wb, &fe.handle);
		if (e) {
			app_log("form: can't make the editing window: %s", e->errmess);
			fe.handle = 0;
			fe.w = NULL;
			return;
		}
	}
	/* Over the field, in front. (A window of its own on top, not one inside
	 * the document window: test41's nested one never took the caret. If the
	 * document window moves or scrolls, what's typed is kept and it goes.) */
	{
		_kernel_oserror *e;
		int ob[8];
		ob[0] = fe.handle;
		ob[1] = box[0];
		ob[2] = box[1];
		ob[3] = box[2];
		ob[4] = box[3];
		ob[5] = 0;
		ob[6] = 0;
		ob[7] = -1;
		e = _swix(Wimp_OpenWindow, _IN(1), ob);
		if (e)
			app_log("form: can't open the editing window: %s", e->errmess);
	}
	{
		_kernel_oserror *e = _swix(Wimp_SetCaretPosition, _INR(0, 5), fe.handle, 0, 0, 0, -1,
			(int)strlen(fe.buf[0]));
		if (e)
			app_log("form: can't put the caret in it: %s", e->errmess);
	}
	app_log("form: editing '%s' on page %d (window &%x at %d,%d %d,%d, %d line%s)", f->name,
		page + 1, fe.handle, box[0], box[1], box[2], box[3], fe.lines, fe.lines == 1 ? "" : "s");
}

/* The next (or previous) text field on the page after index, or -1. */
static int next_text_field(docwin *w, int page, int index, int dir, folio_field *out)
{
	folio_doc *doc = docwin_doc(w);
	folio_loc loc = { 0, page };
	int n, i, k;
	core_now();
	n = folio_field_count(doc, loc);
	for (k = 1; k <= n; k++) {
		i = ((index + dir * k) % n + n) % n;
		if (folio_field_get(doc, loc, i, out) == 1 && out->kind == FOLIO_FIELD_TEXT && !out->read_only)
			break;
	}
	core_unlock();
	return k <= n ? out->index : -1;
}

/* Keys in the editor. 1 if taken. */
int formui_key(int *block)
{
	int key = block[6], icon = block[1];
	docwin *w = fe.w;
	int page = fe.page, index = fe.index;
	if (!fe.handle || block[0] != fe.handle)
		return 0;
	app_log("form: key &%x in icon %d", key, icon);
	switch (key) {
	case 0x1b:                        /* Escape: as it was */
		app_log("form: '%s' closed: Escape", fe.f.name);
		close_editor();
		return 1;
	case 0x0d:                        /* Return */
		if (fe.lines > 1 && icon < fe.lines - 1) {
			_swix(Wimp_SetCaretPosition, _INR(0, 5), fe.handle, icon + 1, 0, 0, -1, 0);
			return 1;
		}
		commit();
		return 1;
	case 0x18e:                       /* Down */
		if (icon < fe.lines - 1)
			_swix(Wimp_SetCaretPosition, _INR(0, 5), fe.handle, icon + 1, 0, 0, -1,
				(int)strlen(fe.buf[icon + 1]));
		return 1;
	case 0x18f:                       /* Up */
		if (icon > 0)
			_swix(Wimp_SetCaretPosition, _INR(0, 5), fe.handle, icon - 1, 0, 0, -1,
				(int)strlen(fe.buf[icon - 1]));
		return 1;
	case 0x18a:                       /* Tab, Shift-Tab: keep it, then the next */
	case 0x19a: {
		folio_field nf;
		commit();
		if (is_docwin(w) && next_text_field(w, page, index, key == 0x19a ? -1 : 1, &nf) >= 0)
			edit(w, page, &nf);
		return 1;
	}
	}
	_swix(Wimp_ProcessKey, _IN(0), key);
	return 1;
}

/* The caret has gone elsewhere: keep what's typed. */
void formui_lose_caret(int handle)
{
	int caret[6];
	if (!fe.handle || handle != fe.handle)
		return;
	/* A late word about an editor since closed (the handle can be reused
	 * by the next one): only if the caret really isn't in it. */
	_swix(Wimp_GetCaretPosition, _IN(1), caret);
	if (caret[0] == fe.handle)
		return;
	/* Taken by the document window itself (not by a click on it, which
	 * keeps the field first): put it back. */
	if (fe.w && caret[0] == docwin_wimp(fe.w)) {
		app_log("form: the caret went to the document window: put back");
		formui_raise(fe.w);
		_swix(Wimp_SetCaretPosition, _INR(0, 5), fe.handle, 0, 0, 0, -1, (int)strlen(fe.buf[0]));
		return;
	}
	commit();
}

/* The document window came to the front: the editor goes in front of it. */
void formui_raise(docwin *w)
{
	int st[9];
	if (fe.w != w || !fe.handle)
		return;
	st[0] = fe.handle;
	if (_swix(Wimp_GetWindowState, _IN(1), st))
		return;
	st[7] = -1;
	_swix(Wimp_OpenWindow, _IN(1), st);
}

/* ---- Choices menu ----------------------------------------------------------- */

static void show_menu(docwin *w, int page, const folio_field *f, int x, int y)
{
	folio_doc *doc = docwin_doc(w);
	folio_loc loc = { 0, page };
	int i, n = f->options > 32 ? 32 : f->options, width = 0;
	char title[13];
	fe.menu_w = w;
	fe.menu_page = page;
	fe.menu_index = f->index;
	fe.menu_n = n;
	core_now();
	for (i = 0; i < n; i++) {
		char u[200];
		if (folio_field_option(doc, loc, f->index, i, u, sizeof u))
			u[0] = 0;
		utf8_to_latin1(u, fe.menu_text[i], sizeof fe.menu_text[i]);
		if ((int)strlen(fe.menu_text[i]) > width)
			width = (int)strlen(fe.menu_text[i]);
	}
	core_unlock();
	if (n == 0)
		return;
	memset(menu, 0, sizeof menu);
	{
		/* The title: the field's own name (its last part), up to 12 characters. */
		const char *t = strrchr(f->name, '.');
		snprintf(title, sizeof title, "%.12s", t ? t + 1 : f->name[0] ? f->name : "Choose");
		memcpy(menu, title, strlen(title) < 12 ? strlen(title) + 1 : 12);
	}
	((unsigned char *)menu)[12] = 7;    /* title fg */
	((unsigned char *)menu)[13] = 2;    /* title bg */
	((unsigned char *)menu)[14] = 7;    /* work fg */
	((unsigned char *)menu)[15] = 0;    /* work bg */
	menu[4] = (width + 2) * 16;
	menu[5] = 44;
	menu[6] = 0;
	for (i = 0; i < n; i++) {
		int *it = &menu[7 + i * 6];
		char u[200];
		it[0] = (i == n - 1 ? 0x80 : 0);
		utf8_to_latin1(f->value, u, sizeof u);
		if (!strcmp(u, fe.menu_text[i]))
			it[0] |= 1;                 /* ticked: the current choice */
		it[1] = -1;
		it[2] = 1 | (1 << 8) | (7 << 24) | (0 << 28);   /* text, indirected */
		it[3] = (int)fe.menu_text[i];
		it[4] = -1;
		it[5] = (int)strlen(fe.menu_text[i]) + 1;
	}
	/* Any other menu open now goes (and with it its MenusDeleted). */
	_swix(Wimp_CreateMenu, _INR(1, 3), menu, x - 64, y);
}

/* Message_MenusDeleted: our menu has gone without a choice. (Wimp_EMenu-
 * Selection comes for the Toolbox's menus too, so it's taken as ours only
 * while ours is open.) */
void formui_menus_deleted(int tree)
{
	if (tree == (int)menu)
		fe.menu_w = NULL;
}

/* Wimp_EMenuSelection for the choices menu: 1 if it was ours. */
int formui_menu_selection(int *block)
{
	docwin *w = fe.menu_w;
	if (!w || !is_docwin(w))
		return 0;
	fe.menu_w = NULL;
	if (block[0] < 0 || block[0] >= fe.menu_n)
		return 1;
	docwin_form_choose(w, fe.menu_page, fe.menu_index, block[0]);
	alerts_pending(w);
	return 1;
}

/* ---- Clicks ------------------------------------------------------------------ */

/* A Select click at p on page: 1 if it was on a field (and was dealt with). */
int formui_click(docwin *w, int page, folio_point p, int x, int y)
{
	folio_field f;
	folio_loc loc = { 0, page };
	int found;
	if (fe.w)
		commit();
	if (!is_docwin(w))
		return 0;
	core_now();
	found = folio_field_at(docwin_doc(w), loc, p, &f);
	core_unlock();
	if (found != 1)
		return 0;
	if (f.read_only) {
		app_log("form: '%s' can't be changed", f.name);
		return 1;
	}
	if (!docwin_allows(w, EV_FILL_FORM)) {
		docwin_ask_permission(w, EV_FILL_FORM);
		return 1;
	}
	switch (f.kind) {
	case FOLIO_FIELD_TEXT:
		edit(w, page, &f);
		break;
	case FOLIO_FIELD_CHECK:
	case FOLIO_FIELD_RADIO:
		docwin_form_toggle(w, page, f.index);
		alerts_pending(w);
		break;
	case FOLIO_FIELD_COMBO:
	case FOLIO_FIELD_LIST:
		show_menu(w, page, &f, x, y);
		break;
	case FOLIO_FIELD_BUTTON:
		if (!app.ch.run_js)
			tbx_report("This button needs the form's JavaScript, which is turned off. "
				"Choices... on Folio's icon bar menu can turn it on.");
		else if (docwin_form_press(w, page, f.index) == 0)
			app_log("form: '%s' has nothing to do", f.name);
		alerts_pending(w);
		break;
	case FOLIO_FIELD_SIGNATURE:
		tbx_report("Folio can't sign a PDF or change a signature.");
		break;
	default:
		return 0;
	}
	return 1;
}

/* What the form's script asked for: an alert is shown, and a web page is
 * opened only if the reader agrees (as for links). */
static int alerts_pending(docwin *w)
{
	char buf[512];
	int any = 0;
	folio_doc *doc;
	if (!is_docwin(w))
		return 0;
	doc = docwin_doc(w);
	if (folio_form_take_alert(doc, buf, sizeof buf)) {
		char latin[512];
		utf8_to_latin1(buf, latin, sizeof latin);
		tbx_report(latin);
		any = 1;
	}
	if (folio_form_take_url(doc, buf, sizeof buf)) {
		docwin_follow_uri(w, buf, 1);
		any = 1;
	}
	return any;
}
