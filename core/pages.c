/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Pages out of a document, and documents out of pictures:
 *  - folio_parse_pages: "1-10, 15, 20-" into a page selection;
 *  - folio_save_pages: chosen pages as a new PDF (a PDF's own pages copied
 *    as they are; other documents' pages written through MuPDF's PDF
 *    writer, which keeps JPEG pictures as they are);
 *  - folio_save_page_image: a page's picture as a file: the JPEG stored in
 *    the PDF, byte for byte, when the page is one picture; else that picture
 *    as a PNG; else the whole page drawn as a PNG;
 *  - folio_images_to_pdf: picture files as the pages of a new PDF, a JPEG
 *    put in as it is (no re-compression), each page the picture's size at
 *    its own resolution. */
#include "folio_int.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ---- Page lists --------------------------------------------------------- */

static const char *skip_space(const char *s)
{
	while (*s == ' ' || *s == '\t')
		s++;
	return s;
}

int folio_parse_pages(const char *spec, int count, unsigned char *sel)
{
	const char *s = skip_space(spec ? spec : "");
	int n = 0, i;
	if (count <= 0)
		return -1;
	memset(sel, 0, (size_t)count);
	if (*s == 0 || ((s[0] == 'a' || s[0] == 'A') && (s[1] == 'l' || s[1] == 'L') &&
			(s[2] == 'l' || s[2] == 'L') && *skip_space(s + 3) == 0)) {
		memset(sel, 1, (size_t)count);
		return count;
	}
	while (*s) {
		long a, b;
		char *e;
		s = skip_space(s);
		if (*s == '-') {
			a = 1;                       /* "-5": from the start */
		} else {
			if (!isdigit((unsigned char)*s))
				return -1;
			a = strtol(s, &e, 10);
			s = e;
		}
		s = skip_space(s);
		b = a;
		if (*s == '-') {
			s = skip_space(s + 1);
			if (isdigit((unsigned char)*s)) {
				b = strtol(s, &e, 10);
				s = e;
			} else {
				b = count;               /* "20-": to the end */
			}
		}
		if (a < 1 || b < a)
			return -1;
		if (a > count)
			return -1;
		if (b > count)
			b = count;
		for (i = (int)a - 1; i < (int)b; i++)
			if (!sel[i]) {
				sel[i] = 1;
				n++;
			}
		s = skip_space(s);
		if (*s == ',' || *s == ';')
			s++;
		else if (*s)
			return -1;
	}
	return n > 0 ? n : -1;
}

/* ---- Save pages as a PDF ------------------------------------------------ */

int folio_save_pages(folio_doc *doc, const unsigned char *sel, int count, const char *path)
{
	fz_context *ctx = doc->ctx;
	pdf_document *src = pdf_specifics(ctx, doc->doc);
	pdf_document *dst = NULL;
	pdf_graft_map *map = NULL;
	fz_document_writer *wri = NULL;
	fz_page *page = NULL;
	int i, ok = 0;
	fz_var(dst);
	fz_var(map);
	fz_var(wri);
	fz_var(page);
	fz_var(i);
	fz_var(ok);
	fz_try(ctx) {
		if (src) {
			/* The pages themselves (contents, pictures, fonts,
			 * annotations, form fields), copied as they are. */
			pdf_write_options opts = pdf_default_write_options;
			dst = pdf_create_document(ctx);
			map = pdf_new_graft_map(ctx, dst);
			for (i = 0; i < count; i++)
				if (sel[i])
					pdf_graft_mapped_page(ctx, map, -1, src, i);
			opts.do_garbage = 1;
			opts.do_compress = 1;
			pdf_save_document(ctx, dst, path, &opts);
		} else {
			/* Other documents: each page drawn into a PDF page. */
			wri = fz_new_pdf_writer(ctx, path, "compress");
			for (i = 0; i < count; i++) {
				folio_loc loc;
				fz_rect r;
				fz_device *dev;
				if (!sel[i])
					continue;
				if (folio_loc_from_number(doc, i, &loc))
					fz_throw(ctx, FZ_ERROR_ARGUMENT, "page %d isn't counted yet", i + 1);
				page = fz_load_chapter_page(ctx, doc->doc, loc.chapter, loc.page);
				r = fz_bound_page(ctx, page);
				dev = fz_begin_page(ctx, wri, r);
				fz_run_page(ctx, page, dev, fz_translate(-r.x0, -r.y0), NULL);
				fz_end_page(ctx, wri);
				fz_drop_page(ctx, page);
				page = NULL;
			}
			fz_close_document_writer(ctx, wri);
		}
		ok = 1;
	}
	fz_always(ctx) {
		fz_drop_page(ctx, page);
		fz_drop_document_writer(ctx, wri);
		pdf_drop_graft_map(ctx, map);
		pdf_drop_document(ctx, dst);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
	}
	return ok ? 0 : -1;
}

/* ---- A page's picture ---------------------------------------------------- */

/* What a page draws: how many pictures, the first one and where, and
 * whether anything visible is drawn after it (on top of it). Anything drawn
 * before a picture that covers the page is hidden by it; text that isn't
 * drawn (a scan's recognised text) doesn't count. */
typedef struct {
	fz_device super;
	int images;
	int after;                  /* something visible drawn after the first picture */
	fz_image *image;
	fz_matrix ctm;
	float alpha;
} probe_dev;

static void probe_other(probe_dev *p)
{
	if (p->images)
		p->after = 1;
}

static void probe_fill_path(fz_context *ctx, fz_device *dev, const fz_path *path, int even_odd,
		fz_matrix ctm, fz_colorspace *cs, const float *color, float alpha, fz_color_params cp)
{
	probe_other((probe_dev *)dev);
}

static void probe_stroke_path(fz_context *ctx, fz_device *dev, const fz_path *path,
		const fz_stroke_state *st, fz_matrix ctm, fz_colorspace *cs, const float *color,
		float alpha, fz_color_params cp)
{
	probe_other((probe_dev *)dev);
}

static void probe_fill_text(fz_context *ctx, fz_device *dev, const fz_text *text, fz_matrix ctm,
		fz_colorspace *cs, const float *color, float alpha, fz_color_params cp)
{
	probe_other((probe_dev *)dev);
}

static void probe_stroke_text(fz_context *ctx, fz_device *dev, const fz_text *text,
		const fz_stroke_state *st, fz_matrix ctm, fz_colorspace *cs, const float *color,
		float alpha, fz_color_params cp)
{
	probe_other((probe_dev *)dev);
}

static void probe_fill_shade(fz_context *ctx, fz_device *dev, fz_shade *shd, fz_matrix ctm,
		float alpha, fz_color_params cp)
{
	probe_other((probe_dev *)dev);
}

static void probe_fill_image(fz_context *ctx, fz_device *dev, fz_image *img, fz_matrix ctm,
		float alpha, fz_color_params cp)
{
	probe_dev *p = (probe_dev *)dev;
	if (p->images++ == 0) {
		p->image = fz_keep_image(ctx, img);
		p->ctm = ctm;
		p->alpha = alpha;
	} else {
		p->after = 1;
	}
}

static void probe_fill_image_mask(fz_context *ctx, fz_device *dev, fz_image *img, fz_matrix ctm,
		fz_colorspace *cs, const float *color, float alpha, fz_color_params cp)
{
	probe_other((probe_dev *)dev);
}

static void probe_drop(fz_context *ctx, fz_device *dev)
{
	fz_drop_image(ctx, ((probe_dev *)dev)->image);
}

static int area_ok(fz_rect page, fz_rect pic)
{
	float pa = (page.x1 - page.x0) * (page.y1 - page.y0);
	fz_rect in = fz_intersect_rect(page, pic);
	float ia = fz_is_empty_rect(in) ? 0 : (in.x1 - in.x0) * (in.y1 - in.y0);
	return pa > 0 && ia >= 0.9f * pa;
}

static void write_buffer(fz_context *ctx, fz_buffer *buf, const char *path)
{
	fz_output *out = fz_new_output_with_path(ctx, path, 0);
	fz_try(ctx) {
		fz_write_buffer(ctx, out, buf);
		fz_close_output(ctx, out);
	}
	fz_always(ctx)
		fz_drop_output(ctx, out);
	fz_catch(ctx)
		fz_rethrow(ctx);
}

/* A pixmap as RGB or grey without alpha (PNG of what's seen). */
static fz_pixmap *plain_pixmap(fz_context *ctx, fz_pixmap *pix)
{
	fz_colorspace *cs = fz_pixmap_colorspace(ctx, pix);
	if (!pix->alpha && cs && (fz_colorspace_is_gray(ctx, cs) || fz_colorspace_is_rgb(ctx, cs)))
		return fz_keep_pixmap(ctx, pix);
	return fz_convert_pixmap(ctx, pix, cs && fz_colorspace_is_gray(ctx, cs) ? fz_device_gray(ctx) :
		fz_device_rgb(ctx), NULL, NULL, fz_default_color_params, 0);
}

int folio_save_page_image(folio_doc *doc, folio_loc loc, int dpi, const char *path)
{
	fz_context *ctx = doc->ctx;
	probe_dev *dev = NULL;
	fz_pixmap *pix = NULL, *plain = NULL;
	int kind = -1;
	fz_var(dev);
	fz_var(pix);
	fz_var(plain);
	fz_var(kind);
	fz_var(dpi);
	if (dpi < 18)
		dpi = 18;
	if (dpi > 1200)
		dpi = 1200;
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		fz_rect bounds = cp->bounds;
		dev = fz_new_derived_device(ctx, probe_dev);
		dev->super.fill_path = probe_fill_path;
		dev->super.stroke_path = probe_stroke_path;
		dev->super.fill_text = probe_fill_text;
		dev->super.stroke_text = probe_stroke_text;
		dev->super.fill_shade = probe_fill_shade;
		dev->super.fill_image = probe_fill_image;
		dev->super.fill_image_mask = probe_fill_image_mask;
		dev->super.drop_device = probe_drop;
		if (cp->contents)
			fz_run_display_list(ctx, cp->contents, &dev->super, fz_identity, fz_infinite_rect, NULL);
		if (cp->annots)
			fz_run_display_list(ctx, cp->annots, &dev->super, fz_identity, fz_infinite_rect, NULL);
		fz_close_device(ctx, &dev->super);

		if (dev->images == 1 && !dev->after && dev->alpha >= 1 &&
				area_ok(bounds, fz_transform_rect(fz_unit_rect, dev->ctm))) {
			fz_image *img = dev->image;
			fz_compressed_buffer *cb = fz_compressed_image_buffer(ctx, img);
			if (cb && cb->params.type == FZ_IMAGE_JPEG && !img->mask && !img->use_decode &&
					!img->use_colorkey && !img->imagemask) {
				/* The JPEG as stored. */
				write_buffer(ctx, cb->buffer, path);
				kind = FOLIO_PAGEIMG_JPEG;
			} else {
				/* The picture at its own size, as a PNG. */
				pix = fz_get_pixmap_from_image(ctx, img, NULL, NULL, NULL, NULL);
				plain = plain_pixmap(ctx, pix);
				fz_save_pixmap_as_png(ctx, plain, path);
				kind = FOLIO_PAGEIMG_IMAGE_PNG;
			}
		} else {
			/* The page, drawn at dpi. */
			float z = dpi / 72.0f;
			fz_matrix m = fz_pre_translate(fz_scale(z, z), -bounds.x0, -bounds.y0);
			fz_irect ib = fz_round_rect(fz_transform_rect(bounds, m));
			fz_device *draw = NULL;
			fz_var(draw);
			pix = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), ib, NULL, 0);
			fz_clear_pixmap_with_value(ctx, pix, 255);
			fz_try(ctx) {
				draw = fz_new_draw_device(ctx, fz_identity, pix);
				if (cp->contents)
					fz_run_display_list(ctx, cp->contents, draw, m, fz_infinite_rect, NULL);
				if (cp->annots)
					fz_run_display_list(ctx, cp->annots, draw, m, fz_infinite_rect, NULL);
				fz_close_device(ctx, draw);
			}
			fz_always(ctx)
				fz_drop_device(ctx, draw);
			fz_catch(ctx)
				fz_rethrow(ctx);
			fz_set_pixmap_resolution(ctx, pix, dpi, dpi);
			fz_save_pixmap_as_png(ctx, pix, path);
			kind = FOLIO_PAGEIMG_PAGE_PNG;
		}
	}
	fz_always(ctx) {
		fz_drop_pixmap(ctx, plain);
		fz_drop_pixmap(ctx, pix);
		fz_drop_device(ctx, dev ? &dev->super : NULL);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		kind = -1;
	}
	return kind;
}

/* ---- Pictures to a PDF -------------------------------------------------- */

int folio_images_to_pdf(folio_ctx *fc, const char *const *paths, int n, const char *path)
{
	return folio_images_to_pdf_res(fc, paths, n, 0, path);
}

int folio_images_to_pdf_res(folio_ctx *fc, const char *const *paths, int n, int dpi, const char *path)
{
	fz_context *ctx = fc->ctx;
	pdf_document *pdf = NULL;
	fz_image *img = NULL;
	fz_buffer *contents = NULL;
	pdf_obj *res = NULL, *page = NULL;
	int i, ok = 0;
	fz_var(pdf);
	fz_var(img);
	fz_var(contents);
	fz_var(res);
	fz_var(page);
	fz_var(i);
	fz_var(ok);
	fz_var(dpi);
	if (n <= 0) {
		snprintf(fc->err, sizeof fc->err, "there are no pictures");
		return -1;
	}
	fz_try(ctx) {
		pdf_write_options opts = pdf_default_write_options;
		pdf = pdf_create_document(ctx);
		for (i = 0; i < n; i++) {
			int xres, yres;
			float w, h;
			pdf_obj *ref, *xobj;
			fz_try(ctx)
				img = fz_new_image_from_file(ctx, paths[i]);
			fz_catch(ctx)
				fz_throw(ctx, FZ_ERROR_FORMAT, "%s: %s", paths[i], fz_caught_message(ctx));
			/* The resolution given (a scan's: what it was scanned at;
			 * scanners' JPEGs don't always say), else the picture's own. */
			if (dpi > 0)
				xres = yres = dpi;
			else
				fz_image_resolution(img, &xres, &yres);
			w = img->w * 72.0f / xres;
			h = img->h * 72.0f / yres;
			/* A JPEG goes in as it is (DCTDecode); other pictures are
			 * compressed losslessly. */
			ref = pdf_add_image(ctx, pdf, img);
			res = pdf_new_dict(ctx, pdf, 1);
			xobj = pdf_dict_put_dict(ctx, res, PDF_NAME(XObject), 1);
			pdf_dict_puts_drop(ctx, xobj, "I0", ref);
			contents = fz_new_buffer(ctx, 64);
			fz_append_printf(ctx, contents, "q %g 0 0 %g 0 0 cm /I0 Do Q\n", w, h);
			page = pdf_add_page(ctx, pdf, fz_make_rect(0, 0, w, h), 0, res, contents);
			pdf_insert_page(ctx, pdf, -1, page);
			pdf_drop_obj(ctx, page);
			page = NULL;
			pdf_drop_obj(ctx, res);
			res = NULL;
			fz_drop_buffer(ctx, contents);
			contents = NULL;
			fz_drop_image(ctx, img);
			img = NULL;
		}
		opts.do_compress = 1;
		pdf_save_document(ctx, pdf, path, &opts);
		ok = 1;
	}
	fz_always(ctx) {
		pdf_drop_obj(ctx, page);
		pdf_drop_obj(ctx, res);
		fz_drop_buffer(ctx, contents);
		fz_drop_image(ctx, img);
		pdf_drop_document(ctx, pdf);
	}
	fz_catch(ctx) {
		folio_set_error(fc);
	}
	return ok ? 0 : -1;
}

/* ---- Files joined into one PDF ------------------------------------------ */

/* Appends every page of the document at path to dst. */
static void combine_one(fz_context *ctx, pdf_document *dst, const char *path)
{
	fz_document *doc = NULL;
	pdf_graft_map *map = NULL;
	fz_page *page = NULL;
	fz_buffer *contents = NULL;
	pdf_obj *res = NULL, *obj = NULL;
	fz_device *dev = NULL;
	int i, n;
	fz_var(doc);
	fz_var(map);
	fz_var(page);
	fz_var(contents);
	fz_var(res);
	fz_var(obj);
	fz_var(dev);
	fz_try(ctx) {
		pdf_document *src;
		doc = fz_open_document(ctx, path);
		if (fz_needs_password(ctx, doc))
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "it needs a password: open it and save it without one first");
		src = pdf_specifics(ctx, doc);
		if (src) {
			/* A PDF's pages, as they are. A graft map is for one source. */
			map = pdf_new_graft_map(ctx, dst);
			n = pdf_count_pages(ctx, src);
			for (i = 0; i < n; i++)
				pdf_graft_mapped_page(ctx, map, -1, src, i);
		} else {
			/* Anything else (a picture, CBZ, XPS, an ebook laid out as
			 * Folio lays it out), drawn into PDF pages: pictures go in
			 * as they are. */
			if (fz_is_document_reflowable(ctx, doc))
				fz_layout_document(ctx, doc, 400, 600, 11);
			n = fz_count_pages(ctx, doc);
			for (i = 0; i < n; i++) {
				fz_rect r;
				page = fz_load_page(ctx, doc, i);
				r = fz_bound_page(ctx, page);
				dev = pdf_page_write(ctx, dst, fz_make_rect(0, 0, r.x1 - r.x0, r.y1 - r.y0), &res, &contents);
				fz_run_page(ctx, page, dev, fz_translate(-r.x0, -r.y0), NULL);
				fz_close_device(ctx, dev);
				fz_drop_device(ctx, dev);
				dev = NULL;
				obj = pdf_add_page(ctx, dst, fz_make_rect(0, 0, r.x1 - r.x0, r.y1 - r.y0), 0, res, contents);
				pdf_insert_page(ctx, dst, -1, obj);
				pdf_drop_obj(ctx, obj);
				obj = NULL;
				pdf_drop_obj(ctx, res);
				res = NULL;
				fz_drop_buffer(ctx, contents);
				contents = NULL;
				fz_drop_page(ctx, page);
				page = NULL;
			}
		}
		if (n <= 0)
			fz_throw(ctx, FZ_ERROR_FORMAT, "it has no pages");
	}
	fz_always(ctx) {
		fz_drop_device(ctx, dev);
		pdf_drop_obj(ctx, obj);
		pdf_drop_obj(ctx, res);
		fz_drop_buffer(ctx, contents);
		fz_drop_page(ctx, page);
		pdf_drop_graft_map(ctx, map);
		fz_drop_document(ctx, doc);
	}
	fz_catch(ctx) {
		fz_throw(ctx, FZ_ERROR_FORMAT, "%s: %s", path, fz_caught_message(ctx));
	}
}

int folio_combine(folio_ctx *fc, const char *const *paths, int n, const char *path)
{
	fz_context *ctx = fc->ctx;
	pdf_document *dst = NULL;
	int i, ok = 0;
	fz_var(dst);
	fz_var(i);
	fz_var(ok);
	if (n <= 0) {
		snprintf(fc->err, sizeof fc->err, "there are no files");
		return -1;
	}
	fz_try(ctx) {
		pdf_write_options opts = pdf_default_write_options;
		dst = pdf_create_document(ctx);
		for (i = 0; i < n; i++)
			combine_one(ctx, dst, paths[i]);
		opts.do_garbage = 1;
		opts.do_compress = 1;
		pdf_save_document(ctx, dst, path, &opts);
		ok = 1;
	}
	fz_always(ctx)
		pdf_drop_document(ctx, dst);
	fz_catch(ctx)
		folio_set_error(fc);
	return ok ? 0 : -1;
}
