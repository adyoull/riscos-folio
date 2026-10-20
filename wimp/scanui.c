/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Scan... (icon bar menu): scanning from a network scanner with eSCL
 * ("AirScan"; core/escl.c), no driver needed. The window has the scanner's
 * address (typed, or found with Find, which asks the local network by
 * mDNS), the glass or the feeder (both sides), colour or grey, 150, 300 or
 * 600 dpi, and A4, Letter or the largest the scanner can. Scan fetches the
 * pages a step at a time on null polls (the desktop carries on), into the
 * scrap directory, and makes them a new PDF (the JPEGs as the scanner sent
 * them), opened unsaved as "Scan". The address is kept in
 * <Choices$Write>.Folio.Scanner. */
#include "app.h"
#include "escl.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

enum {
	S_ADDR_L = 0, S_ADDR, S_FIND, S_FOUND,
	S_SRC_L, S_GLASS, S_FEEDER, S_DUPLEX,
	S_COL_L, S_COLOUR, S_GREY,
	S_RES_L, S_R150, S_R300, S_R600,
	S_SIZE_L, S_A4, S_LETTER, S_LARGEST,
	S_STATUS, S_STOP, S_SCAN, S_OCR
};

typedef struct {
	int flags, type;
	int x0, y0, x1, y1;
	ComponentId id;
	const char *help;
	int max_help;
} gadget_hdr;

typedef struct { gadget_hdr h; const char *label; } label_gadget;
typedef struct { gadget_hdr h; const char *label; int max_label; int event; } option_gadget;
typedef struct { gadget_hdr h; int group; const char *label; int max_label; int event; } radio_gadget;
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

/* In component order. */
static struct {
	label_gadget addr_l;
	writable_gadget addr;
	button_gadget find;
	display_gadget found;
	label_gadget src_l;
	radio_gadget glass, feeder;
	option_gadget duplex;
	label_gadget col_l;
	radio_gadget colour, grey;
	label_gadget res_l;
	radio_gadget r150, r300, r600;
	label_gadget size_l;
	radio_gadget a4, letter, largest;
	display_gadget status;
	button_gadget stop, scan;
	option_gadget ocr;
} g;

#define BOX_W 920
#define BOX_H 648
#define COL   200           /* where the choices start, after the labels */

static struct {
	ObjectId obj;
	char addr_buf[120], found_buf[160], status_buf[160];
	escl_find *find;
	escl_found found[32];
	int nfound, cur;
	escl_job *job;
	char dir[300];          /* where this scan's pages go */
	char **paths;
	int npages, all_images;
} sc;

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

static void label(label_gadget *l, int id, int y1, const char *text)
{
	hdr(&l->h, 320, sizeof *l, id, 16, y1 - 48, COL - 8, y1, NULL);
	l->label = text;
}

static void radio(radio_gadget *r, int id, int group, int x0, int y1, int x1, const char *text,
		const char *help)
{
	hdr(&r->h, 384, sizeof *r, id, x0, y1 - 48, x1, y1, help);
	r->group = group;
	r->label = text;
	r->max_label = (int)strlen(text) + 1;
	r->event = 0;
}

static void button(button_gadget *b, int id, int flags, int x0, int y1, int x1, const char *text,
		int event, const char *help)
{
	hdr(&b->h, 128, sizeof *b, id, x0, y1 - 68, x1, y1, help);
	b->h.flags = flags;
	b->text = text;
	b->max_text = (int)strlen(text) + 1;
	b->event = event;
}

static void display(display_gadget *d, int id, int x0, int y1, int x1, char *buf, int size,
		const char *help)
{
	hdr(&d->h, 448, sizeof *d, id, x0, y1 - 52, x1, y1, help);
	d->text = buf;
	d->max_text = size;
}

static ObjectId create_window(void)
{
	static tbx_window t;
	static char title[32] = "Scan";
	ObjectId id = 0;
	_kernel_oserror *e;
	int y = -28;

	memset(&g, 0, sizeof g);
	hdr(&g.addr_l.h, 320, sizeof g.addr_l, S_ADDR_L, 16, y - 52, COL - 8, y - 4, NULL);
	g.addr_l.label = "Scanner";
	hdr(&g.addr.h, 512, sizeof g.addr, S_ADDR, COL, y - 56, BOX_W - 200, y,
		"Type the scanner's network address, such as 192.168.0.40, or click Find.|MA port or path can follow: "
		"192.168.0.40:8080/eSCL.");
	g.addr.text = sc.addr_buf;
	g.addr.max_text = sizeof sc.addr_buf;
	g.addr.allowable = NULL;
	g.addr.max_allowable = 0;
	g.addr.before = -1;
	g.addr.after = -1;
	button(&g.find, S_FIND, 1u << 2, BOX_W - 184, y, BOX_W - 24, "Find", EV_SC_FIND,
		"Click to look for scanners on the network (eSCL, AirScan). If it finds several, click again for the next.");
	y -= 68;
	display(&g.found, S_FOUND, COL, y, BOX_W - 24, sc.found_buf, sizeof sc.found_buf,
		"This is the scanner Find found, or what it's doing.");
	y -= 76;
	label(&g.src_l, S_SRC_L, y, "Paper");
	radio(&g.glass, S_GLASS, 1, COL, y, COL + 200, "Glass", "Click to scan one page from the glass.");
	radio(&g.feeder, S_FEEDER, 1, COL + 216, y, COL + 440, "Feeder",
		"Click to scan every page in the document feeder.");
	hdr(&g.duplex.h, 192, sizeof g.duplex, S_DUPLEX, COL + 456, y - 48, BOX_W - 24, y,
		"Click to scan both sides of each page in the feeder (if the scanner can).");
	g.duplex.label = "Both sides";
	g.duplex.max_label = 11;
	y -= 64;
	label(&g.col_l, S_COL_L, y, "Colour");
	radio(&g.colour, S_COLOUR, 2, COL, y, COL + 200, "Colour", "Click to scan in colour.");
	radio(&g.grey, S_GREY, 2, COL + 216, y, COL + 440, "Grey", "Click to scan in shades of grey (smaller files).");
	y -= 64;
	label(&g.res_l, S_RES_L, y, "Resolution");
	radio(&g.r150, S_R150, 3, COL, y, COL + 200, "150 dpi", "Click for 150 dpi: quick, fine for reading on screen.");
	radio(&g.r300, S_R300, 3, COL + 216, y, COL + 440, "300 dpi", "Click for 300 dpi: good for text recognition and printing.");
	radio(&g.r600, S_R600, 3, COL + 456, y, BOX_W - 24, "600 dpi", "Click for 600 dpi: fine detail, large files.");
	y -= 64;
	label(&g.size_l, S_SIZE_L, y, "Size");
	radio(&g.a4, S_A4, 4, COL, y, COL + 200, "A4", "Click to scan an A4 area.");
	radio(&g.letter, S_LETTER, 4, COL + 216, y, COL + 440, "Letter", "Click to scan a US Letter area.");
	radio(&g.largest, S_LARGEST, 4, COL + 456, y, BOX_W - 24, "Largest",
		"Click to scan the largest area the scanner can (for long pages in the feeder).");
	y -= 64;
	hdr(&g.ocr.h, 192, sizeof g.ocr, S_OCR, COL, y - 48, BOX_W - 24, y,
		"Click to have the pages' text recognised as soon as they open (with the OCR plugin), so it can be "
		"found, selected, copied and read aloud.");
	g.ocr.label = "Recognise the text";
	g.ocr.max_label = 19;
	y -= 76;
	display(&g.status, S_STATUS, 16, y, BOX_W - 24, sc.status_buf, sizeof sc.status_buf,
		"This shows what the scan is doing.");
	button(&g.stop, S_STOP, 1u << 2, BOX_W - 392, -BOX_H + 92, BOX_W - 216, "Stop", EV_SC_STOP,
		"Click to stop looking for scanners, or to stop the scan (the pages so far are kept).");
	button(&g.scan, S_SCAN, 1 | (1u << 2), BOX_W - 200, -BOX_H + 92, BOX_W - 24, "Scan", EV_SC_SCAN,
		"Click (or press Return) to scan. The pages open as a new PDF, to look at and save.");

	memset(&t, 0, sizeof t);
	t.flags = (1 << 1) | (1 << 2);   /* the Toolbox opens and closes it */
	t.help_message = "This window scans from a network scanner (eSCL, AirScan), with no driver.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.num_gadgets = S_OCR + 1;
	t.gadgets = &g;
	t.default_focus = S_ADDR;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = BOX_W;
	t.window.vis_y1 = BOX_H;
	t.window.behind = -1;
	t.window.window_flags = 0x80000002u | (1u << 24) | (1u << 25) | (1u << 26);
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
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	return id;
}

static void set_text(int comp, const char *s)
{
	if (sc.obj)
		tbx_method(sc.obj, comp == S_ADDR ? 512 : 448, comp, (int)s, 0);
}

static void set_status(const char *s)
{
	snprintf(sc.status_buf, sizeof sc.status_buf, "%.159s", s);
	set_text(S_STATUS, sc.status_buf);
}

static void set_found(const char *s)
{
	snprintf(sc.found_buf, sizeof sc.found_buf, "%.159s", s);
	set_text(S_FOUND, sc.found_buf);
}

static int get_radio(int id)
{
	int v = 0;
	tbx_method_out(sc.obj, 389, id, &v);
	return v != 0;
}

static int get_option(int id)
{
	int v = 0;
	tbx_method_out(sc.obj, 197, id, &v);
	return v != 0;
}

static void fade(int id, int off)
{
	int fl = 0;
	tbx_method_out(sc.obj, 64, id, &fl);
	fl = off ? fl | (int)(1u << 31) : fl & ~(int)(1u << 31);
	tbx_method(sc.obj, 65, id, fl, 0);
}

static void update_buttons(void)
{
	int busy = sc.job || sc.find;
	fade(S_SCAN, busy);
	fade(S_FIND, busy);
	fade(S_STOP, !busy);
}

/* The address kept from last time. */
static void load_address(void)
{
	FILE *f = fopen("Choices:Folio.Scanner", "r");
	if (f) {
		if (fgets(sc.addr_buf, sizeof sc.addr_buf, f))
			sc.addr_buf[strcspn(sc.addr_buf, "\r\n")] = 0;
		fclose(f);
	}
}

static void save_address(const char *a)
{
	const char *cw = getenv("Choices$Write");
	char path[220];
	FILE *f;
	if (!cw || !*cw)
		return;
	snprintf(path, sizeof path, "%s.Folio", cw);
	_swix(OS_File, _INR(0, 1) | _IN(4), 8, path, 0);
	snprintf(path, sizeof path, "%s.Folio.Scanner", cw);
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n", a);
		fclose(f);
	}
}

void scanui_open(void)
{
	int pos[2];
	if (!sc.obj) {
		load_address();
		snprintf(sc.found_buf, sizeof sc.found_buf, "%s", sc.addr_buf[0] ? "" :
			"Type the scanner's address, or click Find.");
		snprintf(sc.status_buf, sizeof sc.status_buf, "Ready.");
		if (!(sc.obj = create_window()))
			return;
		tbx_method(sc.obj, 388, S_GLASS, 1, 0);
		tbx_method(sc.obj, 388, S_COLOUR, 1, 0);
		tbx_method(sc.obj, 388, S_R300, 1, 0);
		tbx_method(sc.obj, 388, S_A4, 1, 0);
		/* Text recognition: on, with the OCR plugin; faded without. */
		tbx_method(sc.obj, 196, S_OCR, ocr_available() ? 1 : 0, 0);
		fade(S_OCR, !ocr_available());
		pos[0] = (app.screen_w - BOX_W) / 2;
		pos[1] = (app.screen_h + BOX_H) / 2 + 100;
		update_buttons();
		tbx_show(sc.obj, 0, 2, pos, 0, -1);
		return;
	}
	tbx_show(sc.obj, 0, 0, NULL, 0, -1);
}

/* "host", "host:port", "host:port/root", "http://host:port/root". */
static void parse_address(const char *in, char *host, size_t hsize, int *port, char *root, size_t rsize)
{
	const char *p = in, *slash, *colon;
	char tmp[120];
	while (*p == ' ')
		p++;
	if (!strncmp(p, "http://", 7))
		p += 7;
	snprintf(tmp, sizeof tmp, "%s", p);
	tmp[strcspn(tmp, " \t\r\n")] = 0;
	slash = strchr(tmp, '/');
	snprintf(root, rsize, "%s", slash && slash[1] ? slash + 1 : "eSCL");
	if (slash)
		tmp[slash - tmp] = 0;
	colon = strchr(tmp, ':');
	*port = colon ? atoi(colon + 1) : 80;
	if (colon)
		tmp[colon - tmp] = 0;
	snprintf(host, hsize, "%.99s", tmp);
}

static void show_found(void)
{
	char a[120], f[160];
	const escl_found *e = &sc.found[sc.cur];
	if (e->port == 80 && !strcmp(e->root, "eSCL"))
		snprintf(a, sizeof a, "%s", e->host);
	else
		snprintf(a, sizeof a, "%s:%d/%s", e->host, e->port, e->root);
	snprintf(sc.addr_buf, sizeof sc.addr_buf, "%s", a);
	set_text(S_ADDR, sc.addr_buf);
	if (sc.nfound > 1)
		snprintf(f, sizeof f, "%.100s (%d of %d: Find for the next)", e->name, sc.cur + 1, sc.nfound);
	else
		snprintf(f, sizeof f, "%.140s", e->name);
	set_found(f);
}

static void find_start(void)
{
	unsigned me = escl_local_address(0xe00000fbu), base, addrs[256];
	int n = 0, i;
	/* Found before, and more than one: the next. */
	if (sc.nfound > 1) {
		sc.cur = (sc.cur + 1) % sc.nfound;
		show_found();
		return;
	}
	if (!me)
		me = escl_local_address(0x08080808u);
	if (!me) {
		set_found("Folio can't tell this computer's network address. Type the scanner's.");
		return;
	}
	/* mDNS's own group, and everyone on this computer's /24. */
	addrs[n++] = 0xe00000fbu;
	base = me & 0xffffff00u;
	for (i = 1; i < 255; i++)
		if ((base | (unsigned)i) != me)
			addrs[n++] = base | (unsigned)i;
	sc.find = escl_find_new(addrs, n, 5353, 200);
	if (!sc.find) {
		set_found("Folio couldn't look for scanners.");
		return;
	}
	app_log("scan: looking for eSCL scanners near %u.%u.%u.%u", me >> 24, (me >> 16) & 255, (me >> 8) & 255, me & 255);
	set_found("Looking for scanners...");
	update_buttons();
}

static void find_done(void)
{
	const escl_found *list;
	int i;
	sc.nfound = escl_find_results(sc.find, &list);
	if (sc.nfound > (int)(sizeof sc.found / sizeof sc.found[0]))
		sc.nfound = (int)(sizeof sc.found / sizeof sc.found[0]);
	for (i = 0; i < sc.nfound; i++) {
		sc.found[i] = list[i];
		app_log("scan: found %s at %s:%d/%s", list[i].name, list[i].host, list[i].port, list[i].root);
	}
	escl_find_free(sc.find);
	sc.find = NULL;
	sc.cur = 0;
	if (sc.nfound)
		show_found();
	else
		set_found("No scanners answered. Type the scanner's address instead.");
	update_buttons();
}

static void free_pages(void)
{
	int i;
	for (i = 0; i < sc.npages; i++)
		free(sc.paths[i]);
	free(sc.paths);
	sc.paths = NULL;
	sc.npages = 0;
}

static void scan_start(void)
{
	char host[100], root[64], text[120];
	int port;
	escl_settings s;
	text[0] = 0;
	_swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, sc.obj, 513, S_ADDR, text, (int)sizeof text);
	text[sizeof text - 1] = 0;
	parse_address(text, host, sizeof host, &port, root, sizeof root);
	if (!host[0]) {
		set_status("Type the scanner's address first, or click Find.");
		return;
	}
	memset(&s, 0, sizeof s);
	s.source = get_radio(S_FEEDER) ? ESCL_FEEDER : ESCL_PLATEN;
	s.duplex = s.source == ESCL_FEEDER && get_option(S_DUPLEX);
	s.color = get_radio(S_GREY) ? ESCL_GRAY : ESCL_RGB;
	s.dpi = get_radio(S_R150) ? 150 : get_radio(S_R600) ? 600 : 300;
	if (get_radio(S_A4)) {
		s.width = 2480;               /* 210 x 297 mm in 1/300 inch */
		s.height = 3508;
	} else if (get_radio(S_LETTER)) {
		s.width = 2550;
		s.height = 3300;
	}
	sc.job = escl_job_new(host, port, root, &s);
	if (!sc.job) {
		set_status(escl_last_error());
		return;
	}
	save_address(text);
	free_pages();
	sc.all_images = 1;
	pagesave_new_path("Pages", sc.dir, sizeof sc.dir);
	/* A directory for the pages (pagesave_new_path names a file in a new one). */
	_swix(OS_File, _INR(0, 1) | _IN(4), 8, sc.dir, 0);
	app_log("scan: %s:%d/%s, %s%s, %s, %d dpi", host, port, root, s.source == ESCL_FEEDER ? "feeder" : "glass",
		s.duplex ? " (both sides)" : "", s.color == ESCL_GRAY ? "grey" : "colour", s.dpi);
	set_status(escl_status(sc.job));
	update_buttons();
}

/* A page has come: into the directory. */
static void keep_page(void)
{
	size_t len;
	const char *mime;
	unsigned char *p = escl_take_page(sc.job, &len, &mime);
	char path[340], **np;
	FILE *f;
	int type = !strcmp(mime, "application/pdf") ? 0xadf : !strcmp(mime, "image/png") ? 0xb60 : 0xc85;
	if (!p)
		return;
	snprintf(path, sizeof path, "%s.Page%03d", sc.dir, sc.npages + 1);
	f = fopen(path, "wb");
	if (!f || fwrite(p, 1, len, f) != len) {
		if (f)
			fclose(f);
		free(p);
		set_status("Folio couldn't keep the page: is the scrap directory full?");
		return;
	}
	fclose(f);
	free(p);
	_swix(OS_File, _INR(0, 2), 18, path, type);
	if (type == 0xadf)
		sc.all_images = 0;
	np = realloc(sc.paths, sizeof *np * (size_t)(sc.npages + 1));
	if (!np)
		return;
	sc.paths = np;
	sc.paths[sc.npages] = malloc(strlen(path) + 1);
	if (sc.paths[sc.npages]) {
		strcpy(sc.paths[sc.npages], path);
		sc.npages++;
	}
	app_log("scan: page %d, %lu bytes, %s", sc.npages, (unsigned long)len, mime);
}

/* The scan has finished: the pages as a new PDF. */
static void scan_done(int ok)
{
	char out[600], m[300];
	int dpi = sc.job ? escl_job_dpi(sc.job) : 0, made;
	if (!ok)
		snprintf(m, sizeof m, "%s.", escl_error(sc.job));
	escl_job_free(sc.job);
	sc.job = NULL;
	update_buttons();
	if (!ok) {
		app_log("scan: stopped: %s", m);
		set_status(m);
		if (!sc.npages)
			return;
	}
	if (!sc.npages) {
		set_status("The scanner sent no pages.");
		return;
	}
	pagesave_new_path("Scan", out, sizeof out);
	_swix(Hourglass_On, 0);
	core_now();
	made = sc.all_images ?
		folio_images_to_pdf_res(app.fc, (const char *const *)sc.paths, sc.npages, dpi, out) == 0 :
		folio_combine(app.fc, (const char *const *)sc.paths, sc.npages, out) == 0;
	core_unlock();
	_swix(Hourglass_Off, 0);
	if (!made) {
		snprintf(m, sizeof m, "Folio couldn't make the PDF: %s", folio_error(app.fc));
		set_status(m);
		return;
	}
	if (ok) {
		snprintf(m, sizeof m, "Scanned %d page%s.", sc.npages, sc.npages == 1 ? "" : "s");
		set_status(m);
	}
	_swix(OS_File, _INR(0, 2), 18, out, 0xadf);
	{
		docwin *w = docwin_open(out);
		if (w) {
			docwin_mark_new(w);
			if (ocr_available() && sc.obj && get_option(S_OCR))
				docwin_ocr_all(w);
		}
	}
}

void scanui_event(int code)
{
	switch (code) {
	case EV_SCAN:
		scanui_open();
		break;
	case EV_SC_FIND:
		if (!sc.find && !sc.job)
			find_start();
		break;
	case EV_SC_SCAN:
		if (!sc.find && !sc.job)
			scan_start();
		break;
	case EV_SC_STOP:
		if (sc.find) {
			escl_find_free(sc.find);
			sc.find = NULL;
			set_found("Stopped looking.");
			update_buttons();
		}
		if (sc.job)
			escl_cancel(sc.job);
		break;
	}
}

int scanui_background(unsigned *wake)
{
	unsigned now;
	int i;
	if (!sc.find && !sc.job)
		return 0;
	if (sc.find && !escl_find_step(sc.find))
		find_done();
	for (i = 0; sc.job && i < 16; i++) {
		int r = escl_step(sc.job);
		if (r == ESCL_PAGE) {
			keep_page();
			continue;
		}
		if (r == ESCL_DONE) {
			scan_done(1);
			break;
		}
		if (r == ESCL_ERROR) {
			scan_done(0);
			break;
		}
		if (strcmp(sc.status_buf, escl_status(sc.job))) {
			char m[160];
			snprintf(m, sizeof m, "%s", escl_status(sc.job));
			if (sc.npages)
				snprintf(m + strlen(m), sizeof m - strlen(m), " (%d so far)", sc.npages);
			if (strcmp(m, sc.status_buf))
				set_status(m);
		}
		break;
	}
	/* Come back in 2 cs (the network is slow next to the machine). */
	_swix(OS_ReadMonotonicTime, _OUT(0), &now);
	if (sc.find || sc.job) {
		unsigned w = now + 2;
		if (!*wake || (int)(w - *wake) < 0)
			*wake = w;
	}
	return 0;
}
