/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* A page as a RISC OS Draw file (PRM 4-457: "Draw file format").
 *
 * A MuPDF device that writes Draw objects as the page is run:
 *   - filled and stroked paths: path objects (type 2), with the fill and
 *     outline colours, width, joins, caps, dashes and winding rule;
 *   - text: each piece of text as one path object of its glyphs' outlines
 *     (so it looks the same without the document's fonts); invisible text
 *     (a scan's OCR layer) is left out; Type 3 glyphs are run as drawings;
 *   - pictures: sprite objects (type 5, or 13 when turned or flipped), at
 *     their own resolution, cut down to the part that shows, with a mask
 *     where they're transparent: so each bitmap can be taken out on its own;
 *   - shadings (gradients): drawn by MuPDF into a sprite, through whatever
 *     clips them, with a mask outside the shape;
 *   - tiled patterns: their cell repeated across the area.
 * Draw has no clipping and no transparency: objects outside the clip are
 * left out, pictures are cut to the clip's bounding box, soft masks and
 * group opacity are ignored.
 *
 * Units: Draw units are 1/46080 inch, so 640 to a point; y goes up from the
 * bottom of the page. Sprites are 32bpp, 90 dpi (512 Draw units a pixel). */
#include "folio_int.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

#define DU_PER_PT   640.0f
#define DU_PER_PX   512          /* a 90 dpi sprite pixel */
#define SHADE_DPI   150.0f
#define MAX_IMAGE_PX (16 << 20)  /* bigger pictures are scaled down to this */
#define MAX_CLIPS   64
#define MAX_TILES   4096

/* ---- The output ------------------------------------------------------------ */

typedef struct {
	unsigned char *d;
	size_t n, cap;
} dbuf;

static void need(fz_context *ctx, dbuf *b, size_t more)
{
	if (b->n + more > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 65536;
		while (cap < b->n + more)
			cap *= 2;
		b->d = fz_realloc(ctx, b->d, cap);
		b->cap = cap;
	}
}

static void put32(fz_context *ctx, dbuf *b, int v)
{
	unsigned u = (unsigned)v;
	need(ctx, b, 4);
	b->d[b->n++] = u & 255;
	b->d[b->n++] = (u >> 8) & 255;
	b->d[b->n++] = (u >> 16) & 255;
	b->d[b->n++] = (u >> 24) & 255;
}

static void set32(dbuf *b, size_t at, int v)
{
	unsigned u = (unsigned)v;
	b->d[at] = u & 255;
	b->d[at + 1] = (u >> 8) & 255;
	b->d[at + 2] = (u >> 16) & 255;
	b->d[at + 3] = (u >> 24) & 255;
}

typedef struct { int x0, y0, x1, y1, any; } dbox;

static void box_add(dbox *bx, int x, int y)
{
	if (!bx->any) {
		bx->x0 = bx->x1 = x;
		bx->y0 = bx->y1 = y;
		bx->any = 1;
		return;
	}
	if (x < bx->x0) bx->x0 = x;
	if (x > bx->x1) bx->x1 = x;
	if (y < bx->y0) bx->y0 = y;
	if (y > bx->y1) bx->y1 = y;
}

/* ---- The device ---------------------------------------------------------- */

typedef enum { CL_PATH, CL_STROKE, CL_TEXT, CL_STROKE_TEXT, CL_IMAGE, CL_OTHER } clip_kind;

typedef struct {
	clip_kind kind;
	fz_rect area;            /* device space, intersected with the one below */
	fz_path *path;
	fz_text *text;
	fz_image *image;
	fz_stroke_state *stroke;
	fz_matrix ctm;
	int even_odd;
} clip_entry;

typedef struct {
	fz_device super;
	dbuf b;                   /* the file */
	dbuf under;               /* multiplied (highlighter) objects: under the rest */
	dbuf *ob;                 /* where objects go now */
	int multiply;             /* inside groups with the multiply blend */
	dbox all;                 /* every object, Draw units */
	float page_h;
	fz_rect page;
	int masking;              /* inside a soft mask's definition: draw nothing */
	clip_entry clip[MAX_CLIPS];
	int nclip, clip_over;     /* clip_over: pushes past MAX_CLIPS, not kept */
	/* Inside a tiled pattern: each thing drawn is repeated at these offsets. */
	int tiling;
	fz_point *toff;
	int ntoff;
	/* The path object being made. */
	size_t obj;
	dbox obox;
	float half_w;             /* half the line width, for the box */
	int npts;
	fz_matrix pctm;           /* applied to points added */
	fz_point poff;            /* tile offset */
} draw_dev;

static int DX(draw_dev *d, float x) { return (int)lrintf(x * DU_PER_PT); }
static int DY(draw_dev *d, float y) { return (int)lrintf((d->page_h - y) * DU_PER_PT); }

static fz_rect cur_clip(draw_dev *d)
{
	return d->nclip ? d->clip[d->nclip - 1].area : d->page;
}

/* Worth drawing: not in a mask's definition, and inside the clip. */
static int visible(draw_dev *d, fz_rect r)
{
	if (d->masking)
		return 0;
	if (d->tiling)
		return 1;       /* repeated: the first cell may lie outside */
	return !fz_is_empty_rect(fz_intersect_rect(r, cur_clip(d)));
}

static unsigned colour_word(fz_context *ctx, fz_colorspace *cs, const float *color, fz_color_params cp)
{
	float rgb[3] = { 0, 0, 0 };
	int r, g, b;
	if (cs)
		fz_convert_color(ctx, cs, color, fz_device_rgb(ctx), rgb, NULL, cp);
	r = (int)lrintf(fz_clamp(rgb[0], 0, 1) * 255);
	g = (int)lrintf(fz_clamp(rgb[1], 0, 1) * 255);
	b = (int)lrintf(fz_clamp(rgb[2], 0, 1) * 255);
	return ((unsigned)b << 24) | ((unsigned)g << 16) | ((unsigned)r << 8);
}

/* Object header: type, size and box filled in by obj_end. */
static size_t obj_begin(fz_context *ctx, draw_dev *d, int type)
{
	size_t at = d->ob->n;
	put32(ctx, d->ob, type);
	put32(ctx, d->ob, 0);
	put32(ctx, d->ob, 0);
	put32(ctx, d->ob, 0);
	put32(ctx, d->ob, 0);
	put32(ctx, d->ob, 0);
	return at;
}

static void obj_end(draw_dev *d, size_t at, dbox *bx)
{
	set32(d->ob, at + 4, (int)(d->ob->n - at));
	set32(d->ob, at + 8, bx->x0);
	set32(d->ob, at + 12, bx->y0);
	set32(d->ob, at + 16, bx->x1);
	set32(d->ob, at + 20, bx->y1);
	box_add(&d->all, bx->x0, bx->y0);
	box_add(&d->all, bx->x1, bx->y1);
}

/* ---- Paths ----------------------------------------------------------------- */

static void pt(fz_context *ctx, draw_dev *d, int tag, int n, const fz_point *p)
{
	int i;
	put32(ctx, d->ob, tag);
	for (i = 0; i < n; i++) {
		fz_point q = fz_transform_point(p[i], d->pctm);
		int x = DX(d, q.x + d->poff.x), y = DY(d, q.y + d->poff.y);
		put32(ctx, d->ob, x);
		put32(ctx, d->ob, y);
		box_add(&d->obox, x, y);
	}
	d->npts++;
}

static void w_move(fz_context *ctx, void *arg, float x, float y)
{
	fz_point p = { x, y };
	pt(ctx, arg, 2, 1, &p);
}

static void w_line(fz_context *ctx, void *arg, float x, float y)
{
	fz_point p = { x, y };
	pt(ctx, arg, 8, 1, &p);
}

static void w_curve(fz_context *ctx, void *arg, float x1, float y1, float x2, float y2, float x3, float y3)
{
	fz_point p[3] = { { x1, y1 }, { x2, y2 }, { x3, y3 } };
	pt(ctx, arg, 6, 3, p);
}

static void w_close(fz_context *ctx, void *arg)
{
	draw_dev *d = arg;
	put32(ctx, d->ob, 5);
}

static const fz_path_walker walker = { w_move, w_line, w_curve, w_close, NULL, NULL, NULL, NULL };

/* Starts a path object: fill and outline as &BBGGRR00 or -1 (none). */
static void path_begin(fz_context *ctx, draw_dev *d, unsigned fill, unsigned outline, float width,
		const fz_stroke_state *st, int even_odd)
{
	int style = 0, i;
	d->obj = obj_begin(ctx, d, 2);
	memset(&d->obox, 0, sizeof d->obox);
	d->npts = 0;
	d->half_w = width / 2;
	put32(ctx, d->ob, (int)fill);
	put32(ctx, d->ob, (int)outline);
	put32(ctx, d->ob, (int)lrintf(width * DU_PER_PT));
	if (st) {
		int join = st->linejoin == FZ_LINEJOIN_ROUND ? 1 : st->linejoin == FZ_LINEJOIN_BEVEL ? 2 : 0;
		style = join | ((int)st->end_cap & 3) << 2 | ((int)st->start_cap & 3) << 4;
		/* Triangle caps: as wide as the line, half as long. */
		style |= 16 << 16 | 8 << 24;
		if (st->dash_len > 0)
			style |= 1 << 7;
	}
	if (even_odd)
		style |= 1 << 6;
	put32(ctx, d->ob, style);
	if (st && st->dash_len > 0) {
		float k = width > 0 && st->linewidth > 0 ? width / st->linewidth : 1;
		put32(ctx, d->ob, (int)lrintf(st->dash_phase * k * DU_PER_PT));
		put32(ctx, d->ob, st->dash_len);
		for (i = 0; i < st->dash_len; i++)
			put32(ctx, d->ob, (int)lrintf(st->dash_list[i] * k * DU_PER_PT));
	}
}

static void path_add(fz_context *ctx, draw_dev *d, const fz_path *path, fz_matrix ctm, fz_point off)
{
	d->pctm = ctm;
	d->poff = off;
	fz_walk_path(ctx, path, &walker, d);
}

/* Ends it; an empty one is taken back out. */
static void path_end(fz_context *ctx, draw_dev *d)
{
	int hw = (int)lrintf(d->half_w * DU_PER_PT);
	if (!d->npts) {
		d->ob->n = d->obj;
		return;
	}
	put32(ctx, d->ob, 0);
	d->obox.x0 -= hw;
	d->obox.y0 -= hw;
	d->obox.x1 += hw;
	d->obox.y1 += hw;
	obj_end(d, d->obj, &d->obox);
}

static fz_point no_off = { 0, 0 };

/* Each offset to draw at: one, or a pattern's cells. */
static int offsets(draw_dev *d, const fz_point **off)
{
	if (d->tiling && d->ntoff > 0) {
		*off = d->toff;
		return d->ntoff;
	}
	*off = &no_off;
	return 1;
}

static void dev_fill_path(fz_context *ctx, fz_device *dev, const fz_path *path, int even_odd, fz_matrix ctm,
		fz_colorspace *cs, const float *color, float alpha, fz_color_params cp)
{
	draw_dev *d = (draw_dev *)dev;
	const fz_point *off;
	int n, i;
	if (!visible(d, fz_bound_path(ctx, path, NULL, ctm)))
		return;
	n = offsets(d, &off);
	path_begin(ctx, d, colour_word(ctx, cs, color, cp), 0xffffffffu, 0, NULL, even_odd);
	for (i = 0; i < n; i++)
		path_add(ctx, d, path, ctm, off[i]);
	path_end(ctx, d);
}

static void dev_stroke_path(fz_context *ctx, fz_device *dev, const fz_path *path, const fz_stroke_state *st,
		fz_matrix ctm, fz_colorspace *cs, const float *color, float alpha, fz_color_params cp)
{
	draw_dev *d = (draw_dev *)dev;
	const fz_point *off;
	int n, i;
	float w = st->linewidth * fz_matrix_expansion(ctm);
	if (!visible(d, fz_bound_path(ctx, path, st, ctm)))
		return;
	n = offsets(d, &off);
	path_begin(ctx, d, 0xffffffffu, colour_word(ctx, cs, color, cp), w, st, 0);
	for (i = 0; i < n; i++)
		path_add(ctx, d, path, ctm, off[i]);
	path_end(ctx, d);
}

/* ---- Text ------------------------------------------------------------------ */

static void add_glyph(fz_context *ctx, draw_dev *d, fz_path *p, const fz_point *off, int n)
{
	fz_try(ctx) {
		int k;
		for (k = 0; k < n; k++)
			path_add(ctx, d, p, fz_identity, off[k]);
	}
	fz_always(ctx)
		fz_drop_path(ctx, p);
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static void text_paths(fz_context *ctx, draw_dev *d, const fz_text *text, fz_matrix ctm, unsigned fill,
		unsigned outline, float width, const fz_stroke_state *st)
{
	const fz_text_span *span;
	const fz_point *off;
	int n = offsets(d, &off), i, any = 0;
	if (!visible(d, fz_bound_text(ctx, text, st, ctm)))
		return;
	path_begin(ctx, d, fill, outline, width, st, 0);
	for (span = text->head; span; span = span->next) {
		for (i = 0; i < span->len; i++) {
			fz_matrix tm = span->trm, trm;
			fz_path *p;
			if (span->items[i].gid < 0)
				continue;
			tm.e = span->items[i].x;
			tm.f = span->items[i].y;
			trm = fz_concat(tm, ctm);
			if (fz_font_t3_procs(ctx, span->font))
				continue;       /* run as drawings below */
			p = fz_outline_glyph(ctx, span->font, span->items[i].gid, trm);
			if (!p)
				continue;
			add_glyph(ctx, d, p, off, n);
			any = 1;
		}
	}
	(void)any;
	path_end(ctx, d);
	/* Type 3 glyphs are little drawings of their own. */
	for (span = text->head; span; span = span->next) {
		if (!fz_font_t3_procs(ctx, span->font))
			continue;
		for (i = 0; i < span->len; i++) {
			fz_matrix tm = span->trm;
			if (span->items[i].gid < 0)
				continue;
			tm.e = span->items[i].x;
			tm.f = span->items[i].y;
			fz_run_t3_glyph(ctx, span->font, span->items[i].gid, fz_concat(tm, ctm), &d->super);
		}
	}
}

static void dev_fill_text(fz_context *ctx, fz_device *dev, const fz_text *text, fz_matrix ctm,
		fz_colorspace *cs, const float *color, float alpha, fz_color_params cp)
{
	draw_dev *d = (draw_dev *)dev;
	text_paths(ctx, d, text, ctm, colour_word(ctx, cs, color, cp), 0xffffffffu, 0, NULL);
}

static void dev_stroke_text(fz_context *ctx, fz_device *dev, const fz_text *text, const fz_stroke_state *st,
		fz_matrix ctm, fz_colorspace *cs, const float *color, float alpha, fz_color_params cp)
{
	draw_dev *d = (draw_dev *)dev;
	text_paths(ctx, d, text, ctm, 0xffffffffu, colour_word(ctx, cs, color, cp),
		st->linewidth * fz_matrix_expansion(ctm), st);
}

/* ---- Sprites --------------------------------------------------------------- */

/* A sprite object from an RGB(A) pixmap (n 3 or 4; alpha unpremultiplied
 * here), or a stencil (mask_only: n 1, the alpha) in one colour, placed by
 * ctm (the unit square to device space, as MuPDF draws images). */
static void sprite_obj(fz_context *ctx, draw_dev *d, fz_pixmap *pix, fz_matrix ctm, int mask_only, unsigned colour,
		fz_point off)
{
	int w = pix->w, h = pix->h, x, y, has_alpha = pix->alpha, mask_words = (w + 31) / 32, any_clear = 0;
	size_t at, spr, img_bytes = (size_t)w * h * 4, mask_bytes = (size_t)mask_words * 4 * h;
	int transformed;
	dbox bx;
	fz_matrix m = ctm;
	m.e += off.x;
	m.f += off.y;
	if (w <= 0 || h <= 0)
		return;
	/* Is there anything to mask? */
	if (has_alpha)
		for (y = 0; y < h && !any_clear; y++) {
			const unsigned char *s = pix->samples + (size_t)y * pix->stride;
			for (x = 0; x < w; x++)
				if (s[x * pix->n + pix->n - 1] < 128) {
					any_clear = 1;
					break;
				}
		}
	transformed = !(m.b == 0 && m.c == 0 && m.a > 0 && m.d > 0);
	at = obj_begin(ctx, d, transformed ? 13 : 5);
	if (transformed) {
		/* Sprite space (x right, y up, 512 units a pixel) to Draw units. */
		double a = DU_PER_PT * m.a / ((double)DU_PER_PX * w), b = -DU_PER_PT * m.b / ((double)DU_PER_PX * w);
		double c = -DU_PER_PT * m.c / ((double)DU_PER_PX * h), dd = DU_PER_PT * m.d / ((double)DU_PER_PX * h);
		double e = DU_PER_PT * (m.c + m.e), f = DU_PER_PT * (d->page_h - m.d - m.f);
		put32(ctx, d->ob, (int)lrint(a * 65536));
		put32(ctx, d->ob, (int)lrint(b * 65536));
		put32(ctx, d->ob, (int)lrint(c * 65536));
		put32(ctx, d->ob, (int)lrint(dd * 65536));
		put32(ctx, d->ob, (int)lrint(e));
		put32(ctx, d->ob, (int)lrint(f));
	}
	/* The sprite: header, image, mask. */
	spr = d->ob->n;
	need(ctx, d->ob, 44 + img_bytes + (any_clear ? mask_bytes : 0));
	put32(ctx, d->ob, 0);                   /* size: set below */
	{
		char name[12] = "image";
		memcpy(d->ob->d + d->ob->n, name, 12);
		d->ob->n += 12;
	}
	put32(ctx, d->ob, w - 1);
	put32(ctx, d->ob, h - 1);
	put32(ctx, d->ob, 0);
	put32(ctx, d->ob, 31);
	put32(ctx, d->ob, 44);
	put32(ctx, d->ob, any_clear ? (int)(44 + img_bytes) : 44);
	put32(ctx, d->ob, (6 << 27) | (90 << 14) | (90 << 1) | 1);
	for (y = 0; y < h; y++) {
		const unsigned char *s = pix->samples + (size_t)y * pix->stride;
		unsigned char *o = d->ob->d + d->ob->n;
		for (x = 0; x < w; x++, s += pix->n, o += 4) {
			int r, g, b;
			if (mask_only) {
				r = (colour >> 8) & 255;
				g = (colour >> 16) & 255;
				b = (colour >> 24) & 255;
			} else {
				int a = has_alpha ? s[pix->n - 1] : 255;
				r = s[0];
				g = s[1];
				b = s[2];
				if (has_alpha && a > 0 && a < 255) {
					r = r * 255 / a;
					g = g * 255 / a;
					b = b * 255 / a;
				}
				/* Where there's no mask, half-clear pixels go on white. */
				if (has_alpha && !any_clear && a < 255) {
					r = r * a / 255 + 255 - a;
					g = g * a / 255 + 255 - a;
					b = b * a / 255 + 255 - a;
				}
			}
			o[0] = (unsigned char)(r > 255 ? 255 : r);
			o[1] = (unsigned char)(g > 255 ? 255 : g);
			o[2] = (unsigned char)(b > 255 ? 255 : b);
			o[3] = 0;
		}
		d->ob->n += (size_t)w * 4;
	}
	if (any_clear) {
		for (y = 0; y < h; y++) {
			const unsigned char *s = pix->samples + (size_t)y * pix->stride;
			unsigned char *o = d->ob->d + d->ob->n;
			memset(o, 0, (size_t)mask_words * 4);
			for (x = 0; x < w; x++)
				if (s[x * pix->n + pix->n - 1] >= 128)
					o[x >> 3] |= (unsigned char)(1 << (x & 7));
			d->ob->n += (size_t)mask_words * 4;
		}
	}
	set32(d->ob, spr, (int)(d->ob->n - spr));
	/* Its box: the unit square's corners. */
	memset(&bx, 0, sizeof bx);
	{
		fz_point c[4] = { { 0, 0 }, { 1, 0 }, { 0, 1 }, { 1, 1 } };
		int i;
		for (i = 0; i < 4; i++) {
			fz_point p = fz_transform_point(c[i], m);
			box_add(&bx, DX(d, p.x), DY(d, p.y));
		}
	}
	obj_end(d, at, &bx);
}

/* The image's pixmap, cut to the part inside the clip (ctm adjusted), in
 * RGB(A); NULL if none of it shows. */
static fz_pixmap *image_part(fz_context *ctx, draw_dev *d, fz_image *img, fz_matrix *ctm, int mask)
{
	fz_pixmap *pix = NULL, *sub = NULL, *rgb = NULL;
	fz_rect clip = d->tiling ? d->page : cur_clip(d), u;
	fz_irect r;
	fz_matrix inv = fz_invert_matrix(*ctm);
	fz_var(pix);
	fz_var(sub);
	pix = mask ? fz_get_pixmap_from_image(ctx, img, NULL, NULL, NULL, NULL)
		: fz_get_pixmap_from_image(ctx, img, NULL, NULL, NULL, NULL);
	fz_try(ctx) {
		int w = pix->w, h = pix->h;
		/* The clip in image pixels. */
		u = fz_transform_rect(clip, inv);
		r.x0 = (int)floorf(fz_max(u.x0, 0) * w);
		r.y0 = (int)floorf(fz_max(u.y0, 0) * h);
		r.x1 = (int)ceilf(fz_min(u.x1, 1) * w);
		r.y1 = (int)ceilf(fz_min(u.y1, 1) * h);
		if (r.x0 < 0) r.x0 = 0;
		if (r.y0 < 0) r.y0 = 0;
		if (r.x1 > w) r.x1 = w;
		if (r.y1 > h) r.y1 = h;
		if (r.x1 <= r.x0 || r.y1 <= r.y0) {
			fz_drop_pixmap(ctx, pix);
			pix = NULL;
		} else if (r.x0 > 0 || r.y0 > 0 || r.x1 < w || r.y1 < h) {
			/* Just the part that shows: copy it out, and move the unit
			 * square onto it. */
			fz_matrix part = { (float)(r.x1 - r.x0) / w, 0, 0, (float)(r.y1 - r.y0) / h,
				(float)r.x0 / w, (float)r.y0 / h };
			int y;
			sub = fz_new_pixmap(ctx, pix->colorspace, r.x1 - r.x0, r.y1 - r.y0, NULL, pix->alpha);
			for (y = r.y0; y < r.y1; y++)
				memcpy(sub->samples + (size_t)(y - r.y0) * sub->stride,
					pix->samples + (size_t)y * pix->stride + (size_t)r.x0 * pix->n,
					(size_t)(r.x1 - r.x0) * pix->n);
			fz_drop_pixmap(ctx, pix);
			pix = sub;
			sub = NULL;
			*ctm = fz_concat(part, *ctm);
		}
		if (pix && (size_t)pix->w * pix->h > MAX_IMAGE_PX) {
			float k = sqrtf((float)MAX_IMAGE_PX / ((float)pix->w * pix->h));
			fz_pixmap *s = fz_scale_pixmap(ctx, pix, 0, 0, pix->w * k, pix->h * k, NULL);
			if (s) {
				fz_drop_pixmap(ctx, pix);
				pix = s;
			}
		}
		if (pix && !mask && (pix->colorspace != fz_device_rgb(ctx) || pix->n != 3 + pix->alpha)) {
			rgb = fz_convert_pixmap(ctx, pix, fz_device_rgb(ctx), NULL, NULL, fz_default_color_params, 1);
			fz_drop_pixmap(ctx, pix);
			pix = rgb;
		}
	}
	fz_catch(ctx) {
		fz_drop_pixmap(ctx, sub);
		fz_drop_pixmap(ctx, pix);
		fz_rethrow(ctx);
	}
	return pix;
}

static int replay_clips(fz_context *ctx, draw_dev *d, fz_device *to, fz_matrix scale);

/* The clips as an alpha mask over the image (pix, placed by ctm): pixels
 * outside the clip's shape become clear, so a picture cut to a curve or a
 * triangle keeps its shape. Returns pix, or a new RGBA pixmap. */
static fz_pixmap *clip_mask(fz_context *ctx, draw_dev *d, fz_pixmap *pix, fz_matrix ctm)
{
	fz_pixmap *a = NULL, *out = NULL;
	fz_device *draw = NULL;
	fz_path *rect = NULL;
	int i, k, x, y, any = 0, clear = 0;
	for (i = 0; i < d->nclip; i++)
		if (d->clip[i].kind != CL_OTHER)
			any = 1;
	if (!any || d->tiling)
		return pix;
	fz_var(pix);
	fz_var(a);
	fz_var(out);
	fz_var(draw);
	fz_var(rect);
	fz_try(ctx) {
		/* Device space to the image's pixels. */
		fz_matrix m = fz_concat(fz_invert_matrix(ctm), fz_scale(pix->w, pix->h));
		float grey = 1;
		a = fz_new_pixmap(ctx, NULL, pix->w, pix->h, NULL, 1);
		fz_clear_pixmap(ctx, a);
		draw = fz_new_draw_device(ctx, fz_identity, a);
		k = replay_clips(ctx, d, draw, m);
		rect = fz_new_path(ctx);
		fz_rectto(ctx, rect, 0, 0, pix->w, pix->h);
		fz_fill_path(ctx, draw, rect, 0, fz_identity, fz_device_gray(ctx), &grey, 1, fz_default_color_params);
		for (i = 0; i < k; i++)
			fz_pop_clip(ctx, draw);
		fz_close_device(ctx, draw);
		for (y = 0; y < pix->h && !clear; y++)
			for (x = 0; x < pix->w; x++)
				if (a->samples[(size_t)y * a->stride + x] < 255) {
					clear = 1;
					break;
				}
		if (clear) {
			/* Premultiplied RGBA: the image's alpha (if any) times the clip's. */
			out = fz_new_pixmap(ctx, fz_device_rgb(ctx), pix->w, pix->h, NULL, 1);
			for (y = 0; y < pix->h; y++) {
				const unsigned char *s = pix->samples + (size_t)y * pix->stride;
				const unsigned char *m8 = a->samples + (size_t)y * a->stride;
				unsigned char *o = out->samples + (size_t)y * out->stride;
				for (x = 0; x < pix->w; x++, s += pix->n, o += 4) {
					int al = m8[x];
					if (pix->alpha)
						al = al * s[pix->n - 1] / 255;
					o[0] = (unsigned char)(s[0] * m8[x] / 255);
					o[1] = (unsigned char)(s[1] * m8[x] / 255);
					o[2] = (unsigned char)(s[2] * m8[x] / 255);
					o[3] = (unsigned char)al;
				}
			}
		}
	}
	fz_always(ctx) {
		fz_drop_path(ctx, rect);
		fz_drop_device(ctx, draw);
		fz_drop_pixmap(ctx, a);
	}
	fz_catch(ctx) {
		fz_drop_pixmap(ctx, out);
		fz_drop_pixmap(ctx, pix);       /* (it takes pix, either way) */
		fz_rethrow(ctx);
	}
	if (!out)
		return pix;
	fz_drop_pixmap(ctx, pix);
	return out;
}

/* Sprite objects of pix at each offset; pix is dropped. */
static void emit_sprites(fz_context *ctx, draw_dev *d, fz_pixmap *pix, fz_matrix ctm, int mask_only,
		unsigned colour)
{
	fz_try(ctx) {
		const fz_point *off;
		int i, n = offsets(d, &off);
		for (i = 0; i < n; i++)
			sprite_obj(ctx, d, pix, ctm, mask_only, colour, off[i]);
	}
	fz_always(ctx)
		fz_drop_pixmap(ctx, pix);
	fz_catch(ctx)
		fz_rethrow(ctx);
}

static void dev_fill_image(fz_context *ctx, fz_device *dev, fz_image *img, fz_matrix ctm, float alpha,
		fz_color_params cp)
{
	draw_dev *d = (draw_dev *)dev;
	fz_pixmap *pix;
	if (!visible(d, fz_transform_rect(fz_unit_rect, ctm)))
		return;
	pix = image_part(ctx, d, img, &ctm, 0);
	if (!pix)
		return;
	pix = clip_mask(ctx, d, pix, ctm);
	emit_sprites(ctx, d, pix, ctm, 0, 0);
}

/* A stencil (an image mask) as an alpha-only pixmap: its one channel is the
 * coverage. Takes pix. */
static fz_pixmap *coverage(fz_context *ctx, fz_pixmap *pix)
{
	fz_pixmap *a = NULL;
	int x, y;
	if (pix->n == 1 && pix->alpha)
		return pix;
	fz_var(pix);
	fz_var(a);
	fz_try(ctx) {
		a = fz_new_pixmap(ctx, NULL, pix->w, pix->h, NULL, 1);
		for (y = 0; y < pix->h; y++)
			for (x = 0; x < pix->w; x++)
				a->samples[(size_t)y * a->stride + x] = pix->samples[(size_t)y * pix->stride + (size_t)x * pix->n];
	}
	fz_always(ctx)
		fz_drop_pixmap(ctx, pix);
	fz_catch(ctx)
		fz_rethrow(ctx);
	return a;
}

static void dev_fill_image_mask(fz_context *ctx, fz_device *dev, fz_image *img, fz_matrix ctm,
		fz_colorspace *cs, const float *color, float alpha, fz_color_params cp)
{
	draw_dev *d = (draw_dev *)dev;
	fz_pixmap *pix;
	if (!visible(d, fz_transform_rect(fz_unit_rect, ctm)))
		return;
	pix = image_part(ctx, d, img, &ctm, 1);
	if (!pix)
		return;
	pix = coverage(ctx, pix);
	emit_sprites(ctx, d, pix, ctm, 1, colour_word(ctx, cs, color, cp));
}

/* ---- Shadings: drawn by MuPDF, through the clips ---------------------------- */

/* Pushes the clips onto 'to'; how many (soft masks aren't kept). */
static int replay_clips(fz_context *ctx, draw_dev *d, fz_device *to, fz_matrix scale)
{
	int i, n = 0;
	for (i = 0; i < d->nclip; i++) {
		clip_entry *c = &d->clip[i];
		fz_matrix m = fz_concat(c->ctm, scale);
		fz_rect sc = fz_infinite_rect;
		switch (c->kind) {
		case CL_PATH: fz_clip_path(ctx, to, c->path, c->even_odd, m, sc); break;
		case CL_STROKE: fz_clip_stroke_path(ctx, to, c->path, c->stroke, m, sc); break;
		case CL_TEXT: fz_clip_text(ctx, to, c->text, m, sc); break;
		case CL_STROKE_TEXT: fz_clip_stroke_text(ctx, to, c->text, c->stroke, m, sc); break;
		case CL_IMAGE: fz_clip_image_mask(ctx, to, c->image, m, sc); break;
		default: continue;
		}
		n++;
	}
	return n;
}

static void dev_fill_shade(fz_context *ctx, fz_device *dev, fz_shade *shd, fz_matrix ctm, float alpha,
		fz_color_params cp)
{
	draw_dev *d = (draw_dev *)dev;
	fz_rect area = fz_intersect_rect(fz_bound_shade(ctx, shd, ctm), d->tiling ? d->page : cur_clip(d));
	fz_matrix scale = fz_scale(SHADE_DPI / 72, SHADE_DPI / 72);
	fz_irect ib;
	fz_pixmap *pix = NULL;
	fz_device *draw = NULL;
	int i;
	if (d->masking || fz_is_empty_rect(area))
		return;
	ib = fz_round_rect(fz_transform_rect(area, scale));
	if (ib.x1 <= ib.x0 || ib.y1 <= ib.y0)
		return;
	fz_var(pix);
	fz_var(draw);
	fz_try(ctx) {
		const fz_point *off;
		int n;
		fz_matrix place;
		pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), ib, NULL, 1);
		fz_clear_pixmap(ctx, pix);
		draw = fz_new_draw_device(ctx, fz_identity, pix);
		n = replay_clips(ctx, d, draw, scale);
		fz_fill_shade(ctx, draw, shd, fz_concat(ctm, scale), 1.0f, cp);
		for (i = 0; i < n; i++)
			fz_pop_clip(ctx, draw);
		fz_close_device(ctx, draw);
		/* The pixmap's rectangle, back in page space. */
		place = fz_make_matrix((ib.x1 - ib.x0) * 72 / SHADE_DPI, 0, 0, (ib.y1 - ib.y0) * 72 / SHADE_DPI,
			ib.x0 * 72 / SHADE_DPI, ib.y0 * 72 / SHADE_DPI);
		n = offsets(d, &off);
		for (i = 0; i < n; i++)
			sprite_obj(ctx, d, pix, place, 0, 0, off[i]);
	}
	fz_always(ctx) {
		fz_drop_device(ctx, draw);
		fz_drop_pixmap(ctx, pix);
	}
	fz_catch(ctx)
		fz_rethrow(ctx);
}

/* ---- Clips, masks, patterns ---------------------------------------------------- */

static clip_entry *push(fz_context *ctx, draw_dev *d, clip_kind kind, fz_rect area, fz_matrix ctm)
{
	clip_entry *c;
	if (d->nclip == MAX_CLIPS) {
		d->clip_over++;
		return NULL;
	}
	c = &d->clip[d->nclip];
	memset(c, 0, sizeof *c);
	c->kind = kind;
	c->area = fz_intersect_rect(area, cur_clip(d));
	c->ctm = ctm;
	d->nclip++;
	return c;
}

static void dev_clip_path(fz_context *ctx, fz_device *dev, const fz_path *path, int even_odd, fz_matrix ctm,
		fz_rect scissor)
{
	draw_dev *d = (draw_dev *)dev;
	clip_entry *c = push(ctx, d, CL_PATH, fz_bound_path(ctx, path, NULL, ctm), ctm);
	if (c) {
		c->path = fz_keep_path(ctx, path);
		c->even_odd = even_odd;
	}
}

static void dev_clip_stroke_path(fz_context *ctx, fz_device *dev, const fz_path *path, const fz_stroke_state *st,
		fz_matrix ctm, fz_rect scissor)
{
	draw_dev *d = (draw_dev *)dev;
	clip_entry *c = push(ctx, d, CL_STROKE, fz_bound_path(ctx, path, st, ctm), ctm);
	if (c) {
		c->path = fz_keep_path(ctx, path);
		c->stroke = fz_keep_stroke_state(ctx, st);
	}
}

static void dev_clip_text(fz_context *ctx, fz_device *dev, const fz_text *text, fz_matrix ctm, fz_rect scissor)
{
	draw_dev *d = (draw_dev *)dev;
	clip_entry *c = push(ctx, d, CL_TEXT, fz_bound_text(ctx, text, NULL, ctm), ctm);
	if (c)
		c->text = fz_keep_text(ctx, text);
}

static void dev_clip_stroke_text(fz_context *ctx, fz_device *dev, const fz_text *text, const fz_stroke_state *st,
		fz_matrix ctm, fz_rect scissor)
{
	draw_dev *d = (draw_dev *)dev;
	clip_entry *c = push(ctx, d, CL_STROKE_TEXT, fz_bound_text(ctx, text, st, ctm), ctm);
	if (c) {
		c->text = fz_keep_text(ctx, text);
		c->stroke = fz_keep_stroke_state(ctx, st);
	}
}

static void dev_clip_image_mask(fz_context *ctx, fz_device *dev, fz_image *img, fz_matrix ctm, fz_rect scissor)
{
	draw_dev *d = (draw_dev *)dev;
	clip_entry *c = push(ctx, d, CL_IMAGE, fz_transform_rect(fz_unit_rect, ctm), ctm);
	if (c)
		c->image = fz_keep_image(ctx, img);
}

static void drop_clip(fz_context *ctx, clip_entry *c)
{
	fz_drop_path(ctx, c->path);
	fz_drop_text(ctx, c->text);
	fz_drop_image(ctx, c->image);
	fz_drop_stroke_state(ctx, c->stroke);
	memset(c, 0, sizeof *c);
}

static void dev_pop_clip(fz_context *ctx, fz_device *dev)
{
	draw_dev *d = (draw_dev *)dev;
	if (d->clip_over) {
		d->clip_over--;
		return;
	}
	if (d->nclip > 0)
		drop_clip(ctx, &d->clip[--d->nclip]);
}

/* A soft mask: what's drawn between begin and end makes the mask, not the
 * page. MuPDF then treats the mask as a clip, popped as one. */
static void dev_begin_mask(fz_context *ctx, fz_device *dev, fz_rect area, int luminosity, fz_colorspace *cs,
		const float *bc, fz_color_params cp)
{
	draw_dev *d = (draw_dev *)dev;
	d->masking++;
}

static void dev_end_mask(fz_context *ctx, fz_device *dev, fz_function *fn)
{
	draw_dev *d = (draw_dev *)dev;
	if (d->masking > 0)
		d->masking--;
	push(ctx, d, CL_OTHER, d->page, fz_identity);
}

static int dev_begin_tile(fz_context *ctx, fz_device *dev, fz_rect area, fz_rect view, float xstep, float ystep,
		fz_matrix ctm, int id, int doc_id)
{
	draw_dev *d = (draw_dev *)dev;
	int i0, i1, j0, j1, i, j, n = 0;
	fz_rect a = fz_intersect_rect(area, fz_transform_rect(cur_clip(d), fz_invert_matrix(ctm)));
	if (d->tiling++)
		return 0;           /* a pattern in a pattern: its cell once */
	d->ntoff = 0;
	if (xstep == 0 || ystep == 0 || fz_is_empty_rect(a))
		return 0;
	xstep = fabsf(xstep);
	ystep = fabsf(ystep);
	i0 = (int)floorf((a.x0 - view.x1) / xstep);
	i1 = (int)ceilf((a.x1 - view.x0) / xstep);
	j0 = (int)floorf((a.y0 - view.y1) / ystep);
	j1 = (int)ceilf((a.y1 - view.y0) / ystep);
	if ((double)(i1 - i0 + 1) * (j1 - j0 + 1) > MAX_TILES) {
		/* Too many to repeat: one cell. */
		i1 = i0;
		j1 = j0;
	}
	d->toff = fz_malloc(ctx, sizeof(fz_point) * (size_t)(i1 - i0 + 1) * (j1 - j0 + 1));
	for (j = j0; j <= j1; j++)
		for (i = i0; i <= i1; i++) {
			fz_point v = fz_transform_vector(fz_make_point(i * xstep, j * ystep), ctm);
			d->toff[n++] = v;
		}
	d->ntoff = n;
	return 0;
}

static void dev_end_tile(fz_context *ctx, fz_device *dev)
{
	draw_dev *d = (draw_dev *)dev;
	if (d->tiling > 0 && --d->tiling == 0) {
		fz_free(ctx, d->toff);
		d->toff = NULL;
		d->ntoff = 0;
	}
}

/* Highlighter marks multiply with what's under them, which Draw can't do:
 * they go under everything else instead, so the text shows through. */
static void dev_begin_group(fz_context *ctx, fz_device *dev, fz_rect area, fz_colorspace *cs, int isolated,
		int knockout, int blendmode, float alpha)
{
	draw_dev *d = (draw_dev *)dev;
	if (blendmode == FZ_BLEND_MULTIPLY || d->multiply)
		d->multiply++;
	d->ob = d->multiply ? &d->under : &d->b;
}

static void dev_end_group(fz_context *ctx, fz_device *dev)
{
	draw_dev *d = (draw_dev *)dev;
	if (d->multiply > 0)
		d->multiply--;
	d->ob = d->multiply ? &d->under : &d->b;
}

static void dev_drop(fz_context *ctx, fz_device *dev)
{
	draw_dev *d = (draw_dev *)dev;
	while (d->nclip > 0)
		drop_clip(ctx, &d->clip[--d->nclip]);
	fz_free(ctx, d->toff);
	d->toff = NULL;
	fz_free(ctx, d->ob->d);
	d->ob->d = NULL;
}

/* ---- The file ----------------------------------------------------------------- */

int folio_export_draw(folio_doc *doc, folio_loc loc, const char *path)
{
	fz_context *ctx = doc->ctx;
	draw_dev *d = NULL;
	FILE *f = NULL;
	int rc = 0;
	folio_begin(doc);
	fz_var(d);
	fz_var(f);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		fz_matrix ctm = fz_translate(-cp->bounds.x0, -cp->bounds.y0);
		size_t hdr, hdr_end;
		d = fz_new_derived_device(ctx, draw_dev);
		d->super.fill_path = dev_fill_path;
		d->super.stroke_path = dev_stroke_path;
		d->super.clip_path = dev_clip_path;
		d->super.clip_stroke_path = dev_clip_stroke_path;
		d->super.fill_text = dev_fill_text;
		d->super.stroke_text = dev_stroke_text;
		d->super.clip_text = dev_clip_text;
		d->super.clip_stroke_text = dev_clip_stroke_text;
		d->super.fill_shade = dev_fill_shade;
		d->super.fill_image = dev_fill_image;
		d->super.fill_image_mask = dev_fill_image_mask;
		d->super.clip_image_mask = dev_clip_image_mask;
		d->super.pop_clip = dev_pop_clip;
		d->super.begin_mask = dev_begin_mask;
		d->super.end_mask = dev_end_mask;
		d->super.begin_tile = dev_begin_tile;
		d->super.end_tile = dev_end_tile;
		d->super.begin_group = dev_begin_group;
		d->super.end_group = dev_end_group;
		d->super.drop_device = dev_drop;
		d->ob = &d->b;
		d->page_h = cp->bounds.y1 - cp->bounds.y0;
		d->page = fz_make_rect(0, 0, cp->bounds.x1 - cp->bounds.x0, d->page_h);
		/* Header: "Draw", version 201.0, the program, the box (set at the end). */
		put32(ctx, &d->b, 0x77617244);
		put32(ctx, &d->b, 201);
		put32(ctx, &d->b, 0);
		need(ctx, &d->b, 12);
		memcpy(d->b.d + d->b.n, "Folio       ", 12);
		d->b.n += 12;
		hdr = d->b.n;
		put32(ctx, &d->b, 0);
		put32(ctx, &d->b, 0);
		put32(ctx, &d->b, 0);
		put32(ctx, &d->b, 0);
		hdr_end = d->b.n;
		fz_run_display_list(ctx, cp->contents, &d->super, ctm, fz_infinite_rect, &doc->cookie);
		if (cp->annots)
			fz_run_display_list(ctx, cp->annots, &d->super, ctm, fz_infinite_rect, &doc->cookie);
		fz_close_device(ctx, &d->super);
		folio_check_abort(doc);
		if (!d->all.any) {
			/* Nothing on the page: the page's own box. */
			box_add(&d->all, 0, 0);
			box_add(&d->all, DX(d, d->page.x1), DY(d, 0));
		}
		set32(&d->b, hdr, d->all.x0);
		set32(&d->b, hdr + 4, d->all.y0);
		set32(&d->b, hdr + 8, d->all.x1);
		set32(&d->b, hdr + 12, d->all.y1);
		f = fopen(path, "wb");
		if (!f)
			fz_throw(ctx, FZ_ERROR_SYSTEM, "can't write %s", path);
		/* Header, the multiplied objects, then the rest. */
		if (fwrite(d->b.d, 1, hdr_end, f) != hdr_end ||
				(d->under.n && fwrite(d->under.d, 1, d->under.n, f) != d->under.n) ||
				fwrite(d->b.d + hdr_end, 1, d->b.n - hdr_end, f) != d->b.n - hdr_end || fclose(f)) {
			f = NULL;
			fz_throw(ctx, FZ_ERROR_SYSTEM, "can't write %s", path);
		}
		f = NULL;
	}
	fz_always(ctx) {
		if (f)
			fclose(f);
		fz_drop_device(ctx, d ? &d->super : NULL);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		rc = -1;
	}
	return rc;
}
