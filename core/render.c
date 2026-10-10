/* Folio core: drawing pages into RISC OS 32bpp sprite pixels.
 *
 * A tile is drawn into a MuPDF RGBA pixmap laid over the caller's buffer
 * (MuPDF's byte order R,G,B,A is RISC OS's &AABBGGRR word), then the A byte
 * is cleared, because a 32bpp sprite without an alpha channel wants 0 there.
 *
 * Dark page: draw the content, invert its luminance (white <-> black with hues
 * kept), put pictures back from a normal drawing so photos aren't negatives,
 * pull near-black to pure black, then draw annotations on top unchanged.
 * Sepia: tint the content, put pictures back, annotations on top.
 */
#include "folio_int.h"
#include <string.h>

#define SEPIA_INK   0x3b2a1a
#define SEPIA_PAPER 0xf4ecd8
#define DARK_FLOOR  10   /* after inversion, channels at or below this become 0 */

static void draw_list(fz_context *ctx, fz_display_list *list, fz_pixmap *pix,
		fz_matrix ctm, fz_rect scissor)
{
	fz_device *dev = fz_new_draw_device(ctx, fz_identity, pix);
	fz_try(ctx) {
		fz_run_display_list(ctx, list, dev, ctm, scissor, NULL);
		fz_close_device(ctx, dev);
	}
	fz_always(ctx)
		fz_drop_device(ctx, dev);
	fz_catch(ctx)
		fz_rethrow(ctx);
}

/* Copy the pixels of rect r (device space) from src to dst (same bbox). */
static void copy_rect(fz_pixmap *dst, fz_pixmap *src, fz_irect r)
{
	int y;
	size_t w = (size_t)(r.x1 - r.x0) * 4;
	for (y = r.y0; y < r.y1; y++) {
		size_t d = (size_t)(y - dst->y) * dst->stride + (size_t)(r.x0 - dst->x) * 4;
		size_t s = (size_t)(y - src->y) * src->stride + (size_t)(r.x0 - src->x) * 4;
		memcpy(dst->samples + d, src->samples + s, w);
	}
}

static void dark_floor(fz_pixmap *pix)
{
	int x, y;
	for (y = 0; y < pix->h; y++) {
		unsigned char *p = pix->samples + (size_t)y * pix->stride;
		for (x = 0; x < pix->w; x++, p += 4)
			if (p[0] <= DARK_FLOOR && p[1] <= DARK_FLOOR && p[2] <= DARK_FLOOR)
				p[0] = p[1] = p[2] = 0;
	}
}

static void clear_alpha(fz_pixmap *pix)
{
	int x, y;
	for (y = 0; y < pix->h; y++) {
		unsigned char *p = pix->samples + (size_t)y * pix->stride + 3;
		for (x = 0; x < pix->w; x++, p += 4)
			*p = 0;
	}
}

/* Put the page's pictures back, unchanged, over an inverted tile. */
static void restore_pictures(folio_doc *doc, folio_cpage *cp, fz_pixmap *pix,
		fz_matrix ctm, fz_irect tile)
{
	fz_context *ctx = doc->ctx;
	fz_stext_page *st = folio_get_text(doc, cp);
	fz_stext_block *b;
	fz_pixmap *normal = NULL;

	fz_var(normal);
	fz_try(ctx) {
		for (b = st->first_block; b; b = b->next) {
			fz_irect r;
			if (b->type != FZ_STEXT_BLOCK_IMAGE)
				continue;
			r = fz_intersect_irect(fz_round_rect(fz_transform_rect(b->bbox, ctm)), tile);
			if (fz_is_empty_irect(r))
				continue;
			if (!normal) {
				normal = fz_new_pixmap_with_bbox(ctx, fz_device_rgb(ctx), tile, NULL, 1);
				fz_clear_pixmap_with_value(ctx, normal, 0xff);
				draw_list(ctx, cp->contents, normal, ctm, fz_rect_from_irect(tile));
			}
			copy_rect(pix, normal, r);
		}
	}
	fz_always(ctx)
		fz_drop_pixmap(ctx, normal);
	fz_catch(ctx)
		fz_rethrow(ctx);
}

int folio_render_tile(folio_doc *doc, folio_loc loc, float zoom,
		folio_irect tile, folio_page_mode mode, int annots,
		uint32_t *pixels, int stride)
{
	fz_context *ctx = doc->ctx;
	fz_pixmap *pix = NULL;
	int w = tile.x1 - tile.x0, h = tile.y1 - tile.y0;

	if (w <= 0 || h <= 0 || stride < w * 4 || (stride & 3)) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "bad tile");
		return -1;
	}
	fz_var(pix);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		fz_irect itile = fz_make_irect(tile.x0, tile.y0, tile.x1, tile.y1);
		fz_rect scissor = fz_rect_from_irect(itile);
		fz_matrix ctm = fz_concat(fz_translate(-cp->bounds.x0, -cp->bounds.y0), fz_scale(zoom, zoom));

		pix = fz_new_pixmap_with_data(ctx, fz_device_rgb(ctx), w, h, NULL, 1, stride,
				(unsigned char *)pixels);
		pix->x = tile.x0;
		pix->y = tile.y0;
		fz_clear_pixmap_with_value(ctx, pix, 0xff);
		draw_list(ctx, cp->contents, pix, ctm, scissor);
		if (mode == FOLIO_PAGE_DARK) {
			fz_invert_pixmap_luminance(ctx, pix);
			dark_floor(pix);
			restore_pictures(doc, cp, pix, ctm, itile);
		} else if (mode == FOLIO_PAGE_SEPIA) {
			fz_tint_pixmap(ctx, pix, SEPIA_INK, SEPIA_PAPER);
			restore_pictures(doc, cp, pix, ctm, itile);
		}
		if (annots)
			draw_list(ctx, cp->annots, pix, ctm, scissor);
		clear_alpha(pix);
	}
	fz_always(ctx)
		fz_drop_pixmap(ctx, pix);
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return 0;
}
