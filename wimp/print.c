/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Printing, through the printer drivers (!Printers).
 *
 * Print... (or the Print key) shows the Toolbox's Print dialogue
 * (PrintDbox: pages, copies, scale, upright or sideways, draft). Clicking
 * Print prints there and then, with the hourglass showing how far it's got:
 *
 *   open printer:, PDriver_SelectJob
 *   each page: PDriver_GiveRectangle (the page as a rectangle of OS units,
 *     placed and turned on the paper: printlay.c), then PDriver_DrawPage and
 *     PDriver_GetRectangle until the driver has had all of it
 *   PDriver_EndJob, close printer:
 *
 * Pages are drawn by MuPDF into 32bpp sprites a band at a time, only the
 * bands each rectangle the driver asks for needs, kept until the page is
 * done (a bitmap driver asks for a strip at a time; PostScript for the
 * whole page once). Always a white page, whatever the window shows (dark
 * or sepia is for the screen), with the notes and markings. */
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
#define BAND_ROWS   256

static struct {
	ObjectId obj;
	docwin *w;
} pr;

void print_forget(docwin *w)
{
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

/* Plots the bands that cross user rectangle r (x0, y0, x1, y1), drawing
 * them first if need be. -1 on an error (message in err). */
static int plot_rect(folio_doc *doc, folio_loc loc, const print_place *pl, band *b, int nb,
		const int *r, char *err, size_t err_size)
{
	int i;
	for (i = 0; i < nb; i++) {
		/* The band's OS y range, page top at h_os. */
		int top = pl->h_os - (int)((long long)b[i].r0 * pl->h_os / pl->px_h);
		int bottom = pl->h_os - (int)((long long)b[i].r1 * pl->h_os / pl->px_h);
		int scale[4];
		_kernel_oserror *e;
		if (top <= r[1] || bottom >= r[3])
			continue;
		if (!b[i].area) {
			folio_irect tile;
			b[i].area = band_area(pl->px_w, b[i].r1 - b[i].r0);
			if (!b[i].area) {
				snprintf(err, err_size, "There isn't enough memory to print this page.");
				return -1;
			}
			tile.x0 = 0;
			tile.x1 = pl->px_w;
			tile.y0 = b[i].r0;
			tile.y1 = b[i].r1;
			if (folio_render_tile(doc, loc, pl->zoom, tile, FOLIO_PAGE_NORMAL, 1,
					(uint32_t *)((char *)(b[i].area + 4) + 44), pl->px_w * 4)) {
				snprintf(err, err_size, "%s", folio_error(app.fc));
				return -1;
			}
		}
		/* Scaled from its natural size (2 OS units a pixel) to its place;
		 * a unit taller, so no gap shows between bands. */
		scale[0] = pl->w_os;
		scale[1] = top - bottom + (i < nb - 1 ? 1 : 0);
		scale[2] = pl->px_w * 2;
		scale[3] = (b[i].r1 - b[i].r0) * 2;
		e = _swix(OS_SpriteOp, _INR(0, 7), 52 + 512, b[i].area, b[i].area + 4, 0,
			bottom - (i < nb - 1 ? 1 : 0), 0, scale, 0);
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

/* ---- The job --------------------------------------------------------------- */

static void print_doc(docwin *w, int first, int last, int copies, int scale_pc, int sideways,
		int draft)
{
	folio_doc *doc = docwin_doc(w);
	int file = 0, old = 0, job = 0, printable[4], xres = 300, i, done = 0;
	char err[256], pagestr[16];
	_kernel_oserror *e;
	band *bands = NULL;
	int nb = 0;
	err[0] = 0;

	if ((e = _swix(PDriver_Info, _OUT(1), &xres)) ||
			(e = _swix(PDriver_PageSize, _OUTR(3, 6), &printable[0], &printable[1],
				&printable[2], &printable[3]))) {
		tbx_report_error(e);
		return;
	}
	if (_swix(OS_Find, _INR(0, 1) | _OUT(0), 0x80, "printer:", &file) || !file) {
		tbx_report("Folio couldn't open the printer (printer:). Is !Printers running?");
		return;
	}
	_swix(Hourglass_On, 0);
	core_now();
	/* Every page has to be known (a book's chapters may still be counting). */
	while (folio_count_step(doc, 1) == 0)
		;
	{
		int exact = 0, pages = folio_page_count(doc, &exact);
		if (last < 0 || last >= pages)
			last = pages - 1;
		if (first < 0)
			first = 0;
	}
	e = _swix(PDriver_SelectJob, _INR(0, 1) | _OUT(0), file, docwin_title(w), &old);
	if (e) {
		snprintf(err, sizeof err, "%s", e->errmess);
		goto fail;
	}
	job = 1;
	app_log("print: pages %d-%d, %d copies, %d%%%s%s, printable %d,%d-%d,%d mp, %d dpi",
		first + 1, last + 1, copies, scale_pc, sideways ? ", sideways" : "", draft ? ", draft" : "",
		printable[0], printable[1], printable[2], printable[3], xres);

	for (i = first; i <= last; i++) {
		folio_loc loc;
		folio_rect bounds;
		print_place pl;
		int r[4], more = 0, id = 0, k, dpi = draft ? DRAFT_DPI : BEST_DPI;
		if (xres > 0 && xres < dpi)
			dpi = xres;
		if (folio_loc_from_number(doc, i, &loc) || folio_page_bounds(doc, loc, &bounds)) {
			snprintf(err, sizeof err, "Page %d can't be read: %s", i + 1, folio_error(app.fc));
			goto fail;
		}
		if (print_layout(bounds.x1 - bounds.x0, bounds.y1 - bounds.y0, printable, sideways,
				scale_pc, dpi, PAGE_BYTES, &pl))
			continue;
		nb = (pl.px_h + BAND_ROWS - 1) / BAND_ROWS;
		bands = calloc(nb, sizeof *bands);
		if (!bands) {
			snprintf(err, sizeof err, "There isn't enough memory to print.");
			goto fail;
		}
		for (k = 0; k < nb; k++) {
			bands[k].r0 = k * BAND_ROWS;
			bands[k].r1 = k == nb - 1 ? pl.px_h : (k + 1) * BAND_ROWS;
		}
		r[0] = 0;
		r[1] = 0;
		r[2] = pl.w_os;
		r[3] = pl.h_os;
		e = _swix(PDriver_GiveRectangle, _INR(0, 4), i + 1, r, pl.matrix, pl.pos, 0xffffff00u);
		if (!e) {
			snprintf(pagestr, sizeof pagestr, "%d", i + 1);
			e = _swix(PDriver_DrawPage, _INR(0, 3) | _OUT(0) | _OUT(2), copies > 0 ? copies : 1, r,
				++done, pagestr, &more, &id);
		}
		while (!e && more) {
			if (plot_rect(doc, loc, &pl, bands, nb, r, err, sizeof err))
				goto fail;
			e = _swix(PDriver_GetRectangle, _IN(1) | _OUT(0) | _OUT(2), r, &more, &id);
		}
		if (e) {
			snprintf(err, sizeof err, "%s", e->errmess);
			goto fail;
		}
		free_bands(bands, nb);
		free(bands);
		bands = NULL;
		_swix(Hourglass_Percentage, _IN(0), (i - first + 1) * 100 / (last - first + 1));
	}
	e = _swix(PDriver_EndJob, _IN(0), file);
	if (e) {
		snprintf(err, sizeof err, "%s", e->errmess);
		goto fail;
	}
	if (old)
		_swix(PDriver_SelectJob, _INR(0, 1), old, 0);
	core_unlock();
	_swix(OS_Find, _INR(0, 1), 0, file);
	_swix(Hourglass_Off, 0);
	app_log("print: %d page(s) sent", done);
	return;

fail:
	if (bands) {
		free_bands(bands, nb);
		free(bands);
	}
	if (job) {
		_swix(PDriver_AbortJob, _IN(0), file);
		if (old)
			_swix(PDriver_SelectJob, _INR(0, 1), old, 0);
	}
	core_unlock();
	_swix(OS_Find, _INR(0, 1), 0, file);
	_swix(Hourglass_Off, 0);
	app_log("print: failed: %s", err);
	tbx_report(err[0] ? err : "Printing failed.");
}

int print_event(int *b)
{
	if (b[2] != PrintDbox_Print)
		return 0;
	if (pr.w) {
		docwin *w = pr.w;
		int from = b[4], to = b[5];
		print_doc(w, from < 0 ? -1 : from - 1, from < 0 ? -1 : to - 1, b[6], b[7],
			(b[3] & 1) != 0, (b[3] & 2) != 0);
	}
	return 1;
}
