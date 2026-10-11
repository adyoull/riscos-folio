/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* The Bookshelf: Folio's main window (Select on the icon bar icon opens it;
 * Style Guide p20). Books are shown as covers in a grid that reflows with
 * the window, with title, author and how far the reader has got.
 *
 *  - Books are added by dragging files to the window, or "On bookshelf" on
 *    a document's menu; directories dragged to it (or to the Watched
 *    folders box) are watched: Folio looks through them, and the
 *    directories inside, for new documents when it starts and on Refresh.
 *  - Covers (page 1) are drawn by the page worker the first time (it opens
 *    the book itself), then kept as sprite files in
 *    <Choices$Write>.Folio.Covers, so the shelf opens at once.
 *  - The shelf is kept in <Choices$Write>.Folio.Shelf (shelfdata.c).
 *  - Removing a book only takes it off the shelf; files are never touched. */
#include "app.h"
#include "worker.h"
#include "shelfdata.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <swis.h>

#define CELL_W      272
#define CELL_H      420
#define PAD         24
#define COVER_W     192       /* OS units */
#define COVER_MAXH  272
#define FOLDER_ROW  48
#define FOLDER_W    1000
#define FOLDER_HINT 104
#define SCAN_DEPTH  6
#define SCAN_MAX_DIRS 4096

enum { C_NONE, C_HAVE, C_LOADING, C_FAILED, C_NEEDJOB };

enum {
	SH_OPEN = 0, SH_REMOVE, SH_FILER, SH_RECENT, SH_TITLE, SH_AUTHOR, SH_FOLDERS, SH_REFRESH,
	SH_MISSING
};

static shelf S;
static struct {
	int wimp, folders_wimp;
	ObjectId menu;
	int open, folders_open;
	int sel;                  /* selected book, or -1 */
	int cols;
	char title[64], ftitle[64];
	int dirty;
	/* Scanning. */
	char (*stack)[256];
	int *depth;
	int sp, scanning, added;
	char cur[256];
	int cur_depth, offset;
} B;

static const int owner_tag = 0;   /* render_job.owner for covers */

/* ---- Files --------------------------------------------------------------- */

static void choices_dir(const char *sub, char *out, size_t size)
{
	const char *cw = getenv("Choices$Write");
	out[0] = 0;
	if (!cw || !*cw)
		return;
	snprintf(out, size, "%s.Folio", cw);
	_swix(OS_File, _INR(0, 1) | _IN(4), 8, out, 0);
	if (sub) {
		snprintf(out, size, "%s.Folio.%s", cw, sub);
		_swix(OS_File, _INR(0, 1) | _IN(4), 8, out, 0);
	}
}

static void save_shelf(void)
{
	char dir[200], path[256];
	choices_dir(NULL, dir, sizeof dir);
	if (!dir[0])
		return;
	snprintf(path, sizeof path, "%s.Shelf", dir);
	if (shelf_save(&S, path) == 0)
		_swix(OS_File, _INR(0, 2), 18, path, 0xfff);
	B.dirty = 0;
}

static unsigned hash(const char *s)
{
	unsigned h = 2166136261u;
	for (; *s; s++)
		h = (h ^ (unsigned char)(*s >= 'A' && *s <= 'Z' ? *s + 32 : *s)) * 16777619u;
	return h;
}

/* A sprite file is a sprite area without its first word. */
static int *load_cover(const shelf_book *b)
{
	char path[64];
	FILE *f;
	long n;
	int *area = NULL;
	snprintf(path, sizeof path, "Choices:Folio.Covers.C%08x", hash(b->path));
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n > 12 && n < (4 << 20) && (area = malloc((size_t)n + 4)) != NULL) {
		area[0] = (int)n + 4;
		if (fread(area + 1, 1, (size_t)n, f) != (size_t)n || area[2] != 16) {
			free(area);
			area = NULL;
		}
	}
	fclose(f);
	return area;
}

static void save_cover(const shelf_book *b)
{
	char dir[200], path[256];
	FILE *f;
	if (!b->cover_area)
		return;
	choices_dir("Covers", dir, sizeof dir);
	if (!dir[0])
		return;
	snprintf(path, sizeof path, "%s.C%08x", dir, hash(b->path));
	f = fopen(path, "wb");
	if (!f)
		return;
	fwrite(b->cover_area + 1, 1, (size_t)b->cover_area[3] - 4, f);
	fclose(f);
	_swix(OS_File, _INR(0, 2), 18, path, 0xff9);
}

static int exists(const char *path)
{
	int type = 0;
	if (_swix(OS_File, _INR(0, 1) | _OUT(0), 17, path, &type))
		return 0;
	return type == 1 || type == 3;
}

static void check_missing(void)
{
	int i;
	for (i = 0; i < S.n; i++)
		S.books[i].missing = !exists(S.books[i].path);
}

/* ---- Layout -------------------------------------------------------------- */

static int rows(void)
{
	return S.n ? (S.n + B.cols - 1) / B.cols : 1;
}

static void set_extent(int vis_h)
{
	int box[4], h = 2 * PAD + rows() * CELL_H;
	if (h < vis_h)
		h = vis_h;
	box[0] = 0;
	box[1] = -h;
	box[2] = app.screen_w > 2 * PAD + CELL_W ? app.screen_w : 2 * PAD + CELL_W;
	box[3] = 0;
	_swix(Wimp_SetExtent, _INR(0, 1), B.wimp, box);
}

static void redraw_all(void)
{
	if (B.open)
		_swix(Wimp_ForceRedraw, _INR(0, 4), B.wimp, 0, -0x1000000, 0x1000000, 0);
}

static void cell_box(int i, int *x0, int *y1)
{
	*x0 = PAD + (i % B.cols) * CELL_W;
	*y1 = -(PAD + (i / B.cols) * CELL_H);
}

static void redraw_cell(int i)
{
	int x0, y1;
	if (!B.open || i < 0 || i >= S.n)
		return;
	cell_box(i, &x0, &y1);
	_swix(Wimp_ForceRedraw, _INR(0, 4), B.wimp, x0, y1 - CELL_H, x0 + CELL_W, y1);
}

static void resort(void)
{
	char sel_path[256] = "";
	if (B.sel >= 0 && B.sel < S.n)
		snprintf(sel_path, sizeof sel_path, "%s", S.books[B.sel].path);
	shelf_sort(&S, S.sort);
	B.sel = sel_path[0] ? shelf_find(&S, sel_path) : -1;
	if (B.open) {
		int st[9];
		st[0] = B.wimp;
		_swix(Wimp_GetWindowState, _IN(1), st);
		set_extent(st[4] - st[2]);
	}
	redraw_all();
}

/* ---- Windows ------------------------------------------------------------- */

static int create_window(const char *title_text, char *title, int titlesize, int w, int h,
		unsigned extra_flags, int button_type, int back)
{
	int b[23], handle = 0;
	memset(b, 0, sizeof b);
	b[2] = w; b[3] = h;
	b[6] = -1;
	/* Moveable, back, close, title, toggle size, vertical scroll, adjust size, new format. */
	b[7] = (int)(0x80000002u | (1u << 24) | (1u << 25) | (1u << 26) | (1u << 27) | (1u << 28) |
		(1u << 29) | extra_flags);
	b[8] = 7 | (2 << 8) | (7 << 16) | (back << 24);
	b[9] = 3 | (1 << 8) | (12 << 16);
	b[10] = 0; b[11] = -h; b[12] = w; b[13] = 0;
	b[14] = 0x119;            /* title: text, centred, indirected */
	b[15] = button_type << 12;
	b[16] = 1;
	b[17] = 200 | (100 << 16);
	snprintf(title, (size_t)titlesize, "%s", title_text);
	b[18] = (int)title;
	b[19] = -1;
	b[20] = titlesize;
	if (_swix(Wimp_CreateWindow, _IN(1) | _OUT(0), b, &handle))
		return 0;
	return handle;
}

typedef struct {
	tbx_menu m;
	tbx_menu_entry e[10];
} menu_block;

static void entry(menu_block *b, int i, int comp, const char *text, int flags, int ev, const char *help)
{
	tbx_menu_entry *e = &b->e[i];
	memset(e, 0, sizeof *e);
	e->flags = flags;
	e->component_id = comp;
	e->text = text;
	e->max_text = (int)strlen(text) + 1;
	e->click_event = ev;
	e->help_message = help;
	e->max_entry_help = help ? (int)strlen(help) + 1 : 0;
}

static void create_objects(void)
{
	static menu_block m;
	B.wimp = create_window("Bookshelf", B.title, sizeof B.title, 4 * CELL_W + 2 * PAD,
		2 * CELL_H + PAD, 0, 10, 1);
	B.folders_wimp = create_window("Watched folders", B.ftitle, sizeof B.ftitle, FOLDER_W, 400, 0, 3, 0);
	entry(&m, 0, SH_OPEN, "Open           Return", 0, EV_SH_OPEN, "Click to open the selected book.");
	entry(&m, 1, SH_REMOVE, "Remove from bookshelf Delete", 0, EV_SH_REMOVE,
		"Click to take the selected book off the bookshelf. The file isn't touched.");
	entry(&m, 2, SH_FILER, "Show in Filer", MENU_DOTTED, EV_SH_FILER, "Click to open the directory the book is in.");
	entry(&m, 3, SH_RECENT, "Sort by recent", 0, EV_SH_RECENT, "Click to show the books read most recently first.");
	entry(&m, 4, SH_TITLE, "Sort by title", 0, EV_SH_TITLE, "Click to show the books in order of title.");
	entry(&m, 5, SH_AUTHOR, "Sort by author", MENU_DOTTED, EV_SH_AUTHOR, "Click to show the books in order of author.");
	entry(&m, 6, SH_FOLDERS, "Watched folders...", 0, EV_SH_FOLDERS,
		"Click to see the directories Folio looks in for new documents.");
	entry(&m, 7, SH_REFRESH, "Refresh", 0, EV_SH_REFRESH,
		"Click to look in the watched folders for new documents, and check the books are still there.");
	entry(&m, 8, SH_MISSING, "Remove missing books", 0, EV_SH_MISSING,
		"Click to take books whose files can't be found off the bookshelf.");
	m.m.flags = 0;
	m.m.title = "Bookshelf";
	m.m.max_title = 10;
	m.m.show_event = -1;
	m.m.hide_event = -1;
	m.m.num_entries = 9;
	if (tbx_create(Class_Menu, 102, 0, &m, (int)(sizeof m.m + 9 * sizeof m.e[0]), &B.menu))
		B.menu = 0;
}

static void open_window(int handle, int w, int h, int center)
{
	int b[8];
	b[0] = handle;
	b[1] = (app.screen_w - w) / 2;
	b[3] = b[1] + w;
	b[4] = (app.screen_h + h) / 2 + (center ? 0 : -64);
	b[2] = b[4] - h;
	b[5] = 0;
	b[6] = 0;
	b[7] = -1;
	_swix(Wimp_OpenWindow, _IN(1), b);
}

void bookshelf_open(void)
{
	int st[9];
	if (!B.wimp)
		return;
	st[0] = B.wimp;
	_swix(Wimp_GetWindowState, _IN(1), st);
	if (B.open && (st[8] & (1 << 16))) {
		st[7] = -1;           /* already open: to the front */
		_swix(Wimp_OpenWindow, _IN(1), st);
		return;
	}
	check_missing();
	B.cols = 4;
	set_extent(2 * CELL_H + PAD);
	B.open = 1;
	open_window(B.wimp, 4 * CELL_W + 2 * PAD + 40, 2 * CELL_H + PAD, 1);
}

static void open_folders(void)
{
	int box[4], h = FOLDER_HINT + (S.nfolders ? S.nfolders : 1) * FOLDER_ROW + PAD;
	box[0] = 0;
	box[1] = -(h > 400 ? h : 400);
	box[2] = FOLDER_W;
	box[3] = 0;
	_swix(Wimp_SetExtent, _INR(0, 1), B.folders_wimp, box);
	B.folders_open = 1;
	open_window(B.folders_wimp, FOLDER_W, h < 400 ? 400 : (h > 900 ? 900 : h), 0);
	_swix(Wimp_ForceRedraw, _INR(0, 4), B.folders_wimp, 0, -0x100000, FOLDER_W, 0);
}

int bookshelf_owns(int handle)
{
	return handle && (handle == B.wimp || handle == B.folders_wimp);
}

/* An Open_Window_Request: the columns follow the width. */
void bookshelf_open_request(int *b)
{
	_swix(Wimp_OpenWindow, _IN(1), b);
	if (b[0] == B.wimp) {
		int cols = (b[3] - b[1] - PAD) / CELL_W;
		if (cols < 1)
			cols = 1;
		if (cols != B.cols) {
			B.cols = cols;
			set_extent(b[4] - b[2]);
			redraw_all();
		}
	}
}

void bookshelf_close(int handle)
{
	int b[1];
	b[0] = handle;
	_swix(Wimp_CloseWindow, _IN(1), b);
	if (handle == B.wimp) {
		B.open = 0;
		if (B.dirty)
			save_shelf();
	} else {
		B.folders_open = 0;
	}
}

/* ---- Drawing ------------------------------------------------------------- */

static void fill(unsigned c, int x0, int y0, int x1, int y1)
{
	_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), c, 0, 0);
	_swix(OS_Plot, _INR(0, 2), 4, x0, y0);
	_swix(OS_Plot, _INR(0, 2), 96 + 5, x1, y1);
}

static int text_width(const char *s)
{
	int w = 0;
	_swix(Wimp_TextOp, _INR(0, 2) | _OUT(0), 1, s, 0, &w);
	return w;
}

/* s cut to fit width (with "..."), into out. */
static void fit(const char *s, int width, char *out, size_t size)
{
	size_t n;
	snprintf(out, size, "%s", s);
	if (text_width(out) <= width)
		return;
	for (n = strlen(out); n > 0; n--) {
		snprintf(out + n, size - n, "...");
		if (text_width(out) <= width)
			return;
		out[n] = 0;
	}
}

static void text(const char *s, int x, int y, unsigned fg, unsigned bg)
{
	_swix(Wimp_TextOp, _INR(0, 2), 0, fg, bg);
	_swix(Wimp_TextOp, _INR(0, 5), 2 | (1u << 28), s, -1, -1, x, y);
}

/* A cover for a book without one yet: a coloured binding with its title. */
static void plain_cover(const shelf_book *b, int x0, int y0, int x1, int y1)
{
	static const unsigned colours[] = {
		0x6a3a1e00u, 0x2e5a8a00u, 0x3a6e2e00u, 0x24288a00u, 0x5a2a6a00u, 0x6a6a2a00u
	};
	unsigned c = colours[hash(b->path) % (sizeof colours / sizeof colours[0])];
	char *t = app_desktop_text(b->title), cut[64];
	fill(c, x0, y0, x1, y1);
	fill(0xd8e8f000u, x0 + 16, y1 - 96, x1 - 16, y1 - 40);
	fit(t ? t : "", x1 - x0 - 40, cut, sizeof cut);
	text(cut, x0 + 24, y1 - 68, 0x00000000u, 0xd8e8f000u);
	free(t);
}

static void draw_book(int i, int ox, int oy)
{
	shelf_book *b = &S.books[i];
	int x0, y1, cx0, cy0, cy1, ch;
	unsigned bg = 0xdddddd00u;
	char line[160], *t;
	cell_box(i, &x0, &y1);
	x0 += ox;
	y1 += oy;
	if (i == B.sel) {
		bg = 0xc0601000u;    /* selected: a blue band (BBGGRR00 of #1060c0) */
		fill(bg, x0 + 4, y1 - CELL_H + 8, x0 + CELL_W - 4, y1 - 4);
	}
	/* The cover, standing on a line at the bottom of its space. */
	cx0 = x0 + (CELL_W - COVER_W) / 2;
	cy0 = y1 - 16 - COVER_MAXH;
	if (b->cover == C_HAVE && b->cover_area) {
		int *spr = b->cover_area + 4;
		ch = (spr[5] + 1) << app.xeig;
		cy1 = cy0 + ch;
		fill(0x55555500u, cx0 + 6, cy0 - 6, cx0 + COVER_W + 5, cy1 - 7);   /* shadow */
		_swix(OS_SpriteOp, _INR(0, 7), 52 + 512, b->cover_area, spr, cx0, cy0, 0, 0, 0);
	} else {
		ch = COVER_W * 4 / 3;
		cy1 = cy0 + ch;
		fill(0x55555500u, cx0 + 6, cy0 - 6, cx0 + COVER_W + 5, cy1 - 7);
		plain_cover(b, cx0, cy0, cx0 + COVER_W - 1, cy1 - 1);
	}
	if (b->missing) {
		/* Greyed: a stipple of the background over it. */
		int y;
		_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), 0xdddddd00u, 0, 0);
		for (y = cy0; y < cy1; y += 4)
			_swix(OS_Plot, _INR(0, 2), 4, cx0, y), _swix(OS_Plot, _INR(0, 2), 5, cx0 + COVER_W - 1, y);
	}
	/* How far through: a bar under the cover. */
	if (b->pages > 0 && b->read) {
		int w = (COVER_W * (b->page + 1)) / b->pages;
		if (w > COVER_W)
			w = COVER_W;
		fill(0xa0a0a000u, cx0, cy0 - 24, cx0 + COVER_W - 1, cy0 - 17);
		fill(0xbe5a1e00u, cx0, cy0 - 24, cx0 + w - 1, cy0 - 17);
	}
	/* Title and second line: author, or why it's grey. */
	t = app_desktop_text(b->title);
	fit(t ? t : "", CELL_W - 24, line, sizeof line);
	free(t);
	text(line, x0 + (CELL_W - text_width(line)) / 2, cy0 - 52,
		i == B.sel ? 0xffffff00u : 0x00000000u, bg);
	if (b->missing) {
		snprintf(line, sizeof line, "Can't be found");
	} else {
		t = app_desktop_text(b->author);
		fit(t ? t : "", CELL_W - 24, line, sizeof line);
		free(t);
	}
	text(line, x0 + (CELL_W - text_width(line)) / 2, cy0 - 92,
		i == B.sel ? 0xe0e0e000u : (b->missing ? 0x0000b000u : 0x55555500u), bg);
}

/* The cover for book i: from its file if there is one, else ask for it. */
static void want_cover(int i)
{
	shelf_book *b = &S.books[i];
	if (b->cover != C_NONE)
		return;
	b->cover_area = load_cover(b);
	b->cover = b->cover_area ? C_HAVE : C_NEEDJOB;
}

static void redraw_shelf(int *b)
{
	int more = 0;
	_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), b, &more);
	while (more) {
		int ox = b[1] - b[5], oy = b[4] - b[6];
		int top = oy - b[10], bottom = oy - b[8];
		int r0 = (top - PAD) / CELL_H, r1 = (bottom - PAD) / CELL_H, r, c;
		if (r0 < 0) r0 = 0;
		if (S.n == 0) {
			text("Drag PDFs and ebooks here to put them on the bookshelf.", ox + PAD, oy - PAD - 30,
				0x00000000u, 0xdddddd00u);
			text("Drag a directory here to have Folio look in it for new ones.", ox + PAD, oy - PAD - 74,
				0x00000000u, 0xdddddd00u);
		}
		for (r = r0; r <= r1; r++)
			for (c = 0; c < B.cols; c++) {
				int i = r * B.cols + c;
				if (i >= S.n)
					break;
				want_cover(i);
				draw_book(i, ox, oy);
			}
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}

static void redraw_folders(int *b)
{
	int more = 0;
	_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), b, &more);
	while (more) {
		int ox = b[1] - b[5], oy = b[4] - b[6], i;
		text("Drag directories here to watch them. Folio looks in them (and the", ox + PAD,
			oy - 30, 0x00000000u, 0xffffff00u);
		text("directories inside) for new documents when it starts, and on Refresh.", ox + PAD,
			oy - 70, 0x00000000u, 0xffffff00u);
		if (S.nfolders == 0)
			text("No directories are watched.", ox + PAD, oy - FOLDER_HINT - FOLDER_ROW / 2,
				0x55555500u, 0xffffff00u);
		for (i = 0; i < S.nfolders; i++) {
			int y = oy - FOLDER_HINT - i * FOLDER_ROW - FOLDER_ROW / 2;
			char line[300];
			fit(S.folders[i], FOLDER_W - 2 * PAD - 220, line, sizeof line);
			text(line, ox + PAD, y, 0x00000000u, 0xffffff00u);
			text("Stop watching", ox + FOLDER_W - PAD - 200, y, 0xbe5a1e00u, 0xffffff00u);
		}
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}

void bookshelf_redraw(int *b)
{
	if (b[0] == B.wimp)
		redraw_shelf(b);
	else
		redraw_folders(b);
}

/* ---- Actions ------------------------------------------------------------- */

static void select_book(int i)
{
	int old = B.sel;
	if (i == old)
		return;
	B.sel = i;
	redraw_cell(old);
	redraw_cell(i);
}

static void open_book(int i)
{
	if (i < 0 || i >= S.n)
		return;
	if (!exists(S.books[i].path)) {
		char msg[340];
		S.books[i].missing = 1;
		redraw_cell(i);
		snprintf(msg, sizeof msg, "Folio can't find '%s'.", S.books[i].path);
		tbx_report(msg);
		return;
	}
	docwin_open(S.books[i].path);
}

static void remove_book(int i)
{
	if (i < 0 || i >= S.n)
		return;
	shelf_remove(&S, i);
	B.sel = -1;
	B.dirty = 1;
	save_shelf();
	resort();
}

static int add_book(const char *path)
{
	int n = S.n, i = shelf_add(&S, path, NULL, (long)time(NULL));
	if (i >= 0 && S.n > n) {
		S.books[i].missing = 0;
		B.dirty = 1;
		return 1;
	}
	return 0;
}

/* ---- Scanning the watched folders ---------------------------------------- */

static void push_dir(const char *path, int depth)
{
	if (!B.stack) {
		B.stack = malloc(SCAN_MAX_DIRS * sizeof *B.stack);
		B.depth = malloc(SCAN_MAX_DIRS * sizeof *B.depth);
		if (!B.stack || !B.depth)
			return;
	}
	if (B.sp >= SCAN_MAX_DIRS)
		return;
	snprintf(B.stack[B.sp], sizeof B.stack[0], "%s", path);
	B.depth[B.sp++] = depth;
}

static void scan_start(const char *only)
{
	int i;
	if (only)
		push_dir(only, 0);
	else
		for (i = 0; i < S.nfolders; i++)
			push_dir(S.folders[i], 0);
	if (!B.scanning) {
		B.scanning = 1;
		B.added = 0;
		B.cur[0] = 0;
	}
}

/* Reads directories for up to 4 cs. */
static void scan_step(void)
{
	unsigned t0, t;
	static int buf[512];
	_swix(OS_ReadMonotonicTime, _OUT(0), &t0);
	for (;;) {
		int read = 0, next = 0;
		const char *p;
		int k;
		if (!B.cur[0]) {
			if (B.sp == 0) {
				B.scanning = 0;
				app_log("bookshelf: scan done, %d new", B.added);
				if (B.added) {
					save_shelf();
					resort();
				}
				return;
			}
			B.sp--;
			snprintf(B.cur, sizeof B.cur, "%s", B.stack[B.sp]);
			B.cur_depth = B.depth[B.sp];
			B.offset = 0;
		}
		/* OS_GBPB 10: names with load/exec, length, attributes and type. */
		if (_swix(OS_GBPB, _INR(0, 6) | _OUTR(3, 4), 10, B.cur, buf, 32, B.offset, sizeof buf, 0,
				&read, &next)) {
			B.cur[0] = 0;
			continue;
		}
		p = (const char *)buf;
		for (k = 0; k < read; k++) {
			const int *e = (const int *)p;
			const char *name = p + 20;
			unsigned load = (unsigned)e[0];
			int type = e[4];
			int filetype = (load & 0xfff00000u) == 0xfff00000u ? (int)((load >> 8) & 0xfff) : -1;
			char full[512];
			snprintf(full, sizeof full, "%s.%s", B.cur, name);
			if (type == 2 && B.cur_depth < SCAN_DEPTH && name[0] != '!')
				push_dir(full, B.cur_depth + 1);
			else if ((type == 1 || type == 3) && strlen(full) < 256 &&
					shelf_is_document(name, filetype, app.epub_type))
				B.added += add_book(full);
			p = name + ((strlen(name) + 4) & ~3u);
		}
		B.offset = next;
		if (next == -1)
			B.cur[0] = 0;
		_swix(OS_ReadMonotonicTime, _OUT(0), &t);
		if (t - t0 >= 4)
			break;
	}
}

/* ---- Covers -------------------------------------------------------------- */

static int submit_cover(void)
{
	int i, st[9], first = 0;
	if (!B.open || !worker_idle())
		return 0;
	/* Those showing first. */
	st[0] = B.wimp;
	_swix(Wimp_GetWindowState, _IN(1), st);
	first = ((-st[6]) / CELL_H) * B.cols;
	for (i = 0; i < S.n; i++) {
		int k = (first + i) % S.n;
		shelf_book *b = &S.books[k];
		if (b->cover == C_NEEDJOB && !b->missing) {
			render_job j;
			int px_w = COVER_W >> app.xeig, px_h = COVER_MAXH >> app.xeig;
			size_t img = (size_t)px_w * 4 * px_h, bytes = 16 + 44 + img;
			int dpi = 180 >> app.xeig, *area = malloc(bytes), *spr;
			if (!area)
				return 0;
			area[0] = (int)bytes;
			area[1] = 1;
			area[2] = 16;
			area[3] = (int)bytes;
			spr = area + 4;
			spr[0] = (int)(44 + img);
			memset(spr + 1, 0, 12);
			strcpy((char *)(spr + 1), "cover");
			spr[4] = px_w - 1;
			spr[5] = px_h - 1;
			spr[6] = 0;
			spr[7] = 31;
			spr[8] = 44;
			spr[9] = 44;
			spr[10] = (6 << 27) | (dpi << 14) | (dpi << 1) | 1;
			memset(&j, 0, sizeof j);
			j.kind = JOB_COVER;
			j.owner = (void *)&owner_tag;
			j.fc = app.fc;
			j.px_w = px_w;
			j.px_h = px_h;
			j.area = area;
			j.bytes = bytes;
			snprintf(j.path, sizeof j.path, "%s", b->path);
			if (worker_submit(&j)) {
				free(area);
				return 0;
			}
			b->cover = C_LOADING;
			return 1;
		}
	}
	return 0;
}

void bookshelf_take(render_job *r)
{
	int i = shelf_find(&S, r->path);
	if (i < 0) {
		free(r->area);
		return;
	}
	if (r->aborted) {
		free(r->area);
		S.books[i].cover = C_NEEDJOB;   /* stopped: try again later */
		return;
	}
	if (!r->ok) {
		free(r->area);
		S.books[i].cover = C_FAILED;
		app_log("bookshelf: no cover for %s: %s", r->path, r->err);
		return;
	}
	/* Trim the sprite area to what was drawn. */
	{
		int *spr = r->area + 4;
		size_t img = (size_t)r->px_w * 4 * r->px_h;
		spr[0] = (int)(44 + img);
		r->area[0] = r->area[3] = (int)(16 + 44 + img);
	}
	S.books[i].cover_area = r->area;
	S.books[i].cover = C_HAVE;
	save_cover(&S.books[i]);
	/* Names from the book itself, if it has them. */
	if (r->title[0])
		snprintf(S.books[i].title, sizeof S.books[i].title, "%s", r->title);
	if (r->author[0])
		snprintf(S.books[i].author, sizeof S.books[i].author, "%s", r->author);
	B.dirty = 1;
	redraw_cell(i);
}

int bookshelf_owns_job(const render_job *r)
{
	return r->kind == JOB_COVER && r->owner == (void *)&owner_tag;
}

int bookshelf_background(void)
{
	int busy = 0, i;
	const render_job *cur = worker_current();
	/* A cover stopped before it finished (the worker's job was taken back
	 * for something more urgent): ask for it again. */
	if (!cur || cur->owner != (void *)&owner_tag)
		for (i = 0; i < S.n; i++)
			if (S.books[i].cover == C_LOADING)
				S.books[i].cover = C_NEEDJOB;
	if (B.scanning) {
		scan_step();
		busy = 1;
	}
	if (submit_cover())
		busy = 1;
	for (i = 0; i < S.n && B.open; i++)
		if (S.books[i].cover == C_NEEDJOB || S.books[i].cover == C_LOADING) {
			busy = 1;
			break;
		}
	return busy;
}

/* ---- Events -------------------------------------------------------------- */

static int book_at(int sx, int sy)
{
	int st[9], wx, wy, c, r, i;
	st[0] = B.wimp;
	_swix(Wimp_GetWindowState, _IN(1), st);
	wx = sx - (st[1] - st[5]) - PAD;
	wy = -(sy - (st[4] - st[6])) - PAD;
	if (wx < 0 || wy < 0)
		return -1;
	c = wx / CELL_W;
	r = wy / CELL_H;
	if (c >= B.cols)
		return -1;
	i = r * B.cols + c;
	return i < S.n ? i : -1;
}

static void show_menu(int x, int y)
{
	int pos[2], has = B.sel >= 0;
	if (!B.menu)
		return;
	menu_set_fade(B.menu, SH_OPEN, !has);
	menu_set_fade(B.menu, SH_REMOVE, !has);
	menu_set_fade(B.menu, SH_FILER, !has);
	menu_set_tick(B.menu, SH_RECENT, S.sort == SHELF_SORT_RECENT);
	menu_set_tick(B.menu, SH_TITLE, S.sort == SHELF_SORT_TITLE);
	menu_set_tick(B.menu, SH_AUTHOR, S.sort == SHELF_SORT_AUTHOR);
	pos[0] = x - 64;
	pos[1] = y;
	tbx_show(B.menu, 0, 2, pos, 0, -1);
}

void bookshelf_click(int *b)
{
	if (b[3] == B.folders_wimp) {
		int st[9], wy, i;
		if (b[2] == 2)
			return;
		st[0] = B.folders_wimp;
		_swix(Wimp_GetWindowState, _IN(1), st);
		wy = -(b[1] - (st[4] - st[6])) - FOLDER_HINT;
		i = wy >= 0 ? wy / FOLDER_ROW : -1;
		/* "Stop watching" at the right of each row. */
		if (i >= 0 && i < S.nfolders && b[0] - (st[1] - st[5]) > FOLDER_W - PAD - 220) {
			app_log("bookshelf: stop watching %s", S.folders[i]);
			shelf_remove_folder(&S, i);
			save_shelf();
			open_folders();
		}
		return;
	}
	/* Keys (Delete, Return) to the shelf. */
	_swix(Wimp_SetCaretPosition, _INR(0, 5), B.wimp, -1, 0, 0, 1 << 25, -1);
	if (b[2] == 2) {                 /* Menu: on a book, that book */
		int i = book_at(b[0], b[1]);
		if (i >= 0)
			select_book(i);
		show_menu(b[0], b[1]);
	} else if (b[2] == 0x400 || b[2] == 0x100) {
		select_book(book_at(b[0], b[1]));
	} else if (b[2] == 4) {          /* double-click: open */
		open_book(book_at(b[0], b[1]));
	}
}

int bookshelf_key(int *b)
{
	if (b[0] != B.wimp)
		return 0;
	switch (b[6]) {
	case 13: open_book(B.sel); return 1;
	case 0x7f: remove_book(B.sel); return 1;
	}
	return 0;
}

static void refresh(void)
{
	check_missing();
	scan_start(NULL);
	redraw_all();
}

void bookshelf_event(int code)
{
	switch (code) {
	case EV_SH_OPEN: open_book(B.sel); break;
	case EV_SH_REMOVE: remove_book(B.sel); break;
	case EV_SH_FILER:
		if (B.sel >= 0) {
			char cmd[300];
			const char *p = S.books[B.sel].path, *dot = strrchr(p, '.');
			if (dot) {
				snprintf(cmd, sizeof cmd, "Filer_OpenDir %.*s", (int)(dot - p), p);
				_swix(OS_CLI, _IN(0), cmd);
			}
		}
		break;
	case EV_SH_RECENT: S.sort = SHELF_SORT_RECENT; resort(); save_shelf(); break;
	case EV_SH_TITLE: S.sort = SHELF_SORT_TITLE; resort(); save_shelf(); break;
	case EV_SH_AUTHOR: S.sort = SHELF_SORT_AUTHOR; resort(); save_shelf(); break;
	case EV_SH_FOLDERS: open_folders(); break;
	case EV_SH_REFRESH: refresh(); break;
	case EV_SH_MISSING: {
		int i;
		check_missing();
		for (i = S.n - 1; i >= 0; i--)
			if (S.books[i].missing)
				shelf_remove(&S, i);
		B.sel = -1;
		save_shelf();
		resort();
		break;
	}
	}
}

/* Files or directories dropped on the Bookshelf or the Watched folders box.
 * Returns 1 if it was for one of them (the caller acknowledges it). */
int bookshelf_data_load(int *b)
{
	const char *path = (const char *)(b + 11);
	int type = b[10];
	if (!bookshelf_owns(b[5]))
		return 0;
	if (type == 0x1000) {
		if (shelf_add_folder(&S, path) >= 0) {
			app_log("bookshelf: watching %s", path);
			save_shelf();
			scan_start(path);
			if (B.folders_open)
				open_folders();
		}
	} else {
		const char *dot = strrchr(path, '.');
		if (shelf_is_document(dot ? dot + 1 : path, type, app.epub_type) || type == 0xadf) {
			if (add_book(path)) {
				save_shelf();
				resort();
			}
		}
	}
	return 1;
}

/* ---- From document windows ------------------------------------------------ */

int bookshelf_has(const char *path)
{
	return shelf_find(&S, path) >= 0;
}

void bookshelf_toggle(const char *path)
{
	int i = shelf_find(&S, path);
	if (i >= 0) {
		remove_book(i);
	} else if (add_book(path)) {
		save_shelf();
		resort();
	}
}

/* A document was opened, or is closing: when, and how far through. */
void bookshelf_note(const char *path, int page, int pages, const char *title, const char *author)
{
	int i = shelf_find(&S, path);
	if (i < 0)
		return;
	S.books[i].read = (long)time(NULL);
	if (pages > 0) {
		S.books[i].page = page;
		S.books[i].pages = pages;
	}
	if (title && *title)
		snprintf(S.books[i].title, sizeof S.books[i].title, "%s", title);
	if (author && *author)
		snprintf(S.books[i].author, sizeof S.books[i].author, "%s", author);
	B.dirty = 1;
	if (S.sort == SHELF_SORT_RECENT)
		resort();
	else
		redraw_cell(i);
}

/* ---- Start and end -------------------------------------------------------- */

void bookshelf_init(void)
{
	shelf_init(&S);
	B.sel = -1;
	B.cols = 4;
	shelf_load(&S, "Choices:Folio.Shelf");
	shelf_sort(&S, S.sort);
	create_objects();
	app_log("bookshelf: %d books, %d watched folders", S.n, S.nfolders);
	if (S.nfolders)
		scan_start(NULL);     /* new documents in the watched folders */
}

void bookshelf_quit(void)
{
	if (B.dirty)
		save_shelf();
}
