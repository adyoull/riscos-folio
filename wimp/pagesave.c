/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Pages out, pictures in (core/pages.c):
 *  - Save pages as PDF... and Save page pictures... (document menu): a small
 *    box for which pages ("1-10, 15, 20-", or all) and, for pictures, the
 *    resolution for pages that aren't one picture; then a SaveAs box: a PDF
 *    file, or a directory of pictures (Page001, Page002...): JPEG (&C85) when
 *    the page is one JPEG picture, stored as it was; else PNG (&B60).
 *  - Save pages as text... (document menu): the same box, then a Text file
 *    (&FFF) of those pages' text, a paragraph a line, in the desktop's
 *    alphabet; scanned pages give their recognised text.
 *  - A directory of pictures dropped on Folio's icon or a document window
 *    becomes a new PDF, a page per picture in name order (Page2 before
 *    Page10), opened in a window to look at and save. */
#include "app.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define Class_SaveAs 0x82bc0

enum { S_LABEL = 0, S_PAGES, S_OF, S_DPI_LABEL, S_DPI, S_DPI_UNITS, S_CANCEL, S_SAVE };

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

/* The gadgets in this order; the PDF box has the first three and the
 * buttons (S_CANCEL, S_SAVE are made with these ids either way). */
typedef struct {
	label_gadget label;
	writable_gadget pages;
	display_gadget of;
	button_gadget cancel, save;
	label_gadget dpi_label;
	writable_gadget dpi;
	label_gadget dpi_units;
} box_gadgets;

/* The three kinds of saving, each with its own box. */
enum { K_PDF, K_PICTURES, K_TEXT, K_COUNT };

static struct {
	ObjectId box[K_COUNT];
	ObjectId saveas[K_COUNT];
	box_gadgets g[K_COUNT];
	char pages_buf[K_COUNT][64], of_buf[K_COUNT][32], dpi_buf[8];
	docwin *w;
	int kind;                   /* which is being used */
	unsigned char *sel;         /* the pages chosen */
	int count, n, dpi;
} ps;

static const char *leafname(const char *path)
{
	const char *p = strrchr(path, '.'), *c = strrchr(path, ':');
	if (c && (!p || c > p))
		p = c;
	return p ? p + 1 : path;
}

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

#define BOX_W 600

static ObjectId create_box(int kind)
{
	static tbx_window t[K_COUNT];
	static char title[K_COUNT][40] = { "Save pages as PDF", "Save page pictures", "Save pages as text" };
	static const char *const pages_help[K_COUNT] = {
		"Type the pages to put in the new PDF, such as 1-10, or 1-3, 20, 45-50.|MLeave it as 'all' for every page.",
		"Type the pages whose pictures you want, such as 1-10, or 1-3, 20, 45-50.|MLeave it as 'all' for every page.",
		"Type the pages whose text you want, such as 1-10, or 1-3, 20, 45-50.|MLeave it as 'all' for every page." };
	static const char *const save_help[K_COUNT] = {
		"Click (or press Return) to choose where the new PDF goes.",
		"Click (or press Return) to choose where the directory of pictures goes.",
		"Click (or press Return) to choose where the text file goes." };
	static const char *const box_help[K_COUNT] = {
		"Use this box to make a new PDF of some of this document's pages.",
		"Use this box to save pages' pictures as files: a scanned page's own JPEG, unchanged.",
		"Use this box to save the text of some of this document's pages as a text file." };
	int pictures = kind == K_PICTURES;
	box_gadgets *g = &ps.g[kind];
	tbx_window *tw = &t[kind];
	int box_h = pictures ? 248 : 184, by = -box_h + 16;
	ObjectId id = 0;
	_kernel_oserror *e;

	memset(g, 0, sizeof *g);
	hdr(&g->label.h, 320, sizeof g->label, S_LABEL, 16, -84, 112, -36, NULL);
	g->label.label = "Pages";
	hdr(&g->pages.h, 512, sizeof g->pages, S_PAGES, 112, -88, 400, -32, pages_help[kind]);
	g->pages.text = ps.pages_buf[kind];
	g->pages.max_text = sizeof ps.pages_buf[kind];
	g->pages.allowable = "0-9,\\- aAlL";
	g->pages.max_allowable = 12;
	g->pages.before = -1;
	g->pages.after = pictures ? S_DPI : -1;
	hdr(&g->of.h, 448, sizeof g->of, S_OF, 408, -84, BOX_W - 16, -36, NULL);
	g->of.text = ps.of_buf[kind];
	g->of.max_text = sizeof ps.of_buf[kind];
	hdr(&g->cancel.h, 128, sizeof g->cancel, S_CANCEL, BOX_W - 360, by, BOX_W - 200, by + 68,
		"Click to close this box.");
	g->cancel.h.flags = 2;          /* Escape */
	g->cancel.text = "Cancel";
	g->cancel.max_text = 7;
	g->cancel.event = EV_PAGES_CANCEL;
	hdr(&g->save.h, 128, sizeof g->save, S_SAVE, BOX_W - 184, by, BOX_W - 24, by + 68, save_help[kind]);
	g->save.h.flags = 1;            /* default: Return */
	g->save.text = "Save";
	g->save.max_text = 5;
	g->save.event = EV_PAGES_OK;
	if (pictures) {
		static const char dpi_help[] = "Pages that are one picture (a scanned page) are saved as that "
			"picture, just as it's stored: a JPEG stays the same JPEG.|MOther pages are drawn at this "
			"resolution and saved as PNG.";
		hdr(&g->dpi_label.h, 320, sizeof g->dpi_label, S_DPI_LABEL, 16, -148, 400, -100, dpi_help);
		g->dpi_label.label = "Draw other pages at";
		hdr(&g->dpi.h, 512, sizeof g->dpi, S_DPI, 400, -152, 488, -96, dpi_help);
		g->dpi.text = ps.dpi_buf;
		g->dpi.max_text = sizeof ps.dpi_buf;
		g->dpi.allowable = "0-9";
		g->dpi.max_allowable = 4;
		g->dpi.before = S_PAGES;
		g->dpi.after = -1;
		hdr(&g->dpi_units.h, 320, sizeof g->dpi_units, S_DPI_UNITS, 496, -148, BOX_W - 16, -100, NULL);
		g->dpi_units.label = "dpi";
	}

	memset(tw, 0, sizeof *tw);
	tw->flags = (1 << 1) | (1 << 2);
	tw->help_message = box_help[kind];
	tw->max_help = (int)strlen(tw->help_message) + 1;
	tw->num_gadgets = pictures ? 8 : 5;
	tw->gadgets = g;
	tw->default_focus = S_PAGES;
	tw->show_event = -1;
	tw->hide_event = -1;
	tw->window.vis_x1 = BOX_W;
	tw->window.vis_y1 = box_h;
	tw->window.behind = -1;
	tw->window.window_flags = 0x80000002u | (1u << 4) | (1u << 26);
	tw->window.title_fore = 7;
	tw->window.title_back = 2;
	tw->window.work_fore = 7;
	tw->window.work_back = 1;
	tw->window.scroll_outer = 3;
	tw->window.scroll_inner = 1;
	tw->window.title_focus = 12;
	tw->window.work_y0 = -box_h;
	tw->window.work_x1 = BOX_W;
	tw->window.title_flags = 0x00000119;
	tw->window.work_flags = 0;
	tw->window.sprite_area = (void *)1;
	tw->window.title_text = title[kind];
	tw->window.title_validation = "";
	tw->window.title_buflen = sizeof title[kind];
	e = tbx_create(Class_Window, 102, 0, tw, sizeof *tw, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	return id;
}

void pagesave_open(docwin *w, int kind)
{
	int dst[9], pos[2], pages = docwin_pages(w), k;
	char of[32];
	if (kind < 0 || kind >= K_COUNT)
		return;
	if (docwin_is_counting(w)) {
		tbx_report("Folio is still counting this book's pages. Try again in a moment.");
		return;
	}
	if (pages <= 0)
		return;
	if (!ps.box[kind]) {
		if (!ps.pages_buf[kind][0])
			strcpy(ps.pages_buf[kind], "all");
		if (!ps.dpi_buf[0])
			strcpy(ps.dpi_buf, "150");
		if (!(ps.box[kind] = create_box(kind)))
			return;
	}
	if (ps.w && ps.w != w)
		for (k = 0; k < K_COUNT; k++)
			if (ps.box[k])
				tbx_hide(ps.box[k]);
	ps.w = w;
	ps.kind = kind;
	snprintf(of, sizeof of, "of %d", pages);
	tbx_method(ps.box[kind], 448 /* DisplayField_SetValue */, S_OF, (int)of, 0);
	dst[0] = docwin_wimp(w);
	_swix(Wimp_GetWindowState, _IN(1), dst);
	pos[0] = dst[1] + 64;
	pos[1] = dst[4] - 64;
	tbx_show(ps.box[kind], 1, 2, pos, 0, -1);
}

static void show_saveas(void)
{
	static struct {
		int flags;
		const char *filename;
		int filetype;
		const char *title;
		int max_title;
		const char *window;
	} t[K_COUNT];
	static const char *const name[K_COUNT] = { "Pages", "Pictures", "Text" };
	static const int type[K_COUNT] = { 0xadf, 0x1000, 0xfff };
	static const char *const title[K_COUNT] = { "Save pages as PDF", "Save page pictures", "Save pages as text" };
	int ptr[5], pos[2], k = ps.kind;
	if (!ps.saveas[k]) {
		_kernel_oserror *e;
		memset(&t[k], 0, sizeof t[k]);
		t[k].flags = 4;              /* no Selection option; Folio writes it itself */
		t[k].filename = name[k];
		t[k].filetype = type[k];
		t[k].title = title[k];
		t[k].max_title = 32;
		e = tbx_create(Class_SaveAs, 100, 0, &t[k], sizeof t[k], &ps.saveas[k]);
		if (e) {
			tbx_report_error(e);
			ps.saveas[k] = 0;
			return;
		}
	}
	_swix(Wimp_GetPointerInfo, _IN(1), ptr);
	pos[0] = ptr[0] - 64;
	pos[1] = ptr[1] + 64;
	tbx_show(ps.saveas[k], 0, 2, pos, 0, -1);
}

/* Save in the Pages box: check the pages, then ask where. */
void pagesave_event(int code)
{
	char text[64], dpi[8];
	int k = ps.kind, n;
	if (code != EV_PAGES_OK || !ps.w)
		return;
	text[0] = 0;
	_swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, ps.box[k], 513, S_PAGES, text, (int)sizeof text);
	text[sizeof text - 1] = 0;
	snprintf(ps.pages_buf[k], sizeof ps.pages_buf[k], "%s", text);
	ps.count = docwin_pages(ps.w);
	free(ps.sel);
	ps.sel = malloc(ps.count > 0 ? (size_t)ps.count : 1);
	if (!ps.sel)
		return;
	n = folio_parse_pages(text, ps.count, ps.sel);
	if (n <= 0) {
		char m[200];
		snprintf(m, sizeof m, "Folio can't tell which pages '%s' means. Type page numbers and ranges "
			"from 1 to %d, such as 1-10, or 1-3, 20, 45-50.", text, ps.count);
		tbx_report(m);
		return;
	}
	ps.n = n;
	ps.dpi = 150;
	if (k == K_PICTURES) {
		dpi[0] = 0;
		_swix(Toolbox_ObjectMiscOp, _INR(0, 5), 0, ps.box[k], 513, S_DPI, dpi, (int)sizeof dpi);
		dpi[sizeof dpi - 1] = 0;
		ps.dpi = atoi(dpi);
		if (ps.dpi < 36)
			ps.dpi = 36;
		if (ps.dpi > 600)
			ps.dpi = 600;
		snprintf(ps.dpi_buf, sizeof ps.dpi_buf, "%d", ps.dpi);
	}
	tbx_hide(ps.box[k]);
	show_saveas();
}

static int save_pdf(const char *path)
{
	int ok;
	_swix(Hourglass_On, 0);
	core_now();
	ok = folio_save_pages(docwin_doc(ps.w), ps.sel, ps.count, path) == 0;
	core_unlock();
	_swix(Hourglass_Off, 0);
	app_log("save pages: %d of %d to %s%s%s", ps.n, ps.count, path, ok ? "" : ": ",
		ok ? "" : folio_error(app.fc));
	if (ok) {
		_swix(OS_File, _INR(0, 2), 18, path, 0xadf);
	} else {
		char m[300];
		snprintf(m, sizeof m, "Folio couldn't save the pages: %s", folio_error(app.fc));
		tbx_report(m);
	}
	return ok;
}

static int save_pictures(const char *path)
{
	folio_doc *doc = docwin_doc(ps.w);
	int i, done = 0, kinds[4] = { 0 }, width = 3, ok = 1;
	_kernel_oserror *e;
	char file[300];
	if (ps.count >= 1000)
		width = ps.count >= 10000 ? 5 : 4;
	/* The directory (it may be there already). */
	e = _swix(OS_File, _INR(0, 1) | _IN(4), 8, path, 0);
	if (e) {
		char m[300];
		snprintf(m, sizeof m, "Folio couldn't make the directory: %s", e->errmess);
		tbx_report(m);
		return 0;
	}
	_swix(Hourglass_On, 0);
	core_now();
	for (i = 0; i < ps.count && ok; i++) {
		folio_loc loc;
		int kind;
		if (!ps.sel[i])
			continue;
		_swix(Hourglass_Percentage, _IN(0), done * 100 / ps.n);
		snprintf(file, sizeof file, "%s.Page%0*d", path, width, i + 1);
		if (folio_loc_from_number(doc, i, &loc)) {
			ok = 0;
			break;
		}
		kind = folio_save_page_image(doc, loc, ps.dpi, file);
		if (kind < 0) {
			ok = 0;
			break;
		}
		kinds[kind]++;
		_swix(OS_File, _INR(0, 2), 18, file, kind == FOLIO_PAGEIMG_JPEG ? 0xc85 : 0xb60);
		done++;
	}
	core_unlock();
	_swix(Hourglass_Off, 0);
	app_log("page pictures: %d of %d to %s: %d JPEG as stored, %d pictures as PNG, %d drawn at %d dpi%s%s",
		done, ps.n, path, kinds[FOLIO_PAGEIMG_JPEG], kinds[FOLIO_PAGEIMG_IMAGE_PNG],
		kinds[FOLIO_PAGEIMG_PAGE_PNG], ps.dpi, ok ? "" : "; stopped: ", ok ? "" : folio_error(app.fc));
	if (!ok) {
		char m[300];
		snprintf(m, sizeof m, "Folio couldn't save page %d's picture: %s", i + 1, folio_error(app.fc));
		tbx_report(m);
	}
	return ok;
}

static void text_progress(int done, void *arg)
{
	_swix(Hourglass_Percentage, _IN(0), done * 100 / *(int *)arg);
}

static int save_text(const char *path)
{
	char *utf8, *text = NULL;
	int empty = 0, ok = 0;
	_kernel_oserror *e = NULL;
	_swix(Hourglass_On, 0);
	core_now();
	utf8 = folio_pages_text(docwin_doc(ps.w), ps.sel, ps.count, &empty, text_progress, &ps.n);
	core_unlock();
	if (utf8) {
		/* As the desktop shows text: Latin-1, unless it's set to UTF-8. */
		text = app_desktop_text(utf8);
		free(utf8);
	}
	if (text) {
		e = _swix(OS_File, _INR(0, 2) | _INR(4, 5), 10, path, 0xfff, text, text + strlen(text));
		ok = !e;
	}
	_swix(Hourglass_Off, 0);
	app_log("save text: %d of %d pages (%d with no text), %d bytes to %s%s%s", ps.n, ps.count, empty,
		text ? (int)strlen(text) : 0, path, ok ? "" : ": ", ok ? "" : e ? e->errmess : folio_error(app.fc));
	if (!ok) {
		char m[300];
		snprintf(m, sizeof m, "Folio couldn't save the text: %s", e ? e->errmess :
			utf8 || text ? "not enough memory" : folio_error(app.fc));
		tbx_report(m);
	} else if (empty == ps.n) {
		tbx_report(ocr_available() ?
			"Those pages have no text: they're probably scanned pictures of pages. Choose Text recognition > "
			"Recognise all pages first, then save the text again." :
			"Those pages have no text: they're probably scanned pictures of pages. With the OCR plugin, "
			"Folio can recognise the text in them.");
	}
	free(text);
	return ok;
}

/* SaveAs events for these boxes. */
int pagesave_saveas_event(int *b)
{
	int k;
	for (k = 0; k < K_COUNT; k++)
		if (ps.saveas[k] && app.idb.self_id == ps.saveas[k])
			break;
	if (k == K_COUNT)
		return 0;
	if (b[2] == 0x82bc2) {            /* SaveAs_SaveToFile */
		const char *path = (const char *)(b + 4);
		int ok = 0;
		if (ps.w && ps.sel)
			ok = k == K_PICTURES ? save_pictures(path) : k == K_TEXT ? save_text(path) : save_pdf(path);
		_swix(Toolbox_ObjectMiscOp, _INR(0, 3), ok ? 1 : 0, ps.saveas[k], 12, path);
	}
	return 1;
}

void pagesave_forget(docwin *w)
{
	int k;
	if (ps.w != w)
		return;
	ps.w = NULL;
	for (k = 0; k < K_COUNT; k++) {
		if (ps.box[k])
			tbx_hide(ps.box[k]);
		if (ps.saveas[k])
			tbx_hide(ps.saveas[k]);
	}
}

/* ---- A directory of pictures as a new PDF -------------------------------- */

static int picture_type(int type, const char *leaf)
{
	const char *dot;
	/* JPEG, PNG, GIF, BMP, TIFF, PBMPlus (PBM, PGM, PPM). */
	if (type == 0xc85 || type == 0xb60 || type == 0x695 || type == 0x69c || type == 0xff0 ||
			type == 0x69e)
		return 1;
	/* Untyped, from elsewhere: by the name's ending (Page1/jpg). */
	dot = strrchr(leaf, '/');
	if ((type == 0xffd || type == -1) && dot) {
		static const char *const ends[] = { "jpg", "jpeg", "png", "gif", "bmp", "tif", "tiff",
			"pbm", "pgm", "ppm", "pnm", "pam", NULL };
		int i;
		for (i = 0; ends[i]; i++) {
			const char *a = dot + 1, *b = ends[i];
			while (*a && *b && tolower((unsigned char)*a) == *b)
				a++, b++;
			if (!*a && !*b)
				return 1;
		}
	}
	return 0;
}

int pagesave_is_pdf(int type, const char *leaf)
{
	const char *dot = strrchr(leaf, '/');
	return type == 0xadf || ((type == 0xffd || type == -1) && dot &&
		(dot[1] == 'p' || dot[1] == 'P') && (dot[2] == 'd' || dot[2] == 'D') &&
		(dot[3] == 'f' || dot[3] == 'F') && !dot[4]);
}

static int natural_cmp(const void *pa, const void *pb);

int pagesave_natural(const char *a, const char *b)
{
	return natural_cmp(&a, &b);
}

int pagesave_is_picture(int type, const char *leaf)
{
	return picture_type(type, leaf);
}

/* Names in reading order: numbers by value (Page2 before Page10), letters
 * without regard to case. */
static int natural_cmp(const void *pa, const void *pb)
{
	const char *a = *(const char *const *)pa, *b = *(const char *const *)pb;
	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			const char *ea = a, *eb = b;
			size_t la, lb;
			while (*a == '0' && isdigit((unsigned char)a[1]))
				a++;
			while (*b == '0' && isdigit((unsigned char)b[1]))
				b++;
			for (ea = a; isdigit((unsigned char)*ea); ea++)
				;
			for (eb = b; isdigit((unsigned char)*eb); eb++)
				;
			la = (size_t)(ea - a);
			lb = (size_t)(eb - b);
			if (la != lb)
				return la < lb ? -1 : 1;
			{
				int c = strncmp(a, b, la);
				if (c)
					return c;
			}
			a = ea;
			b = eb;
		} else {
			int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
			if (ca != cb)
				return ca - cb;
			a++;
			b++;
		}
	}
	return (unsigned char)*a - (unsigned char)*b;
}

/* The pictures (and, with pdfs, the PDFs) in dir, in name order, as full
 * paths in *out (free with pagesave_free_list). Returns how many. */
int pagesave_list(const char *dir, int pdfs, char ***out)
{
	char **names = NULL, *buf;
	int n = 0, cap = 0, next = 0, i;
	/* OS_GBPB 10: load address, exec, length, attributes, type, name. */
	*out = NULL;
	buf = malloc(4096);
	if (!buf)
		return 0;
	while (next != -1) {
		int got = 0;
		char *p;
		if (_swix(OS_GBPB, _INR(0, 6) | _OUTR(3, 4), 10, dir, buf, 64, next, 4096, "*", &got, &next))
			break;
		for (p = buf, i = 0; i < got; i++) {
			unsigned load = *(unsigned *)p;
			int objtype = *(int *)(p + 16);
			const char *name = p + 20;
			int ftype = (load & 0xfff00000u) == 0xfff00000u ? (int)((load >> 8) & 0xfff) : -1;
			if (objtype == 1 && (picture_type(ftype, name) || (pdfs && pagesave_is_pdf(ftype, name)))) {
				if (n == cap) {
					char **nn = realloc(names, (size_t)(cap = cap ? cap * 2 : 64) * sizeof *names);
					if (!nn)
						break;
					names = nn;
				}
				names[n] = malloc(strlen(dir) + strlen(name) + 2);
				if (!names[n])
					break;
				sprintf(names[n], "%s.%s", dir, name);
				n++;
			}
			p += (20 + strlen(name) + 1 + 3) & ~3;
		}
	}
	free(buf);
	if (n > 1)
		qsort(names, (size_t)n, sizeof *names, natural_cmp);
	*out = names;
	return n;
}

void pagesave_free_list(char **names, int n)
{
	int i;
	for (i = 0; i < n; i++)
		free(names[i]);
	free(names);
}

/* A new file in the scrap directory, named leaf (so that's its window's
 * title): <Wimp$ScrapDir>.FolioNew.<n>.<leaf>. */
void pagesave_new_path(const char *leaf, char *out, size_t size)
{
	static int made;
	char sub[300];
	const char *scrap = getenv("Wimp$ScrapDir");
	if (!scrap || !*scrap)
		scrap = "<Wimp$ScrapDir>";
	snprintf(sub, sizeof sub, "%s.FolioNew", scrap);
	_swix(OS_File, _INR(0, 1) | _IN(4), 8, sub, 0);
	snprintf(sub, sizeof sub, "%s.FolioNew.%d", scrap, ++made);
	_swix(OS_File, _INR(0, 1) | _IN(4), 8, sub, 0);
	snprintf(out, size, "%s.%s", sub, leaf);
}

int pagesave_folder(const char *dir)
{
	char **names = NULL, out[600];
	const char *leaf = leafname(dir);
	int n, i, ok = 0;
	docwin *w;

	n = pagesave_list(dir, 0, &names);
	if (n == 0) {
		char m[300];
		snprintf(m, sizeof m, "There are no pictures in '%s'. Folio makes a PDF from the JPEG, PNG, GIF, BMP, "
			"TIFF and PBM/PGM/PPM files in a directory dragged to it, a page for each.", leaf);
		tbx_report(m);
		free(names);
		return 1;
	}

	/* Made in the scrap directory under the directory's name; saving puts
	 * it where it's wanted. */
	pagesave_new_path(leaf, out, sizeof out);
	_swix(Hourglass_On, 0);
	core_now();
	ok = folio_images_to_pdf(app.fc, (const char *const *)names, n, out) == 0;
	core_unlock();
	_swix(Hourglass_Off, 0);
	app_log("new PDF from %s: %d pictures%s%s", dir, n, ok ? "" : ": ", ok ? "" : folio_error(app.fc));
	pagesave_free_list(names, n);
	(void)i;
	if (!ok) {
		char m[400];
		snprintf(m, sizeof m, "Folio couldn't make a PDF from '%s': %s", leaf, folio_error(app.fc));
		tbx_report(m);
		return 1;
	}
	_swix(OS_File, _INR(0, 2), 18, out, 0xadf);
	w = docwin_open(out);
	if (w)
		docwin_mark_new(w);
	return 1;
}
