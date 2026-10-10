/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Find (wimp/find.c) against the real core and tests/data/doc.pdf, with
 * stand-ins for the Toolbox and the document window. Run by `make test`:
 *   find_test tests/data/doc.pdf
 * doc.pdf: page 1 "Folio test page one" and "serendipity is the word under
 * test"; pages 2 and 3 "Folio test page N". */
#include "app.h"
#include "worker.h"
#include <swis.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

app_state app;
static folio_doc *doc;
static int cur_page;
static char query[128];
static char status_text[128];
static int shown_page = -1, shown_n;
static folio_quad shown_q;

/* ---- Stand-ins ------------------------------------------------------- */

_kernel_oserror *_swix(int swi, unsigned mask, ...)
{
	va_list ap;
	va_start(ap, mask);
	if (swi == OS_ReadMonotonicTime) {
		struct timespec ts;
		unsigned *out = va_arg(ap, unsigned *);
		clock_gettime(CLOCK_MONOTONIC, &ts);
		*out = (unsigned)(ts.tv_sec * 100 + ts.tv_nsec / 10000000);
	} else if (swi == Toolbox_ObjectMiscOp) {
		int flags = va_arg(ap, int), obj = va_arg(ap, int), method = va_arg(ap, int);
		int comp = va_arg(ap, int);
		char *buf = va_arg(ap, char *);
		int size = va_arg(ap, int);
		(void)flags; (void)obj; (void)comp;
		if (method == 513)
			snprintf(buf, (size_t)size, "%s", query);
	} else if (swi == Wimp_GetWindowState) {
		int *st = va_arg(ap, int *);
		memset(st, 0, 9 * sizeof *st);
	}
	va_end(ap);
	return NULL;
}

_kernel_oserror *tbx_create(int class_id, int version, int flags, void *body, int size, ObjectId *id)
{
	*id = 1;
	return NULL;
}
_kernel_oserror *tbx_show(ObjectId id, int flags, int type, const void *data, ObjectId parent,
		ComponentId pc) { return NULL; }
_kernel_oserror *tbx_hide(ObjectId id) { return NULL; }
_kernel_oserror *tbx_method(ObjectId id, int method, int r3, int r4, int r5)
{
	if (method == 448)
		snprintf(status_text, sizeof status_text, "%s", (const char *)r4);
	return NULL;
}
void tbx_report_error(const _kernel_oserror *e) {}
void app_log(const char *fmt, ...) {}

folio_doc *docwin_doc(docwin *w) { return doc; }
int docwin_wimp(docwin *w) { return 0; }
int docwin_current_page(docwin *w) { return cur_page; }
void docwin_show_hit(docwin *w, int page, const folio_quad *q, int n)
{
	shown_page = page;
	shown_n = n;
	shown_q = q[0];
}

/* ---- Tests ----------------------------------------------------------- */

static docwin *const W = (docwin *)1;

/* Presses Next (dir 1) or Previous (-1) and runs the search to its end. */
static void press(int dir)
{
	int polls = 0;
	shown_page = -1;
	find_event(dir > 0 ? EV_FIND_NEXT : EV_FIND_PREV);
	while (find_background() && polls < 1000)
		polls++;
	CHECK(polls < 1000, "search didn't end");
}

int main(int argc, char **argv)
{
	folio_ctx *fc = folio_new(32 << 20);
	float x_first;
	doc = fc ? folio_open(fc, argc > 1 ? argv[1] : "tests/data/doc.pdf", NULL) : NULL;
	CHECK(doc != NULL, "open: %s", fc ? folio_error(fc) : "no context");
	if (!doc)
		return 1;
	/* The document window counts the pages in null polls; here, now. */
	CHECK(folio_count_step(doc, 100) == 1, "count");
	find_open(W);

	/* "TEST": twice on page 1, once on pages 2 and 3. */
	snprintf(query, sizeof query, "TEST");
	cur_page = 0;
	press(1);
	CHECK(shown_page == 0 && shown_n == 1, "1st: page %d n %d", shown_page + 1, shown_n);
	CHECK(!strcmp(status_text, "Page 1: 1 of 2 on this page"), "1st status '%s'", status_text);
	x_first = shown_q.ul.x;
	press(1);
	CHECK(shown_page == 0 && shown_q.ul.x != x_first, "2nd: page %d", shown_page + 1);
	CHECK(!strcmp(status_text, "Page 1: 2 of 2 on this page"), "2nd status '%s'", status_text);
	press(1);
	CHECK(shown_page == 1 && !strcmp(status_text, "Page 2"), "3rd: page %d '%s'", shown_page + 1, status_text);
	press(1);
	CHECK(shown_page == 2, "4th: page %d", shown_page + 1);
	press(1);
	CHECK(shown_page == 0 && shown_q.ul.x == x_first && strstr(status_text, "wrapped round"),
		"5th wraps: page %d '%s'", shown_page + 1, status_text);
	/* And back. */
	press(-1);
	CHECK(shown_page == 2 && strstr(status_text, "wrapped round"), "previous wraps: page %d '%s'",
		shown_page + 1, status_text);
	press(-1);
	CHECK(shown_page == 1, "previous: page %d", shown_page + 1);

	/* A new word starts from the page being read. */
	snprintf(query, sizeof query, "serendipity");
	cur_page = 1;
	press(1);
	CHECK(shown_page == 0 && strstr(status_text, "wrapped round"), "serendipity: page %d '%s'",
		shown_page + 1, status_text);
	/* Only one: Next comes round to it again. */
	press(1);
	CHECK(shown_page == 0, "serendipity again: page %d", shown_page + 1);

	/* Previous from page 1 with the hit on page 1: the page being read counts. */
	snprintf(query, sizeof query, "page one");
	cur_page = 0;
	press(-1);
	CHECK(shown_page == 0 && !strstr(status_text, "wrapped"), "previous on this page: %d '%s'",
		shown_page + 1, status_text);

	/* Not there at all: stops. */
	snprintf(query, sizeof query, "zzzz");
	press(1);
	CHECK(shown_page == -1 && !strcmp(status_text, "Not found."), "not found: '%s'", status_text);
	press(-1);
	CHECK(shown_page == -1 && !strcmp(status_text, "Not found."), "not found back: '%s'", status_text);

	/* Nothing typed. */
	query[0] = 0;
	press(1);
	CHECK(!strcmp(status_text, "Type the text to find first."), "empty: '%s'", status_text);

	/* The window closes: the search forgets it. */
	snprintf(query, sizeof query, "test");
	find_event(EV_FIND_NEXT);
	find_forget(W);
	CHECK(find_background() == 0, "still searching after forget");

	folio_close(doc);
	folio_drop(fc);
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
