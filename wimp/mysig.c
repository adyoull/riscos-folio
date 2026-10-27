/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* My signature: a handwritten signature kept for putting on documents.
 *  - Set up (Edit > Stamp > Set up my signature...): a pad to write it on
 *    with the mouse (Select drag; Adjust takes the last stroke off), or a
 *    picture of it (a scan of it on paper) dragged into the window, with
 *    "Make white see-through"; and how big it goes on the page.
 *  - Kept in <Choices$Write>.Folio.Signature ("folio-signature 1": size,
 *    then the strokes in points, or "picture" and the clear flag), and a
 *    picture copied to <Choices$Write>.Folio.SigPicture.
 *  - Placed (Edit > Stamp > My signature, then a click on the page): the
 *    strokes as a drawing (Ink, trimmed to their own size and scaled), or
 *    the picture as a stamp.
 *  - In a digital signature (the Sign box's "Show my handwritten
 *    signature"): its picture beside the name and date. */
#include "app.h"
#include "worker.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define PAD_X0   16
#define PAD_Y1   (-112)
#define PAD_W    1000           /* OS units: 400 x 120 points */
#define PAD_H    300
#define K        2.5f           /* OS units a point on the pad */
#define MAX_PTS  4000
#define MAX_STR  128
#define INK      0x1a2b8c

static struct {
	/* What's kept. */
	int have;                   /* 0 none, 1 strokes, 2 picture */
	int size;                   /* width on the page, points */
	int clear;                  /* picture: white see-through */
	folio_point pt[MAX_PTS];
	int count[MAX_STR], n, strokes;
	/* The window, while setting up (a copy to edit). */
	ObjectId obj;
	int wimp;
	int e_have, e_clear, e_size;
	folio_point e_pt[MAX_PTS];
	int e_count[MAX_STR], e_n, e_strokes;
	int dragging;
	char e_pic[256];            /* a picture dropped in, not yet kept */
	int loaded;
} ms = { .size = 144, .clear = 1 };

static const char *choices_dir(void)
{
	const char *cw = getenv("Choices$Write");
	return cw && *cw ? cw : NULL;
}

/* ---- Keeping it ------------------------------------------------------------- */

static void load(void)
{
	FILE *f;
	char line[64];
	if (ms.loaded)
		return;
	ms.loaded = 1;
	f = fopen("Choices:Folio.Signature", "r");
	if (!f)
		return;
	if (!fgets(line, sizeof line, f) || strncmp(line, "folio-signature 1", 17)) {
		fclose(f);
		return;
	}
	while (fscanf(f, "%63s", line) == 1) {
		if (!strcmp(line, "size"))
			(void)!fscanf(f, "%d", &ms.size);
		else if (!strcmp(line, "picture")) {
			(void)!fscanf(f, "%d", &ms.clear);
			ms.have = 2;
		} else if (!strcmp(line, "s")) {
			int c, i;
			if (fscanf(f, "%d", &c) != 1 || c <= 0 || ms.strokes >= MAX_STR || ms.n + c > MAX_PTS)
				break;
			for (i = 0; i < c; i++)
				if (fscanf(f, "%f %f", &ms.pt[ms.n + i].x, &ms.pt[ms.n + i].y) != 2)
					break;
			if (i < c)
				break;
			ms.count[ms.strokes++] = c;
			ms.n += c;
			ms.have = 1;
		}
	}
	fclose(f);
	if (ms.size < 36 || ms.size > 400)
		ms.size = 144;
	app_log("signature: %s kept", ms.have == 1 ? "a drawn signature" : ms.have == 2 ? "a picture" : "nothing");
}

static int save(void)
{
	const char *cw = choices_dir();
	char path[260];
	FILE *f;
	int i, s, at = 0;
	if (!cw)
		return -1;
	snprintf(path, sizeof path, "%s.Folio", cw);
	_swix(OS_File, _INR(0, 1) | _IN(4), 8, path, 0);
	snprintf(path, sizeof path, "%s.Folio.Signature", cw);
	f = fopen(path, "w");
	if (!f)
		return -1;
	fprintf(f, "folio-signature 1\nsize %d\n", ms.size);
	if (ms.have == 2)
		fprintf(f, "picture %d\n", ms.clear);
	else
		for (s = 0; s < ms.strokes; s++) {
			fprintf(f, "s %d", ms.count[s]);
			for (i = 0; i < ms.count[s]; i++, at++)
				fprintf(f, " %.1f %.1f", ms.pt[at].x, ms.pt[at].y);
			fputc('\n', f);
		}
	return fclose(f) ? -1 : 0;
}

/* A file's bytes copied (RISC OS names). */
static int copy_file(const char *from, const char *to)
{
	FILE *a = fopen(from, "rb"), *b;
	char buf[8192];
	size_t n;
	int bad = 0;
	if (!a)
		return -1;
	b = fopen(to, "wb");
	if (!b) {
		fclose(a);
		return -1;
	}
	while ((n = fread(buf, 1, sizeof buf, a)) > 0)
		if (fwrite(buf, 1, n, b) != n)
			bad = 1;
	fclose(a);
	if (fclose(b))
		bad = 1;
	return bad ? -1 : 0;
}

static void picture_path(char *out, size_t size)
{
	const char *cw = choices_dir();
	snprintf(out, size, "%s.Folio.SigPicture", cw ? cw : "<Wimp$ScrapDir>");
}

int mysig_have(void)
{
	load();
	return ms.have;
}

/* ---- Placing it -------------------------------------------------------------- */

/* The strokes' own box (their ink, not the pad). */
static void bounds(const folio_point *pt, int n, float *x0, float *y0, float *x1, float *y1)
{
	int i;
	*x0 = *y0 = 1e9f;
	*x1 = *y1 = -1e9f;
	for (i = 0; i < n; i++) {
		if (pt[i].x < *x0) *x0 = pt[i].x;
		if (pt[i].y < *y0) *y0 = pt[i].y;
		if (pt[i].x > *x1) *x1 = pt[i].x;
		if (pt[i].y > *y1) *y1 = pt[i].y;
	}
}

int mysig_place(folio_doc *doc, folio_loc loc, folio_point centre)
{
	load();
	if (ms.have == 2) {
		char pic[260];
		picture_path(pic, sizeof pic);
		return folio_annot_picture(doc, loc, centre, (float)ms.size, pic, ms.clear);
	}
	if (ms.have == 1) {
		folio_point *p = malloc(sizeof *p * (size_t)ms.n);
		float x0, y0, x1, y1, k;
		int i, rc;
		if (!p)
			return -1;
		bounds(ms.pt, ms.n, &x0, &y0, &x1, &y1);
		k = x1 - x0 > 1 ? ms.size / (x1 - x0) : 1;
		for (i = 0; i < ms.n; i++) {
			p[i].x = centre.x + (ms.pt[i].x - (x0 + x1) / 2) * k;
			p[i].y = centre.y + (ms.pt[i].y - (y0 + y1) / 2) * k;
		}
		rc = folio_annot_ink(doc, loc, p, ms.count, ms.strokes, INK, 1.4f * (k < 1 ? 1 : k > 2 ? 2 : k));
		free(p);
		return rc;
	}
	return -2;                  /* nothing set up */
}

/* For a digital signature's appearance: the signature as a picture. */
int mysig_for_signing(folio_signer *sg)
{
	load();
	if (ms.have == 2) {
		char pic[260];
		picture_path(pic, sizeof pic);
		return folio_signer_set_picture(app.fc, sg, pic, ms.clear);
	}
	if (ms.have == 1) {
		float x0, y0, x1, y1;
		folio_point *p = malloc(sizeof *p * (size_t)ms.n);
		const char *png = "<Wimp$ScrapDir>.FolioSig";
		int i, rc;
		if (!p)
			return -1;
		bounds(ms.pt, ms.n, &x0, &y0, &x1, &y1);
		for (i = 0; i < ms.n; i++) {
			p[i].x = ms.pt[i].x - x0 + 4;
			p[i].y = ms.pt[i].y - y0 + 4;
		}
		rc = folio_strokes_png(app.fc, p, ms.count, ms.strokes, x1 - x0 + 8, y1 - y0 + 8, INK, 2, png);
		free(p);
		if (rc == 0)
			rc = folio_signer_set_picture(app.fc, sg, png, 0);
		return rc;
	}
	return 0;
}

/* ---- The set-up window --------------------------------------------------------- */

typedef struct {
	int flags, type;
	int x0, y0, x1, y1;
	ComponentId id;
	const char *help;
	int max_help;
} gadget_hdr;
typedef struct { gadget_hdr h; const char *label; } label_gadget;
typedef struct { gadget_hdr h; int group; const char *label; int max_label; int event; } radio_gadget;
typedef struct { gadget_hdr h; const char *label; int max_label; int event; } option_gadget;
typedef struct {
	gadget_hdr h;
	const char *text;
	int max_text;
	const char *click_show;
	int event;
} button_gadget;

enum { S_HINT = 0, S_SIZE_L, S_SMALL, S_MEDIUM, S_LARGE, S_WHITE, S_CLEAR, S_CANCEL, S_SAVE, S_N };

static struct {
	label_gadget hint, size_l;
	radio_gadget small, medium, large;
	option_gadget white;
	button_gadget clear, cancel, save;
} sg;

#define WIN_W 1032
#define WIN_H 660

static void hdr(gadget_hdr *h, int type, int size, int id, int x0, int y0, int x1, int y1, const char *help)
{
	h->flags = 0;
	h->type = (size << 16) | type;
	h->x0 = x0; h->y0 = y0; h->x1 = x1; h->y1 = y1;
	h->id = id;
	h->help = help;
	h->max_help = help ? (int)strlen(help) + 1 : 0;
}

static void radio(radio_gadget *r, int id, int x0, const char *label, const char *help)
{
	hdr(&r->h, 384, sizeof *r, id, x0, -488, x0 + 220, -440, help);
	r->group = 1;
	r->label = label;
	r->max_label = (int)strlen(label) + 1;
}

static void button(button_gadget *b, int id, int flags, int x0, int x1, const char *text, int event,
		const char *help)
{
	hdr(&b->h, 128, sizeof *b, id, x0, -WIN_H + 16, x1, -WIN_H + 84, help);
	b->h.flags = flags;
	b->text = text;
	b->max_text = (int)strlen(text) + 1;
	b->event = event;
}

static ObjectId create_window(void)
{
	static tbx_window t;
	static char title[] = "My signature";
	ObjectId id = 0;
	_kernel_oserror *e;
	memset(&sg, 0, sizeof sg);
	hdr(&sg.hint.h, 320, sizeof sg.hint, S_HINT, 16, -88, WIN_W - 16, -24, NULL);
	sg.hint.h.flags = 1;
	sg.hint.label = "Write your signature below, or drag a picture of it here.";
	hdr(&sg.size_l.h, 320, sizeof sg.size_l, S_SIZE_L, 16, -488, 236, -440, NULL);
	sg.size_l.h.flags = 1;
	sg.size_l.label = "On the page";
	radio(&sg.small, S_SMALL, 248, "Small", "Click to put it on the page about 1.4 inches wide.");
	radio(&sg.medium, S_MEDIUM, 480, "Medium", "Click to put it on the page 2 inches wide.");
	radio(&sg.large, S_LARGE, 712, "Large", "Click to put it on the page about 2.8 inches wide.");
	hdr(&sg.white.h, 192, sizeof sg.white, S_WHITE, 248, -552, WIN_W - 16, -504,
		"Click to make a picture's white see-through, so the page shows through the paper.");
	sg.white.label = "A picture's white is see-through";
	sg.white.max_label = (int)strlen(sg.white.label) + 1;
	button(&sg.clear, S_CLEAR, 4, 16, 196, "Clear", EV_MS_CLEAR, "Click to start again.");
	button(&sg.cancel, S_CANCEL, 2, WIN_W - 376, WIN_W - 216, "Cancel", EV_MS_CANCEL,
		"Click to leave your signature as it was.");
	button(&sg.save, S_SAVE, 1, WIN_W - 196, WIN_W - 16, "Save", EV_MS_SAVE,
		"Click to keep this as your signature (in Choices).");
	memset(&t, 0, sizeof t);
	t.flags = (1 << 1) | (1 << 2);
	t.help_message = "Write your signature on the pad with Select (Adjust takes the last stroke off), "
		"or drag a picture of it into this window.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.num_gadgets = S_N;
	t.gadgets = &sg;
	t.default_focus = -1;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = WIN_W;
	t.window.vis_y1 = WIN_H;
	t.window.behind = -1;
	/* Folio draws the pad (no auto-redraw); clicks and drags on it. */
	t.window.window_flags = 0x80000002u | (1u << 24) | (1u << 25) | (1u << 26);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 1;
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -WIN_H;
	t.window.work_x1 = WIN_W;
	t.window.title_flags = 0x00000119;
	t.window.work_flags = 10 << 12;      /* click, drag, double */
	t.window.sprite_area = (void *)1;
	t.window.title_text = title;
	t.window.title_validation = "";
	t.window.title_buflen = sizeof title;
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	window_get_wimp_handle(id, &ms.wimp);
	return id;
}

static void redraw_pad(void)
{
	_swix(Wimp_ForceRedraw, _INR(0, 4), ms.wimp, PAD_X0 - 4, PAD_Y1 - PAD_H - 4, PAD_X0 + PAD_W + 4, PAD_Y1 + 4);
}

void mysig_setup(void)
{
	int ptr[5], pos[2];
	load();
	if (!ms.obj && !(ms.obj = create_window()))
		return;
	ms.e_have = ms.have;
	ms.e_clear = ms.clear;
	ms.e_size = ms.size;
	memcpy(ms.e_pt, ms.pt, sizeof ms.pt);
	memcpy(ms.e_count, ms.count, sizeof ms.count);
	ms.e_n = ms.n;
	ms.e_strokes = ms.strokes;
	ms.e_pic[0] = 0;
	tbx_method(ms.obj, 388, ms.size < 120 ? S_SMALL : ms.size > 170 ? S_LARGE : S_MEDIUM, 1, 0);
	tbx_method(ms.obj, 196, S_WHITE, ms.clear, 0);
	_swix(Wimp_GetPointerInfo, _IN(1), ptr);
	pos[0] = ptr[0] - WIN_W / 2;
	pos[1] = ptr[1] + WIN_H / 2;
	tbx_show(ms.obj, 0, 2, pos, 0, -1);
	redraw_pad();
}

int mysig_owns(int wimp_handle)
{
	return ms.obj && wimp_handle == ms.wimp;
}

static void text_at(const char *s, int x, int y)
{
	_swix(Wimp_TextOp, _INR(0, 2), 0, 0x77777700, 0xffffff00);
	_swix(Wimp_TextOp, _INR(0, 5), 2 | (1u << 28), s, -1, -1, x, y);
}

void mysig_redraw(int *b)
{
	int more = 0;
	b[0] = ms.wimp;
	_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), b, &more);
	while (more) {
		int ox = b[1] - b[5], oy = b[4] - b[6], s, j, at = 0;
		int x0 = ox + PAD_X0, y1 = oy + PAD_Y1;
		/* The pad: white, a grey edge, a line to write on. */
		_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), 0xffffff00u, 0, 0);
		_swix(OS_Plot, _INR(0, 2), 4, x0, y1 - PAD_H);
		_swix(OS_Plot, _INR(0, 2), 96 + 5, x0 + PAD_W, y1);
		_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), 0x99999900u, 0, 0);
		_swix(OS_Plot, _INR(0, 2), 4, x0, y1 - PAD_H);
		_swix(OS_Plot, _INR(0, 2), 5, x0 + PAD_W, y1 - PAD_H);
		_swix(OS_Plot, _INR(0, 2), 5, x0 + PAD_W, y1);
		_swix(OS_Plot, _INR(0, 2), 5, x0, y1);
		_swix(OS_Plot, _INR(0, 2), 5, x0, y1 - PAD_H);
		_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), 0xccccdd00u, 0, 0);
		_swix(OS_Plot, _INR(0, 2), 4, x0 + 40, y1 - PAD_H + 70);
		_swix(OS_Plot, _INR(0, 2), 5, x0 + PAD_W - 40, y1 - PAD_H + 70);
		if (ms.e_have == 2)
			text_at(ms.e_pic[0] ? "A picture of your signature (Save keeps it)." :
				"A picture of your signature is kept. Write here, or drag another, to change it.",
				x0 + 24, y1 - PAD_H / 2);
		_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), 0x8c2b1a00u, 0, 0);
		for (s = 0; ms.e_have == 1 && s < ms.e_strokes + ms.dragging && at < ms.e_n; s++) {
			int n = s < ms.e_strokes ? ms.e_count[s] : ms.e_n - at;
			for (j = 1; j < n; j++) {
				int d;
				for (d = 0; d < 4; d += 2) {
					_swix(OS_Plot, _INR(0, 2), 4, x0 + (int)(ms.e_pt[at + j - 1].x * K) + d,
						y1 - (int)(ms.e_pt[at + j - 1].y * K));
					_swix(OS_Plot, _INR(0, 2), 5, x0 + (int)(ms.e_pt[at + j].x * K) + d,
						y1 - (int)(ms.e_pt[at + j].y * K));
				}
			}
			at += n;
		}
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}

/* A screen point to the pad's points; 0 if off the pad. */
static int pad_point(int sx, int sy, folio_point *p)
{
	int st[9];
	float x, y;
	st[0] = ms.wimp;
	_swix(Wimp_GetWindowState, _IN(1), st);
	x = (float)(sx - (st[1] - st[5]) - PAD_X0) / K;
	y = (float)(PAD_Y1 - (sy - (st[4] - st[6]))) / K;
	if (x < 0) x = 0;
	if (y < 0) y = 0;
	if (x > PAD_W / K) x = PAD_W / K;
	if (y > PAD_H / K) y = PAD_H / K;
	p->x = x;
	p->y = y;
	return 1;
}

void mysig_click(int *b)
{
	folio_point p;
	int st[9], wx, wy;
	if (!ms.obj)
		return;
	st[0] = ms.wimp;
	_swix(Wimp_GetWindowState, _IN(1), st);
	wx = b[0] - (st[1] - st[5]);
	wy = b[1] - (st[4] - st[6]);
	if (wx < PAD_X0 || wx > PAD_X0 + PAD_W || wy > PAD_Y1 || wy < PAD_Y1 - PAD_H)
		return;
	if (b[2] == 0x40) {                     /* Select drag: a stroke */
		int d[10], ptr[5];
		if (ms.e_have != 1) {
			ms.e_have = 1;
			ms.e_n = ms.e_strokes = 0;
			ms.e_pic[0] = 0;
		}
		if (ms.e_strokes >= MAX_STR || ms.e_n >= MAX_PTS - 1)
			return;
		pad_point(b[0], b[1], &p);
		ms.e_pt[ms.e_n++] = p;
		ms.dragging = 1;
		_swix(Wimp_GetPointerInfo, _IN(1), ptr);
		d[0] = ms.wimp;
		d[1] = 7;
		d[2] = d[4] = ptr[0];
		d[3] = d[5] = ptr[1];
		d[6] = d[7] = -0x10000;
		d[8] = d[9] = 0x10000;
		_swix(Wimp_DragBox, _IN(1), d);
	} else if (b[2] == 0x100 && ms.e_have == 1 && ms.e_strokes > 0) {   /* Adjust: undo */
		ms.e_n -= ms.e_count[--ms.e_strokes];
		redraw_pad();
	}
}

/* Null polls while writing. */
int mysig_background(void)
{
	int ptr[5];
	folio_point p, last;
	if (!ms.dragging)
		return 0;
	_swix(Wimp_GetPointerInfo, _IN(1), ptr);
	pad_point(ptr[0], ptr[1], &p);
	last = ms.e_pt[ms.e_n - 1];
	if (ms.e_n < MAX_PTS && (p.x - last.x) * (p.x - last.x) + (p.y - last.y) * (p.y - last.y) >= 0.5f) {
		ms.e_pt[ms.e_n++] = p;
		redraw_pad();
	}
	return 1;
}

/* Wimp_EUserDrag: 1 if it was ours. */
int mysig_drag_done(void)
{
	if (!ms.dragging)
		return 0;
	mysig_background();
	ms.dragging = 0;
	{
		int used = 0, s;
		for (s = 0; s < ms.e_strokes; s++)
			used += ms.e_count[s];
		ms.e_count[ms.e_strokes++] = ms.e_n - used;
	}
	redraw_pad();
	return 1;
}

int mysig_data_load(int *b)
{
	if (!ms.obj || b[5] != ms.wimp)
		return 0;
	if (b[10] == 0x1000 || b[10] == 0x2000) {
		tbx_report("Drag a picture file of your signature here, not a directory.");
		return 1;
	}
	snprintf(ms.e_pic, sizeof ms.e_pic, "%s", (const char *)(b + 11));
	ms.e_have = 2;
	app_log("signature: picture %s", ms.e_pic);
	redraw_pad();
	return 1;
}

void mysig_event(int code)
{
	int on = 0;
	switch (code) {
	case EV_MS_CLEAR:
		ms.e_have = 0;
		ms.e_n = ms.e_strokes = 0;
		ms.e_pic[0] = 0;
		redraw_pad();
		break;
	case EV_MS_CANCEL:
		ms.dragging = 0;
		if (ms.obj)
			tbx_hide(ms.obj);
		break;
	case EV_MS_SAVE:
		if (ms.e_pic[0]) {
			char to[260];
			picture_path(to, sizeof to);
			if (!choices_dir() || copy_file(ms.e_pic, to)) {
				tbx_report("Folio couldn't keep the picture (in Choices).");
				return;
			}
		}
		ms.have = ms.e_have;
		memcpy(ms.pt, ms.e_pt, sizeof ms.pt);
		memcpy(ms.count, ms.e_count, sizeof ms.count);
		ms.n = ms.e_n;
		ms.strokes = ms.e_strokes;
		tbx_method_out(ms.obj, 389, S_SMALL, &on);
		ms.size = on ? 100 : 144;
		on = 0;
		tbx_method_out(ms.obj, 389, S_LARGE, &on);
		if (on)
			ms.size = 200;
		on = 0;
		tbx_method_out(ms.obj, 197, S_WHITE, &on);
		ms.clear = on != 0;
		if (ms.have == 1 && ms.strokes == 0)
			ms.have = 0;
		if (save())
			tbx_report("Folio couldn't save your signature (in Choices).");
		app_log("signature: saved (%s)", ms.have == 1 ? "drawn" : ms.have == 2 ? "picture" : "none");
		tbx_hide(ms.obj);
		break;
	}
}
