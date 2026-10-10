/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Find (F4, as in Edit; Style Guide p60): a persistent dialogue with the
 * text to find, a status line, and Cancel, Previous and Next (the default,
 * so Return finds the next one). Searching is case-insensitive.
 *
 * The search runs a page at a time in null polls, taking the core lock only
 * when the page worker isn't using it, so the desktop and page drawing keep
 * going in a long book. A hit becomes the window's selection (so Copy and
 * Define work on it) and is scrolled into view. The search wraps round the
 * end of the document once, and stops where it began. */
#include "app.h"
#include "worker.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define MAX_QUADS 512
#define SLICE_CS  4

enum { G_LABEL = 0, G_TEXT = 1, G_STATUS = 2, G_CANCEL = 3, G_PREV = 4, G_NEXT = 5 };

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

/* All the gadgets, one after another, as the Window template wants them. */
static struct {
	label_gadget label;
	writable_gadget text;
	display_gadget status;
	button_gadget cancel, prev, next;
} g;

static struct {
	ObjectId obj;
	docwin *w;
	char query[128];        /* UTF-8 as typed (the desktop's alphabet) */
	int searching;
	int dir;                /* +1 forwards, -1 backwards */
	int page;               /* page being searched */
	int first;              /* the first page searched: back there after wrapping = done */
	int wrapped;
	int new_query;
	/* Hits on hit_page. */
	int hit_page, hit, nhits;
	folio_quad q[MAX_QUADS];
	int mark[MAX_QUADS];
	int nq;
} fs;

static char text_buf[128], status_buf[100];

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

static void button(button_gadget *b, int id, int flags, int x0, const char *text,
		int event, const char *help)
{
	hdr(&b->h, 128, sizeof *b, id, x0, -216, x0 + 176, -148, help);
	b->h.flags = flags;
	b->text = text;
	b->max_text = (int)strlen(text) + 1;
	b->event = event;
}

static ObjectId create_window(void)
{
	static tbx_window t;
	static char title[64] = "Find";
	ObjectId id = 0;
	_kernel_oserror *e;

	memset(&g, 0, sizeof g);
	hdr(&g.label.h, 320, sizeof g.label, G_LABEL, 16, -84, 120, -36, NULL);
	g.label.label = "Find";
	hdr(&g.text.h, 512, sizeof g.text, G_TEXT, 120, -88, 612, -32,
		"Type the text to find. Capitals don't matter.|MPress Return to find the next one.");
	g.text.text = text_buf;
	g.text.max_text = sizeof text_buf;
	g.text.before = -1;
	g.text.after = -1;
	hdr(&g.status.h, 448, sizeof g.status, G_STATUS, 16, -136, 612, -96,
		"This says where the text was found.");
	g.status.text = status_buf;
	g.status.max_text = sizeof status_buf;
	/* Bottom row: Cancel at the left; the default action at the right (SG p55). */
	button(&g.cancel, G_CANCEL, 2 /* Cancel: Escape */, 16, "Cancel", EV_FIND_CANCEL,
		"Click to close this box.");
	button(&g.prev, G_PREV, 4 /* local */, 252, "Previous", EV_FIND_PREV,
		"Click to find the text before this place.");
	button(&g.next, G_NEXT, 1 | 4 /* default, local */, 436, "Next", EV_FIND_NEXT,
		"Click (or press Return) to find the next place the text appears.");

	memset(&t, 0, sizeof t);
	t.flags = (1 << 1) | (1 << 2);   /* the Toolbox opens and closes it */
	t.help_message = "Use this box to find text in the document.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.num_gadgets = 6;
	t.gadgets = &g;
	t.default_focus = G_TEXT;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = 628;
	t.window.vis_y1 = 232;
	t.window.behind = -1;
	/* Moveable, auto-redraw, title; no close icon (a dialogue: Cancel). */
	t.window.window_flags = 0x80000002u | (1u << 4) | (1u << 26);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 1;
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -232;
	t.window.work_x1 = 628;
	t.window.title_flags = 0x00000119;
	t.window.work_flags = 0;
	t.window.sprite_area = (void *)1;
	t.window.title_text = title;
	t.window.title_validation = "";
	t.window.title_buflen = sizeof title;
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	return id;
}

static void status(const char *s)
{
	tbx_method(fs.obj, 448 /* DisplayField_SetValue */, G_STATUS, (int)s, 0);
}

void find_open(docwin *w)
{
	int dst[9], pos[2];
	if (!fs.obj && !(fs.obj = create_window()))
		return;
	if (fs.w != w) {
		fs.searching = 0;
		fs.nhits = 0;
		fs.hit_page = -1;
		fs.new_query = 1;
		status("");
	}
	fs.w = w;
	/* Near the top right of the document window. */
	dst[0] = docwin_wimp(w);
	_swix(Wimp_GetWindowState, _IN(1), dst);
	pos[0] = dst[3] - 628 - 64;
	if (pos[0] < dst[1])
		pos[0] = dst[1];
	pos[1] = dst[4] - 64;
	tbx_show(fs.obj, 0, 2, pos, 0, -1);
}

void find_forget(docwin *w)
{
	if (fs.w != w)
		return;
	fs.w = NULL;
	fs.searching = 0;
	fs.nhits = 0;
	if (fs.obj)
		tbx_hide(fs.obj);
}

/* Where hit h starts in fs.q, and how many quads it has. */
static int hit_quads(int h, int *first)
{
	int i, k = -1, n = 0;
	for (i = 0; i < fs.nq; i++) {
		if (fs.mark[i])
			k++;
		if (k == h) {
			if (n == 0)
				*first = i;
			n++;
		} else if (k > h) {
			break;
		}
	}
	return n;
}

static void show_hit(void)
{
	int first = 0, n = hit_quads(fs.hit, &first);
	char msg[100];
	if (n <= 0)
		return;
	docwin_show_hit(fs.w, fs.hit_page, fs.q + first, n);
	if (fs.nhits > 1)
		snprintf(msg, sizeof msg, "Page %d: %d of %d on this page%s", fs.hit_page + 1,
			fs.hit + 1, fs.nhits, fs.wrapped ? " (wrapped round)" : "");
	else
		snprintf(msg, sizeof msg, "Page %d%s", fs.hit_page + 1, fs.wrapped ? " (wrapped round)" : "");
	status(msg);
}

/* Next or Previous. */
static void go(int dir)
{
	char text[128];
	if (!fs.w)
		return;
	text[0] = 0;
	/* WritableField_GetValue: R4 buffer, R5 its size. */
	_swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, fs.obj, 513, G_TEXT, text, (int)sizeof text);
	text[sizeof text - 1] = 0;
	if (!text[0]) {
		status("Type the text to find first.");
		return;
	}
	if (strcmp(text, fs.query)) {
		snprintf(fs.query, sizeof fs.query, "%s", text);
		fs.new_query = 1;
	}
	fs.dir = dir;
	fs.wrapped = 0;
	if (!fs.new_query && fs.nhits > 0) {
		/* Another hit on the same page? */
		if (fs.hit + dir >= 0 && fs.hit + dir < fs.nhits) {
			fs.hit += dir;
			show_hit();
			return;
		}
		/* On from the hit's page; coming back round to it finds its
		 * other hits (or the same one) again. */
		fs.page = fs.hit_page + dir;
	} else {
		/* From the page being read, including it. */
		fs.page = docwin_current_page(fs.w);
	}
	fs.first = fs.page;
	fs.new_query = 0;
	fs.nhits = 0;
	fs.searching = 1;
	status("Searching...");
}

void find_event(int code)
{
	switch (code) {
	case EV_FIND_NEXT: go(1); break;
	case EV_FIND_PREV: go(-1); break;
	case EV_FIND_CANCEL: fs.searching = 0; break;   /* the button closes the box */
	}
}

/* Searches one page (core lock held). Returns 1 if it has hits. */
static int search_page(folio_doc *doc, int page)
{
	folio_loc loc;
	int i, n;
	if (folio_loc_from_number(doc, page, &loc))
		return 0;
	n = folio_find_on_page(doc, loc, fs.query, fs.q, fs.mark, MAX_QUADS);
	if (n <= 0)
		return 0;
	fs.nq = n;
	fs.nhits = 0;
	for (i = 0; i < n; i++)
		if (fs.mark[i])
			fs.nhits++;
	fs.hit_page = page;
	fs.hit = fs.dir > 0 ? 0 : fs.nhits - 1;
	return fs.nhits > 0;
}

int find_background(void)
{
	folio_doc *doc;
	unsigned t0, t;
	int found = 0, exact = 0, count;
	char msg[64];
	if (!fs.searching || !fs.w)
		return 0;
	if (!core_trylock())
		return 1;
	doc = docwin_doc(fs.w);
	_swix(OS_ReadMonotonicTime, _OUT(0), &t0);
	for (;;) {
		count = folio_page_count(doc, &exact);
		if (fs.page >= count || fs.page < 0) {
			if (!exact)
				break;              /* not counted that far yet: wait */
			if (fs.wrapped) {       /* round once already */
				fs.searching = 0;
				break;
			}
			fs.page = fs.page < 0 ? count - 1 : 0;
			fs.wrapped = 1;
			if (fs.first >= count)
				fs.first = 0;
			else if (fs.first < 0)
				fs.first = count - 1;
		} else if (fs.wrapped && fs.page == fs.first) {
			fs.searching = 0;       /* back where it started */
			break;
		}
		if (search_page(doc, fs.page)) {
			found = 1;
			break;
		}
		fs.page += fs.dir;
		_swix(OS_ReadMonotonicTime, _OUT(0), &t);
		if (t - t0 >= SLICE_CS)
			break;
	}
	core_unlock();
	if (found) {
		fs.searching = 0;
		show_hit();
		app_log("find '%s': page %d, %d hit(s)", fs.query, fs.hit_page + 1, fs.nhits);
	} else if (!fs.searching) {
		status("Not found.");
		fs.nhits = 0;
	} else {
		snprintf(msg, sizeof msg, "Searching page %d...", fs.page + 1);
		status(msg);
	}
	return fs.searching;
}
