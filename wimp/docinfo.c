/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Document info (the document menu's "Document info..."), like a PDF
 * reader's Properties box: the file, its title, author, dates, version,
 * page size, security and what the author allows, and what's inside it,
 * down to the pictures and every font it uses (and whether each is held in
 * the file, "embedded").
 * A plain window of "label  value" rows drawn with Wimp_TextOp; values too
 * long for it end in "...". One window, for the document it was last
 * opened from. */
#include "app.h"
#include "worker.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define ROW_H    44
#define PAD      12
#define WIDTH    1240
#define LABEL_W  420        /* label column, right-justified */
#define MAX_ROWS (FOLIO_INFO_MAX + FOLIO_FONTS_MAX + 8)

static struct {
	ObjectId obj;
	int wimp;
	docwin *w;
	int n;
	char label[MAX_ROWS][40];
	char value[MAX_ROWS][300];   /* desktop alphabet, cut to fit */
	char title[200];
} di;

static ObjectId create_window(void)
{
	static tbx_window t;
	ObjectId id = 0;
	_kernel_oserror *e;
	memset(&t, 0, sizeof t);
	t.flags = (1 << 1) | (1 << 2);   /* the Toolbox opens and closes it */
	t.help_message = "This shows the document's properties.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.default_focus = -1;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = WIDTH;
	t.window.vis_y1 = 400;
	t.window.behind = -1;
	/* Moveable, back, close, title, vertical scroll; not auto-redraw. */
	t.window.window_flags = 0x80000002u | (1u << 24) | (1u << 25) | (1u << 26) | (1u << 28);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 1;          /* light grey, as dialogues */
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -400;
	t.window.work_x1 = WIDTH;
	t.window.title_flags = 0x00000119;
	t.window.work_flags = 0;
	t.window.sprite_area = (void *)1;
	t.window.title_text = di.title;
	t.window.title_validation = "";
	t.window.title_buflen = sizeof di.title;
	strcpy(di.title, "Document info");
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	window_get_wimp_handle(id, &di.wimp);
	return id;
}

static int text_width(const char *s)
{
	int w = 0;
	if (_swix(Wimp_TextOp, _INR(0, 2) | _OUT(0), 1, s, 0, &w))
		return (int)strlen(s) * 16;
	return w;
}

/* Cuts s (desktop alphabet) to fit width, ending "..." if cut. */
static void fit(char *s, int width)
{
	size_t n = strlen(s);
	if (text_width(s) <= width)
		return;
	while (n > 0) {
		n--;
		s[n] = 0;
		if (n >= 3 || n == 0) {
			char save[4];
			memcpy(save, s + (n >= 3 ? n - 3 : 0), 4);
			if (n >= 3)
				strcpy(s + n - 3, "...");
			if (text_width(s) <= width)
				return;
			if (n >= 3)
				memcpy(s + n - 3, save, 4);
		}
	}
}

static void add(const char *label, const char *utf8)
{
	char *d;
	if (di.n >= MAX_ROWS)
		return;
	snprintf(di.label[di.n], sizeof di.label[0], "%s", label);
	d = app_desktop_text(utf8);
	snprintf(di.value[di.n], sizeof di.value[0], "%s", d ? d : utf8);
	free(d);
	/* Control characters (a title with a newline) as spaces. */
	{
		char *p;
		for (p = di.value[di.n]; *p; p++)
			if ((unsigned char)*p < 32)
				*p = ' ';
	}
	fit(di.value[di.n], WIDTH - LABEL_W - 48);
	di.n++;
}

void docinfo_open(docwin *w)
{
	folio_info in;
	folio_fonts *fonts = malloc(sizeof *fonts);
	const char *path = docwin_path(w), *leaf, *dot, *colon;
	char buf[300];
	int st[9], dst[9], h, i, type = 0, len = 0, box[4];
	if (!di.obj && !(di.obj = create_window()))
		return;
	_swix(Hourglass_On, 0);
	core_now();
	folio_get_info(docwin_doc(w), &in);
	/* A few seconds at most: a 2000-page PDF says how far it got. */
	if (fonts && folio_get_fonts(docwin_doc(w), fonts, 4) < 0) {
		free(fonts);
		fonts = NULL;
	}
	core_unlock();
	_swix(Hourglass_Off, 0);
	di.w = w;
	di.n = 0;
	dot = strrchr(path, '.');
	colon = strrchr(path, ':');
	leaf = dot > colon ? dot + 1 : colon ? colon + 1 : path;
	/* The file: its name, where it is, and its size. */
	add("File", leaf);
	snprintf(buf, sizeof buf, "%.*s", (int)(leaf - path > 1 ? leaf - path - 1 : 0), path);
	if (buf[0])
		add("In", buf);
	if (!_swix(OS_File, _INR(0, 1) | _OUT(0) | _OUT(4), 17, path, &type, &len) && type == 1) {
		if (len >= 1 << 20)
			snprintf(buf, sizeof buf, "%.1f MB (%d bytes)", len / 1048576.0, len);
		else
			snprintf(buf, sizeof buf, "%d KB (%d bytes)", (len + 1023) / 1024, len);
		add("Size", buf);
	}
	for (i = 0; i < in.n; i++)
		add(in.item[i].label, in.item[i].value);
	if (fonts) {
		int missing = 0, len2;
		snprintf(buf, sizeof buf, "%d", fonts->pictures);
		add("Pictures", fonts->pictures ? buf : "None");
		for (i = 0; i < fonts->n; i++)
			missing += fonts->font[i].embedded == 0;
		if (!fonts->n)
			snprintf(buf, sizeof buf, "None");
		else if (missing)
			snprintf(buf, sizeof buf, "%d, %d not embedded", fonts->n + fonts->more, missing);
		else
			snprintf(buf, sizeof buf, "%d, all embedded", fonts->n + fonts->more);
		len2 = (int)strlen(buf);
		if (fonts->pages_done < fonts->pages)
			snprintf(buf + len2, sizeof buf - len2, " (on the first %d of %d pages)",
				fonts->pages_done, fonts->pages);
		add("Fonts", buf);
		for (i = 0; i < fonts->n; i++) {
			static const char *how[] = { "not embedded", "embedded", "embedded subset", "" };
			folio_font *f = &fonts->font[i];
			if (f->embedded == 3)
				snprintf(buf, sizeof buf, "%s: %s", f->name, f->type);
			else
				snprintf(buf, sizeof buf, "%s: %s, %s", f->name, f->type, how[f->embedded & 3]);
			add("", buf);
		}
		if (fonts->more) {
			snprintf(buf, sizeof buf, "and %d more", fonts->more);
			add("", buf);
		}
		free(fonts);
	}
	snprintf(di.title, sizeof di.title, "Document info: %.150s", leaf);
	window_set_title(di.obj, di.title);
	h = PAD * 2 + di.n * ROW_H;
	box[0] = 0;
	box[1] = -h;
	box[2] = WIDTH;
	box[3] = 0;
	_swix(Wimp_SetExtent, _INR(0, 1), di.wimp, box);
	if (h > app.screen_h - 200)
		h = app.screen_h - 200;
	/* Over the document window, near its top left. */
	dst[0] = docwin_wimp(w);
	_swix(Wimp_GetWindowState, _IN(1), dst);
	st[0] = di.wimp;
	st[1] = dst[1] + 64;
	st[4] = dst[4] - 64;
	st[3] = st[1] + WIDTH;
	st[2] = st[4] - h;
	if (st[2] < 64) {
		st[4] += 64 - st[2];
		st[2] = 64;
	}
	st[5] = 0;
	st[6] = 0;
	st[7] = -1;
	tbx_show(di.obj, 0, 1, st + 1, 0, -1);
	_swix(Wimp_ForceRedraw, _INR(0, 4), di.wimp, 0, -(PAD * 2 + di.n * ROW_H), WIDTH, 0);
}

void docinfo_forget(docwin *w)
{
	if (di.w != w)
		return;
	di.w = NULL;
	if (di.obj)
		tbx_hide(di.obj);
}

int docinfo_owns(int wimp_handle)
{
	return di.obj && wimp_handle == di.wimp;
}

static void text_at(const char *s, int x, int y, int fg, int right)
{
	_swix(Wimp_TextOp, _INR(0, 2), 0, fg, 0xdddddd00);
	_swix(Wimp_TextOp, _INR(0, 5), 2 | (1u << 28) | (right ? (1u << 31) : 0), s, -1, -1, x, y);
}

void docinfo_redraw(int *b)
{
	int more = 0;
	b[0] = di.wimp;
	_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), b, &more);
	while (more) {
		int ox = b[1] - b[5], oy = b[4] - b[6], i;
		for (i = 0; i < di.n; i++) {
			int yc = oy - PAD - i * ROW_H - ROW_H / 2;
			text_at(di.label[i], ox + LABEL_W, yc, 0x55555500, 1);
			text_at(di.value[i], ox + LABEL_W + 24, yc, 0x00000000, 0);
		}
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}
