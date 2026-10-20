/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Text recognition for scanned pages: the OCR plugin and when pages are
 * recognised.
 *
 * The plugin is Tesseract's English data, in !Folio.Plugins.OCR (or
 * <Choices$Write>.Folio.Plugins.OCR): Accurate.eng (the standard set without
 * its unused Tesseract 3 part) and Fast.eng. Without it Folio has no Text
 * recognition menu and does nothing here.
 *
 * Recognising a page takes seconds, so it's a worker job (JOB_OCR), given
 * out only when no page or thumbnail is waiting to be drawn, and stopped if
 * one is wanted. Which pages, in order:
 *   1. a page read aloud has reached (speech waits for it: ocr_win_wait);
 *   2. "Recognise text on this page" (even a page with text of its own);
 *   3. with "Recognise scanned pages as I read" (on by default): the page
 *      being read, then the next two the way the reader is going;
 *   4. "Recognise text on all pages": every page in turn.
 * Pages with text of their own are left alone except in 2: the worker
 * checks (FOLIO_OCR_NOT_SCAN) and says so quickly.
 *
 * What's recognised is kept in <Choices$Write>.Folio.OCR.T<hash of the
 * path> (folio_ocr_save), every ten pages and when the window closes, and
 * loaded when the document is opened again. */
#include "app.h"
#include "worker.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

enum { S_UNKNOWN = 0, S_NOT_SCAN, S_DONE, S_EMPTY, S_FAILED };

struct ocr_win {
	docwin *w;
	unsigned char *st;          /* S_* per page */
	int n;
	int urgent;                 /* a page reading aloud is waiting for, or -1 */
	int asked;                  /* "this page", or -1 */
	int all, all_next;          /* "all pages": on, and the next to look at */
	int running;                /* the page the worker has, or -1 */
	int since_save;
	int unsaved;                /* save even if the core has no changes */
	unsigned t0;                /* when "all pages" started */
	char cache[256];            /* where its text is kept */
};

static struct {
	int available;
	char dir[256];
	int has_accurate, has_fast;
} ocr;

/* ---- The plugin ---------------------------------------------------------- */

static int object_type(const char *path)
{
	int type = 0;
	if (_swix(OS_File, _INR(0, 1) | _OUT(0), 17, path, &type))
		return 0;
	return type;
}

static int canonical(const char *path, char *out, int size)
{
	int spare = 0;
	if (_swix(OS_FSControl, _INR(0, 5) | _OUT(5), 37, path, out, 0, 0, size, &spare) || spare <= 0)
		return -1;
	return 0;
}

static int try_plugin(const char *where)
{
	char dir[256], f[300];
	if (canonical(where, dir, sizeof dir) || (object_type(dir) & 2) == 0)
		return 0;
	snprintf(f, sizeof f, "%s.Accurate.eng", dir);
	ocr.has_accurate = object_type(f) == 1;
	snprintf(f, sizeof f, "%s.Fast.eng", dir);
	ocr.has_fast = object_type(f) == 1;
	if (!ocr.has_accurate && !ocr.has_fast)
		return 0;
	snprintf(ocr.dir, sizeof ocr.dir, "%s", dir);
	return 1;
}

int ocr_plugin_init(void)
{
	ocr.available = try_plugin("<Folio$Dir>.Plugins.OCR") ||
		try_plugin("<Choices$Write>.Folio.Plugins.OCR");
	if (ocr.available)
		app_log("ocr: plugin %s (%s%s%s)", ocr.dir, ocr.has_accurate ? "Accurate" : "",
			ocr.has_accurate && ocr.has_fast ? ", " : "", ocr.has_fast ? "Fast" : "");
	else
		app_log("ocr: no plugin (Plugins.OCR)");
	return ocr.available;
}

int ocr_available(void)
{
	return ocr.available;
}

int ocr_has_accurate(void) { return ocr.has_accurate; }
int ocr_has_fast(void) { return ocr.has_fast; }

/* The data folder for the chosen speed (the other if only it is there). */
static int use_fast(void)
{
	return ocr.has_fast && (app.ch.ocr_fast || !ocr.has_accurate);
}

static void data_dir(char *out, size_t size)
{
	snprintf(out, size, "%.240s.%s", ocr.dir, use_fast() ? "Fast" : "Accurate");
}

/* ---- Per window ------------------------------------------------------------ */

static unsigned hash(const char *s)
{
	unsigned h = 2166136261u;
	for (; *s; s++) {
		unsigned c = (unsigned char)*s;
		if (c >= 'A' && c <= 'Z')
			c += 32;          /* RISC OS names ignore case */
		h = (h ^ c) * 16777619u;
	}
	return h;
}

static void cache_path(ocr_win *o, int for_writing)
{
	const char *cw = getenv("Choices$Write");
	char dir[200];
	unsigned h = hash(docwin_path(o->w));
	if (!for_writing) {
		snprintf(o->cache, sizeof o->cache, "Choices:Folio.OCR.T%08x", h);
		return;
	}
	o->cache[0] = 0;
	if (!cw || !*cw)
		return;
	snprintf(dir, sizeof dir, "%s.Folio", cw);
	_swix(OS_File, _INR(0, 1) | _IN(4), 8, dir, 0);
	snprintf(dir, sizeof dir, "%s.Folio.OCR", cw);
	_swix(OS_File, _INR(0, 1) | _IN(4), 8, dir, 0);
	snprintf(o->cache, sizeof o->cache, "%s.T%08x", dir, h);
}

static int grow(ocr_win *o, int pages)
{
	unsigned char *p;
	if (pages <= o->n)
		return 0;
	p = realloc(o->st, pages);
	if (!p)
		return -1;
	memset(p + o->n, S_UNKNOWN, pages - o->n);
	o->st = p;
	o->n = pages;
	return 0;
}

/* Reads the saved text. Called with the core lock held. */
static void load(ocr_win *o)
{
	folio_doc *doc = docwin_doc(o->w);
	int i, n;
	cache_path(o, 0);
	n = folio_ocr_load(doc, o->cache);
	if (n <= 0)
		return;
	for (i = 0; i < o->n; i++) {
		folio_loc loc;
		if (folio_loc_from_number(doc, i, &loc) == 0 && folio_page_has_ocr(doc, loc))
			o->st[i] = S_DONE;
	}
	app_log("ocr: %d page(s) of recognised text from %s", n, o->cache);
}

/* Called with the core lock held. */
static void save(ocr_win *o)
{
	folio_doc *doc = docwin_doc(o->w);
	if (!doc || (!folio_ocr_changed(doc) && !o->unsaved))
		return;
	cache_path(o, 1);
	if (!o->cache[0])
		return;
	if (folio_ocr_save(doc, o->cache))
		app_log("ocr: couldn't save: %s", folio_error(app.fc));
	else {
		_swix(OS_File, _INR(0, 2), 18, o->cache, 0xfff);
		app_log("ocr: %d page(s) saved", folio_ocr_pages(doc));
	}
	o->unsaved = 0;
	o->since_save = 0;
}

ocr_win *ocr_win_new(docwin *w)
{
	ocr_win *o;
	folio_doc *doc = docwin_doc(w);
	if (!ocr.available || !doc || !folio_ocr_supported(doc))
		return NULL;
	o = calloc(1, sizeof *o);
	if (!o)
		return NULL;
	o->w = w;
	o->urgent = o->asked = o->running = -1;
	grow(o, docwin_pages(w));
	load(o);
	return o;
}

void ocr_win_close(ocr_win *o)
{
	if (!o)
		return;
	save(o);
	free(o->st);
	free(o);
}

/* Before saving the PDF (core lock held): its text into the cache file. */
void ocr_win_before_reopen(ocr_win *o)
{
	if (!o)
		return;
	o->unsaved = 1;
	save(o);
}

/* After saving the PDF: the document was closed and opened again, perhaps
 * under a new name. Its text comes back from where before_reopen put it,
 * and is saved under the new name when the window closes. Core lock held. */
void ocr_win_after_reopen(ocr_win *o)
{
	int i;
	char was[256];
	if (!o)
		return;
	snprintf(was, sizeof was, "%s", o->cache);
	memset(o->st, S_UNKNOWN, o->n);
	o->running = -1;
	if (was[0] && folio_ocr_load(docwin_doc(o->w), was) > 0) {
		for (i = 0; i < o->n; i++) {
			folio_loc loc;
			if (folio_loc_from_number(docwin_doc(o->w), i, &loc) == 0 &&
					folio_page_has_ocr(docwin_doc(o->w), loc))
				o->st[i] = S_DONE;
		}
		o->unsaved = 1;    /* the file may have a new name now */
	}
}

/* ---- Choosing pages -------------------------------------------------------- */

static void set_note(ocr_win *o, int page)
{
	char note[48];
	if (page < 0)
		note[0] = 0;
	else if (o->all)
		snprintf(note, sizeof note, " (reading text: page %d of %d)", page + 1, docwin_pages(o->w));
	else
		snprintf(note, sizeof note, " (reading text on page %d)", page + 1);
	/* Printing has its own note: leave it alone. */
	if (!print_busy_for(o->w))
		docwin_set_note(o->w, note);
}

static int submit(ocr_win *o, int page, int only_scans)
{
	render_job j;
	folio_doc *doc = docwin_doc(o->w);
	memset(&j, 0, sizeof j);
	if (folio_loc_from_number(doc, page, &j.loc))
		return 0;
	j.kind = JOB_OCR;
	j.owner = o->w;
	j.fc = app.fc;
	j.doc = doc;
	j.page = page;
	j.only_scans = only_scans;
	data_dir(j.datadir, sizeof j.datadir);
	snprintf(j.lang, sizeof j.lang, "eng");
	if (worker_submit(&j))
		return 0;
	o->running = page;
	set_note(o, page);
	return 1;
}

static int wanted(ocr_win *o, int page)
{
	return page >= 0 && page < o->n && o->st[page] == S_UNKNOWN;
}

int ocr_win_next(ocr_win *o)
{
	int cur, k, dir;
	if (!o)
		return 0;
	/* The worker is idle, so a job of ours was stopped and thrown away
	 * (core_now): that page is still to do. */
	if (o->running >= 0) {
		o->running = -1;
		set_note(o, -1);
	}
	grow(o, docwin_pages(o->w));
	if (wanted(o, o->urgent) && submit(o, o->urgent, 1))
		return 1;
	if (o->asked >= 0 && o->asked < o->n && submit(o, o->asked, 0))
		return 1;
	/* Reading aloud is short of time: only what it's waiting for. */
	if (app.speech_urgent)
		return 0;
	if (app.ch.ocr_auto) {
		cur = docwin_current_page(o->w);
		dir = docwin_reading_dir(o->w);
		for (k = 0; k <= 2; k++)
			if (wanted(o, cur + k * dir) && submit(o, cur + k * dir, 1))
				return 1;
	}
	if (o->all) {
		while (o->all_next < o->n && o->st[o->all_next] != S_UNKNOWN)
			o->all_next++;
		if (o->all_next < o->n && !docwin_is_counting(o->w))
			return submit(o, o->all_next, 1);
		if (o->all_next >= o->n && !docwin_is_counting(o->w)) {
			unsigned t;
			int i, done = 0;
			_swix(OS_ReadMonotonicTime, _OUT(0), &t);
			for (i = 0; i < o->n; i++)
				done += o->st[i] == S_DONE;
			app_log("ocr: all pages looked at in %u s: %d recognised", (t - o->t0) / 100, done);
			o->all = 0;
			set_note(o, -1);
			core_now();
			save(o);
			core_unlock();
		}
	}
	return 0;
}

void ocr_win_take(ocr_win *o, const render_job *r)
{
	int p = r->page;
	if (!o)
		return;
	o->running = -1;
	if (p < 0 || p >= o->n)
		return;
	if (r->aborted) {
		app_log("ocr: page %d stopped (wanted again later)", p + 1);
	} else if (r->chars == FOLIO_OCR_NOT_SCAN) {
		o->st[p] = S_NOT_SCAN;
	} else if (r->chars < 0) {
		o->st[p] = S_FAILED;
		app_log("ocr: page %d: %s", p + 1, r->err);
		if (o->asked == p) {
			char msg[200];
			snprintf(msg, sizeof msg, "Folio couldn't read the text on page %d: %s", p + 1, r->err);
			tbx_report(msg);
		}
	} else {
		o->st[p] = r->chars > 0 ? S_DONE : S_EMPTY;
		app_log("ocr: page %d: %d characters in %d cs (%s)", p + 1, r->chars, r->cs,
			use_fast() ? "Fast" : "Accurate");
		if (++o->since_save >= 10 && core_trylock()) {
			save(o);
			core_unlock();
		}
		docwin_text_changed(o->w, p);
	}
	if (!r->aborted) {
		if (o->asked == p)
			o->asked = -1;
		if (o->urgent == p)
			o->urgent = -1;
	}
	set_note(o, -1);
}

/* Whether the worker has an OCR job that a page to draw should stop. */
int ocr_win_running(ocr_win *o)
{
	return o && o->running >= 0;
}

int ocr_win_wait(ocr_win *o, int page)
{
	if (!o || page < 0)
		return 0;
	grow(o, docwin_pages(o->w));
	if (page >= o->n)
		return 0;
	if (o->running == page || o->asked == page)
		return 1;
	if (o->st[page] != S_UNKNOWN)
		return 0;
	o->urgent = page;
	return 1;
}

void ocr_win_this_page(ocr_win *o)
{
	int page;
	if (!o)
		return;
	page = docwin_current_page(o->w);
	if (o->running == page)
		return;
	o->asked = page;
	if (page < o->n)
		o->st[page] = S_UNKNOWN;
}

void ocr_win_all(ocr_win *o)
{
	if (!o)
		return;
	o->all = !o->all;
	if (o->all) {
		int i;
		/* Pages that failed get another go. */
		for (i = 0; i < o->n; i++)
			if (o->st[i] == S_FAILED)
				o->st[i] = S_UNKNOWN;
		o->all_next = 0;
		_swix(OS_ReadMonotonicTime, _OUT(0), &o->t0);
		app_log("ocr: all pages (%s)", use_fast() ? "Fast" : "Accurate");
	} else {
		app_log("ocr: all pages stopped at page %d", o->all_next + 1);
		if (o->running < 0)
			set_note(o, -1);
	}
}

/* Pages turned (Rotate, undo): one recognised before whose text has gone
 * is read again when it's wanted. Core lock held. */
void ocr_win_resync(ocr_win *o)
{
	int i;
	folio_doc *doc;
	int exact, pages;
	if (!o)
		return;
	doc = docwin_doc(o->w);
	/* Pages added (or taken away again). */
	pages = folio_page_count(doc, &exact);
	if (pages > o->n)
		grow(o, pages);
	else if (pages >= 0 && pages < o->n)
		o->n = pages;
	if (o->all_next > o->n)
		o->all_next = o->n;
	for (i = 0; i < o->n; i++) {
		folio_loc loc;
		if (o->st[i] == S_DONE && !folio_loc_from_number(doc, i, &loc) &&
				!folio_page_has_ocr(doc, loc))
			o->st[i] = S_UNKNOWN;
	}
	if (o->all)
		o->all_next = 0;
}

int ocr_win_all_on(ocr_win *o)
{
	return o && o->all;
}

/* After a change of speed: pages that found nothing get another look. */
static void forget_empty(ocr_win *o)
{
	int i;
	for (i = 0; i < o->n; i++)
		if (o->st[i] == S_EMPTY || o->st[i] == S_FAILED)
			o->st[i] = S_UNKNOWN;
}

void ocr_settings_changed(ocr_win *o)
{
	if (o)
		forget_empty(o);
}
