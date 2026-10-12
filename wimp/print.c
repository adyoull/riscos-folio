/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Printing, through the printer drivers (!Printers).
 *
 * Print... (or the Print key) shows the Toolbox's Print dialogue
 * (PrintDbox: pages, copies, scale, upright or sideways, draft). Clicking
 * Print starts the job, which then runs in the background, a step per null
 * poll, so the desktop carries on (test21 printed in one go and held the
 * machine until it finished):
 *
 *   start: open printer:, PDriver_SelectJob, then suspend it (select the
 *     previous job) whenever Folio goes back to the Wimp
 *   count: a book's chapters, a step a poll (every page has to be known)
 *   each page: draw its bands (MuPDF, 32bpp sprites), one a poll; then, the
 *     job selected again, PDriver_GiveRectangle (the page placed and turned
 *     on the paper: printlay.c), PDriver_DrawPage and PDriver_GetRectangle
 *     until the driver has had all of it (only plotting: the bands are
 *     ready), and suspend
 *   end: PDriver_EndJob, close printer:
 *
 * The window's title shows how far it's got; Print... again (or closing the
 * window) stops it. Always a white page, whatever the window shows (dark or
 * sepia is for the screen), with the notes and markings. FolioLog has each
 * page's times: drawing (MuPDF) and plotting (the driver). */
#include "app.h"
#include "worker.h"
#include "printlay.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define Class_PrintDbox      0x82b00
#define PrintDbox_Print      0x82b05
#define PrintDbox_SetPageRange 1

#define DRAFT_DPI   150
#define BEST_DPI    300
#define PAGE_BYTES  (48u << 20)   /* most a page's pixels may take */
#define BAND_ROWS   128           /* about 4 cs to draw at 300 dpi on a Pi 4 */

static struct {
	ObjectId obj;
	docwin *w;
} pr;

static void job_stop(const char *why);
static int job_running(docwin *w);

void print_forget(docwin *w)
{
	if (job_running(w))
		job_stop(NULL);
	if (pr.w == w) {
		if (pr.obj)
			tbx_hide(pr.obj);
		pr.w = NULL;
	}
}

static int have_printer(char *name, size_t size)
{
	int r0, xres, yres, features;
	const char *n = NULL;
	if (_swix(PDriver_Info, _OUTR(0, 4), &r0, &xres, &yres, &features, &n))
		return 0;
	snprintf(name, size, "%s", n ? n : "");
	return 1;
}

void print_open(docwin *w)
{
	static struct {
		int flags, from, to, copies, scale;
		const char *further_options, *window;
	} t;
	char name[64];
	int pages, exact = 0;
	if (job_running(NULL)) {
		/* One job at a time: Print... while printing stops it. */
		job_stop("Printing stopped.");
		return;
	}
	if (!have_printer(name, sizeof name)) {
		tbx_report("There's no printer driver loaded: run !Printers, choose your printer, then try again.");
		return;
	}
	core_now();
	pages = folio_page_count(docwin_doc(w), &exact);
	core_unlock();
	if (!pr.obj) {
		_kernel_oserror *e;
		memset(&t, 0, sizeof t);
		/* Page range, copies, scale, upright/sideways, draft (Toolbox p207). */
		t.flags = (1 << 3) | (1 << 4) | (1 << 5) | (1 << 6) | (1 << 9);
		t.from = 1;
		t.to = pages > 0 ? pages : 1;
		t.copies = 1;
		t.scale = 100;
		e = tbx_create(Class_PrintDbox, 100, 0, &t, sizeof t, &pr.obj);
		if (e) {
			tbx_report_error(e);
			pr.obj = 0;
			return;
		}
	}
	pr.w = w;
	/* All pages; To filled in with the last one. */
	tbx_method(pr.obj, PrintDbox_SetPageRange, -1, pages > 0 ? pages : 1, 0);
	tbx_show(pr.obj, 0, 0, NULL, 0, -1);
	app_log("print: %s, %d pages%s", name, pages, exact ? "" : " so far");
}

/* ---- A page in bands ------------------------------------------------------ */

typedef struct {
	int *area;              /* 32bpp sprite of rows r0..r1, or NULL */
	int r0, r1;
} band;

static int *band_area(int px_w, int rows)
{
	size_t img = (size_t)px_w * 4 * rows, bytes = 16 + 44 + img;
	int *area = malloc(bytes), *spr;
	if (!area)
		return NULL;
	area[0] = (int)bytes;
	area[1] = 1;
	area[2] = 16;
	area[3] = (int)bytes;
	spr = area + 4;
	spr[0] = (int)(44 + img);
	memset(spr + 1, 0, 12);
	strcpy((char *)(spr + 1), "band");
	spr[4] = px_w - 1;
	spr[5] = rows - 1;
	spr[6] = 0;
	spr[7] = 31;
	spr[8] = 44;
	spr[9] = 44;
	/* 32bpp, 90 dpi: a pixel is 2 OS units before scaling. */
	spr[10] = (6 << 27) | (90 << 14) | (90 << 1) | 1;
	return area;
}

/* Time spent, for the log: drawing (MuPDF) and plotting (the driver). */
static unsigned t_draw, t_plot;
static int n_rects, n_plots;

static unsigned cs_now(void)
{
	unsigned t;
	_swix(OS_ReadMonotonicTime, _OUT(0), &t);
	return t;
}

/* A sprite of some rows of a band, for plotting just those (reused). */
static int *part;
static size_t part_bytes;

static int *rows_area(const int *band, int px_w, int r_from, int rows)
{
	size_t img = (size_t)px_w * 4 * rows, bytes = 16 + 44 + img;
	int *spr;
	if (bytes > part_bytes) {
		int *n = realloc(part, bytes);
		if (!n)
			return NULL;
		part = n;
		part_bytes = bytes;
	}
	memcpy(part, band, 16 + 44);          /* the area and sprite headers */
	part[0] = part[3] = (int)bytes;
	spr = part + 4;
	spr[0] = (int)(44 + img);
	spr[5] = rows - 1;
	memcpy((char *)spr + 44, (const char *)(band + 4) + 44 + (size_t)r_from * px_w * 4, img);
	return part;
}

/* Plots the parts of the bands that cross user rectangle r (x0, y0, x1, y1),
 * (drawn already). A bitmap driver asks for the page a strip
 * at a time, so only the strip's rows go to it: plotting a whole band
 * for each strip made it work through every row of the band, clipped or not.
 * -1 on an error (message in err). */
static int plot_rect(const print_place *pl, band *b, int nb,
		const int *r, char *err, size_t err_size)
{
	int i;
	/* The page's pixel rows the rectangle covers (a row's margin each side). */
	int row_a = (int)((long long)(pl->h_os - r[3]) * pl->px_h / pl->h_os) - 1;
	int row_b = (int)((long long)(pl->h_os - r[1]) * pl->px_h / pl->h_os) + 2;
	n_rects++;
	for (i = 0; i < nb; i++) {
		int ra = b[i].r0 > row_a ? b[i].r0 : row_a, rb = b[i].r1 < row_b ? b[i].r1 : row_b;
		int top, bottom, scale[4], *area;
		unsigned t0;
		_kernel_oserror *e;
		if (ra >= rb)
			continue;
		if (!b[i].area)
			continue;            /* (all drawn before the page is sent) */
		/* Rows ra..rb of the page, from this band. */
		area = ra == b[i].r0 && rb == b[i].r1 ? b[i].area :
			rows_area(b[i].area, pl->px_w, ra - b[i].r0, rb - ra);
		if (!area) {
			snprintf(err, err_size, "There isn't enough memory to print this page.");
			return -1;
		}
		top = pl->h_os - (int)((long long)ra * pl->h_os / pl->px_h);
		bottom = pl->h_os - (int)((long long)rb * pl->h_os / pl->px_h);
		/* Scaled from its natural size (2 OS units a pixel) to its place;
		 * a unit taller (unless it's the page's last row), so no gap shows. */
		scale[0] = pl->w_os;
		scale[1] = top - bottom + (rb < pl->px_h ? 1 : 0);
		scale[2] = pl->px_w * 2;
		scale[3] = (rb - ra) * 2;
		t0 = cs_now();
		e = _swix(OS_SpriteOp, _INR(0, 7), 52 + 512, area, area + 4, 0,
			bottom - (rb < pl->px_h ? 1 : 0), 0, scale, 0);
		t_plot += cs_now() - t0;
		n_plots++;
		if (e) {
			snprintf(err, err_size, "%s", e->errmess);
			return -1;
		}
	}
	return 0;
}

static void free_bands(band *b, int nb)
{
	int i;
	for (i = 0; i < nb; i++) {
		free(b[i].area);
		b[i].area = NULL;
	}
}

/* ---- The job, in the background ------------------------------------------- */

enum { J_IDLE, J_COUNT, J_SETUP, J_DRAW, J_SEND, J_END };

static struct {
	int stage;
	docwin *w;
	int file, old;            /* printer: and the job that was current before ours */
	int first, last, cur, copies, scale_pc, sideways, draft, xres, printable[4], done;
	folio_loc loc;
	print_place pl;
	band *bands;
	int nb, next_band;
	unsigned t_page;
} J;

static int job_running(docwin *w)
{
	return J.stage != J_IDLE && (!w || J.w == w);
}

static void set_note(void)
{
	char note[48];
	if (J.stage == J_COUNT)
		snprintf(note, sizeof note, " (printing: counting pages)");
	else
		snprintf(note, sizeof note, " (printing page %d of %d)", J.cur + 1, J.last + 1);
	docwin_set_note(J.w, note);
}

static void drop_bands(void)
{
	if (J.bands) {
		free_bands(J.bands, J.nb);
		free(J.bands);
		J.bands = NULL;
	}
	J.nb = 0;
}

/* Ends the job: aborted with a message (why), or quietly (NULL: the window
 * closed). */
static void job_stop(const char *why)
{
	if (J.stage == J_IDLE)
		return;
	drop_bands();
	_swix(PDriver_AbortJob, _IN(0), J.file);
	_swix(OS_Find, _INR(0, 1), 0, J.file);
	if (J.w && why)
		docwin_set_note(J.w, "");       /* (not as the window closes) */
	J.stage = J_IDLE;
	free(part);
	part = NULL;
	part_bytes = 0;
	app_log("print: stopped%s%s", why ? ": " : "", why ? why : "");
	if (why)
		tbx_report(why);
}

static void job_fail(const char *msg)
{
	char m[300];
	snprintf(m, sizeof m, "Printing failed: %s", msg);
	job_stop(m);
}

/* Our job current again (between polls it's suspended). */
static _kernel_oserror *resume(void)
{
	int prev;
	return _swix(PDriver_SelectJob, _INR(0, 1) | _OUT(0), J.file, 0, &prev);
}

static void suspend(void)
{
	_swix(PDriver_SelectJob, _INR(0, 1), J.old, 0);
}

static void print_start(docwin *w, int first, int last, int copies, int scale_pc, int sideways,
		int draft)
{
	_kernel_oserror *e;
	memset(&J, 0, sizeof J);
	J.xres = 300;
	if ((e = _swix(PDriver_Info, _OUT(1), &J.xres)) ||
			(e = _swix(PDriver_PageSize, _OUTR(3, 6), &J.printable[0], &J.printable[1],
				&J.printable[2], &J.printable[3]))) {
		tbx_report_error(e);
		return;
	}
	if (_swix(OS_Find, _INR(0, 1) | _OUT(0), 0x80, "printer:", &J.file) || !J.file) {
		tbx_report("Folio couldn't open the printer (printer:). Is !Printers running?");
		return;
	}
	e = _swix(PDriver_SelectJob, _INR(0, 1) | _OUT(0), J.file, docwin_title(w), &J.old);
	if (e) {
		_swix(OS_Find, _INR(0, 1), 0, J.file);
		tbx_report_error(e);
		return;
	}
	suspend();
	J.w = w;
	J.first = first;
	J.last = last;
	J.copies = copies > 0 ? copies : 1;
	J.scale_pc = scale_pc;
	J.sideways = sideways;
	J.draft = draft;
	J.stage = J_COUNT;
	app_log("print: pages %d-%d, %d copies, %d%%%s%s, printable %d,%d-%d,%d mp, %d dpi",
		first + 1, last + 1, J.copies, scale_pc, sideways ? ", sideways" : "", draft ? ", draft" : "",
		J.printable[0], J.printable[1], J.printable[2], J.printable[3], J.xres);
	set_note();
}

/* Sends page J.cur, its bands all drawn: the only part that has the
 * machine for long (the driver's work). */
static void send_page(void)
{
	char pagestr[16];
	int r[4], more = 0, id = 0;
	char err[256];
	_kernel_oserror *e;
	err[0] = 0;
	t_plot = 0;
	n_rects = n_plots = 0;
	_swix(Hourglass_On, 0);
	e = resume();
	if (!e) {
		r[0] = 0;
		r[1] = 0;
		r[2] = J.pl.w_os;
		r[3] = J.pl.h_os;
		e = _swix(PDriver_GiveRectangle, _INR(0, 4), J.cur + 1, r, J.pl.matrix, J.pl.pos, 0xffffff00u);
	}
	if (!e) {
		snprintf(pagestr, sizeof pagestr, "%d", J.cur + 1);
		e = _swix(PDriver_DrawPage, _INR(0, 3) | _OUT(0) | _OUT(2), J.copies, r, ++J.done, pagestr,
			&more, &id);
	}
	while (!e && more) {
		if (plot_rect(&J.pl, J.bands, J.nb, r, err, sizeof err))
			break;
		e = _swix(PDriver_GetRectangle, _IN(1) | _OUT(0) | _OUT(2), r, &more, &id);
	}
	suspend();
	_swix(Hourglass_Off, 0);
	if (e)
		snprintf(err, sizeof err, "%s", e->errmess);
	if (err[0]) {
		job_fail(err);
		return;
	}
	app_log("print: page %d: %dx%d px, drawing %u cs; %d rectangle(s), %d plot(s), plotting %u cs; "
		"in all %u cs", J.cur + 1, J.pl.px_w, J.pl.px_h, t_draw, n_rects, n_plots, t_plot,
		cs_now() - J.t_page);
	drop_bands();
	J.cur++;
	J.stage = J.cur > J.last ? J_END : J_SETUP;
}

/* A step of the job (null polls). 1 while there's more to do. */
int print_background(void)
{
	folio_doc *doc;
	if (J.stage == J_IDLE)
		return 0;
	doc = docwin_doc(J.w);
	switch (J.stage) {
	case J_COUNT: {
		int done, exact = 0, pages;
		core_now();
		done = folio_count_step(doc, 1);
		pages = folio_page_count(doc, &exact);
		core_unlock();
		if (done < 0) {
			job_fail("the document's pages couldn't be counted.");
			return 0;
		}
		if (done) {
			if (J.last < 0 || J.last >= pages)
				J.last = pages - 1;
			if (J.first < 0)
				J.first = 0;
			J.cur = J.first;
			J.stage = J.cur > J.last ? J_END : J_SETUP;
		}
		break;
	}
	case J_SETUP: {
		folio_rect bounds;
		int dpi = J.draft ? DRAFT_DPI : BEST_DPI, k, rc;
		if (J.xres > 0 && J.xres < dpi)
			dpi = J.xres;
		core_now();
		rc = folio_loc_from_number(doc, J.cur, &J.loc) || folio_page_bounds(doc, J.loc, &bounds);
		core_unlock();
		if (rc) {
			char m[200];
			snprintf(m, sizeof m, "page %d can't be read.", J.cur + 1);
			job_fail(m);
			return 0;
		}
		J.t_page = cs_now();
		t_draw = 0;
		if (print_layout(bounds.x1 - bounds.x0, bounds.y1 - bounds.y0, J.printable, J.sideways,
				J.scale_pc, dpi, PAGE_BYTES, &J.pl)) {
			J.cur++;
			J.stage = J.cur > J.last ? J_END : J_SETUP;
			break;
		}
		J.nb = (J.pl.px_h + BAND_ROWS - 1) / BAND_ROWS;
		J.bands = calloc(J.nb, sizeof *J.bands);
		if (!J.bands) {
			job_fail("there isn't enough memory.");
			return 0;
		}
		for (k = 0; k < J.nb; k++) {
			J.bands[k].r0 = k * BAND_ROWS;
			J.bands[k].r1 = k == J.nb - 1 ? J.pl.px_h : (k + 1) * BAND_ROWS;
		}
		J.next_band = 0;
		J.stage = J_DRAW;
		set_note();
		break;
	}
	case J_DRAW: {
		band *b = &J.bands[J.next_band];
		folio_irect tile;
		unsigned t0 = cs_now();
		int rc;
		b->area = band_area(J.pl.px_w, b->r1 - b->r0);
		if (!b->area) {
			job_fail("there isn't enough memory to print this page.");
			return 0;
		}
		tile.x0 = 0;
		tile.x1 = J.pl.px_w;
		tile.y0 = b->r0;
		tile.y1 = b->r1;
		core_now();
		rc = folio_render_tile(doc, J.loc, J.pl.zoom, tile, FOLIO_PAGE_NORMAL, 1,
			(uint32_t *)((char *)(b->area + 4) + 44), J.pl.px_w * 4);
		core_unlock();
		t_draw += cs_now() - t0;
		if (rc) {
			job_fail(folio_error(app.fc));
			return 0;
		}
		if (++J.next_band == J.nb)
			J.stage = J_SEND;
		break;
	}
	case J_SEND:
		send_page();
		break;
	case J_END: {
		_kernel_oserror *e = resume();
		if (!e)
			e = _swix(PDriver_EndJob, _IN(0), J.file);
		suspend();
		if (e) {
			job_fail(e->errmess);
			return 0;
		}
		_swix(OS_Find, _INR(0, 1), 0, J.file);
		docwin_set_note(J.w, "");
		app_log("print: %d page(s) sent", J.done);
		J.stage = J_IDLE;
		free(part);
		part = NULL;
		part_bytes = 0;
		return 0;
	}
	}
	return J.stage != J_IDLE;
}

int print_event(int *b)
{
	if (b[2] != PrintDbox_Print)
		return 0;
	if (pr.w && !job_running(NULL)) {
		int from = b[4], to = b[5];
		print_start(pr.w, from < 0 ? -1 : from - 1, from < 0 ? -1 : to - 1, b[6], b[7],
			(b[3] & 1) != 0, (b[3] & 2) != 0);
	}
	return 1;
}

void print_quit(void)
{
	job_stop(NULL);
}
