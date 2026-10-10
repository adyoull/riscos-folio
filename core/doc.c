/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio core: context, documents, layout, counting, locations, marks,
 * contents and metadata. */
#include "folio_int.h"
#include <stdio.h>
#include <string.h>

/* ---- Context ---------------------------------------------------------- */

void folio_set_error(folio_ctx *fc)
{
	const char *m = fz_caught_message(fc->ctx);
	snprintf(fc->err, sizeof fc->err, "%s", m && *m ? m : "unknown error");
}

folio_ctx *folio_new(size_t store_limit)
{
	folio_ctx *fc = calloc(1, sizeof *fc);
	if (!fc)
		return NULL;
	fc->ctx = fz_new_context(NULL, NULL, store_limit ? store_limit : FZ_STORE_DEFAULT);
	if (!fc->ctx) {
		free(fc);
		return NULL;
	}
	fz_var(fc);
	fz_try(fc->ctx)
		fz_register_document_handlers(fc->ctx);
	fz_catch(fc->ctx) {
		fz_drop_context(fc->ctx);
		free(fc);
		return NULL;
	}
	/* Warnings go nowhere: the Wimp front end has no stderr to show. */
	fz_set_warning_callback(fc->ctx, NULL, NULL);
	fz_set_error_callback(fc->ctx, NULL, NULL);
	return fc;
}

void folio_drop(folio_ctx *fc)
{
	if (!fc)
		return;
	fz_drop_context(fc->ctx);
	free(fc);
}

const char *folio_error(folio_ctx *fc)
{
	return fc->err;
}

/* ---- Page cache ------------------------------------------------------- */

void folio_drop_cpage(folio_doc *doc, folio_cpage *cp)
{
	fz_context *ctx = doc->ctx;
	if (!cp->used)
		return;
	fz_drop_link(ctx, cp->links);
	fz_drop_stext_page(ctx, cp->text);
	fz_drop_display_list(ctx, cp->annots);
	fz_drop_display_list(ctx, cp->contents);
	fz_drop_page(ctx, cp->page);
	memset(cp, 0, sizeof *cp);
}

void folio_forget_pages(folio_doc *doc)
{
	int i;
	for (i = 0; i < FOLIO_PAGE_CACHE; i++)
		folio_drop_cpage(doc, &doc->cache[i]);
}

folio_cpage *folio_get_page(folio_doc *doc, folio_loc loc)
{
	fz_context *ctx = doc->ctx;
	folio_cpage *cp = NULL, *oldest = &doc->cache[0];
	fz_device *dev = NULL;
	int i;

	for (i = 0; i < FOLIO_PAGE_CACHE; i++) {
		folio_cpage *c = &doc->cache[i];
		if (c->used && c->gen == doc->gen && c->loc.chapter == loc.chapter && c->loc.page == loc.page) {
			c->age = ++doc->clock;
			return c;
		}
		if (!c->used) {
			if (!cp)
				cp = c;
		} else if (c->age < oldest->age)
			oldest = c;
	}
	if (!cp) {
		folio_drop_cpage(doc, oldest);
		cp = oldest;
	}

	fz_var(dev);
	fz_var(cp);
	fz_try(ctx) {
		cp->used = 1;
		cp->loc = loc;
		cp->gen = doc->gen;
		cp->age = ++doc->clock;
		cp->page = fz_load_chapter_page(ctx, doc->doc, loc.chapter, loc.page);
		cp->bounds = fz_bound_page(ctx, cp->page);
		cp->contents = fz_new_display_list_from_page_contents(ctx, cp->page);
		cp->annots = fz_new_display_list(ctx, cp->bounds);
		dev = fz_new_list_device(ctx, cp->annots);
		fz_run_page_annots(ctx, cp->page, dev, fz_identity, NULL);
		fz_run_page_widgets(ctx, cp->page, dev, fz_identity, NULL);
		fz_close_device(ctx, dev);
	}
	fz_always(ctx)
		fz_drop_device(ctx, dev);
	fz_catch(ctx) {
		folio_drop_cpage(doc, cp);
		fz_rethrow(ctx);
	}
	return cp;
}

/* ---- Metadata --------------------------------------------------------- */

static char *meta(fz_context *ctx, fz_document *d, const char *key)
{
	char buf[512];
	int n = fz_lookup_metadata(ctx, d, key, buf, sizeof buf);
	return fz_strdup(ctx, n > 0 ? buf : "");
}

/* EPUB: dc:language from the package file (MuPDF doesn't report it). */
static char *epub_language(fz_context *ctx, const char *path)
{
	fz_archive *zip = NULL;
	fz_buffer *buf = NULL;
	fz_xml_doc *xml = NULL;
	char *lang = NULL;
	char opf[512];

	fz_var(zip); fz_var(buf); fz_var(xml); fz_var(lang);
	fz_try(ctx) {
		const char *full;
		fz_xml *n;
		zip = fz_open_archive(ctx, path);
		buf = fz_read_archive_entry(ctx, zip, "META-INF/container.xml");
		xml = fz_parse_xml(ctx, buf, 0);
		n = fz_xml_find_down(fz_xml_find_down(fz_xml_root(xml), "rootfiles"), "rootfile");
		full = fz_xml_att(n, "full-path");
		if (!full)
			fz_throw(ctx, FZ_ERROR_FORMAT, "no rootfile");
		fz_strlcpy(opf, full, sizeof opf);
		fz_drop_xml(ctx, xml); xml = NULL;
		fz_drop_buffer(ctx, buf); buf = NULL;
		buf = fz_read_archive_entry(ctx, zip, opf);
		xml = fz_parse_xml(ctx, buf, 0);
		n = fz_xml_find_down(fz_xml_find_down(fz_xml_root(xml), "metadata"), "language");
		if (n && fz_xml_text(fz_xml_down(n)))
			lang = fz_strdup(ctx, fz_xml_text(fz_xml_down(n)));
	}
	fz_always(ctx) {
		fz_drop_xml(ctx, xml);
		fz_drop_buffer(ctx, buf);
		fz_drop_archive(ctx, zip);
	}
	fz_catch(ctx) {
		fz_ignore_error(ctx);
		lang = NULL;
	}
	return lang ? lang : fz_strdup(ctx, "");
}

/* PDF: /Lang in the document catalogue. */
static char *pdf_language(fz_context *ctx, fz_document *d)
{
	char *lang = NULL;
	pdf_document *pdf = pdf_specifics(ctx, d);
	if (!pdf)
		return fz_strdup(ctx, "");
	fz_var(lang);
	fz_try(ctx) {
		pdf_obj *l = pdf_dict_get(ctx, pdf_dict_get(ctx, pdf_trailer(ctx, pdf), PDF_NAME(Root)), PDF_NAME(Lang));
		if (pdf_is_string(ctx, l))
			lang = fz_strdup(ctx, pdf_to_text_string(ctx, l));
	}
	fz_catch(ctx)
		fz_ignore_error(ctx);
	return lang ? lang : fz_strdup(ctx, "");
}

/* ---- Open and close --------------------------------------------------- */

static int file_exists(const char *path)
{
	FILE *f = path ? fopen(path, "rb") : NULL;
	if (f)
		fclose(f);
	return f != NULL;
}

static void reset_counts(folio_doc *doc)
{
	int i;
	for (i = 0; i < doc->nchap; i++)
		doc->chap_pages[i] = -1;
	doc->next_count = 0;
}

folio_doc *folio_open(folio_ctx *fc, const char *path, const char *accel_path)
{
	fz_context *ctx = fc->ctx;
	folio_doc *doc = calloc(1, sizeof *doc);
	if (!doc) {
		snprintf(fc->err, sizeof fc->err, "out of memory");
		return NULL;
	}
	doc->fc = fc;
	doc->ctx = ctx;
	fz_var(doc);
	fz_try(ctx) {
		if (file_exists(accel_path))
			doc->doc = fz_open_accelerated_document(ctx, path, accel_path);
		else
			doc->doc = fz_open_document(ctx, path);
		if (fz_needs_password(ctx, doc->doc))
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "the document is password protected");
		doc->reflow = fz_is_document_reflowable(ctx, doc->doc);
		doc->format = meta(ctx, doc->doc, FZ_META_FORMAT);
		doc->title = meta(ctx, doc->doc, FZ_META_INFO_TITLE);
		doc->author = meta(ctx, doc->doc, FZ_META_INFO_AUTHOR);
		if (pdf_specifics(ctx, doc->doc))
			doc->lang = pdf_language(ctx, doc->doc);
		else if (!strncmp(doc->format, "EPUB", 4) || !strncmp(doc->format, "epub", 4))
			doc->lang = epub_language(ctx, path);
		else
			doc->lang = fz_strdup(ctx, "");
		folio_default_layout(&doc->lay);
		if (doc->reflow && folio_set_layout(doc, &doc->lay))  /* sets chapters too */
			fz_throw(ctx, FZ_ERROR_FORMAT, "%s", fc->err);
		if (!doc->chap_pages) {
			doc->nchap = fz_count_chapters(ctx, doc->doc);
			doc->chap_pages = fz_malloc_array(ctx, doc->nchap > 0 ? doc->nchap : 1, int);
			reset_counts(doc);
		}
	}
	fz_catch(ctx) {
		folio_set_error(fc);
		folio_close(doc);
		return NULL;
	}
	return doc;
}

static void drop_outline(folio_doc *doc)
{
	fz_free(doc->ctx, doc->outline);
	fz_free(doc->ctx, doc->outline_strings);
	doc->outline = NULL;
	doc->outline_strings = NULL;
	doc->outline_n = 0;
}

void folio_close(folio_doc *doc)
{
	fz_context *ctx;
	if (!doc)
		return;
	ctx = doc->ctx;
	folio_forget_pages(doc);
	drop_outline(doc);
	fz_free(ctx, doc->chap_pages);
	fz_free(ctx, doc->css);
	fz_free(ctx, doc->format);
	fz_free(ctx, doc->title);
	fz_free(ctx, doc->author);
	fz_free(ctx, doc->lang);
	fz_drop_document(ctx, doc->doc);
	free(doc);
}

int folio_is_reflowable(folio_doc *doc) { return doc->reflow; }
const char *folio_format(folio_doc *doc) { return doc->format ? doc->format : ""; }
const char *folio_title(folio_doc *doc) { return doc->title ? doc->title : ""; }
const char *folio_author(folio_doc *doc) { return doc->author ? doc->author : ""; }
const char *folio_language(folio_doc *doc) { return doc->lang ? doc->lang : ""; }

/* ---- Layout ----------------------------------------------------------- */

void folio_default_layout(folio_layout *lay)
{
	lay->page_w = 400;
	lay->page_h = 600;
	lay->em = 11;
	lay->margin = 18;
	lay->line_spacing = 1.0f;
	lay->publisher_css = 1;
}

unsigned folio_layout_key(const folio_layout *lay)
{
	/* FNV-1a over the rounded values, so tiny float noise doesn't matter. */
	int v[6];
	unsigned h = 2166136261u;
	size_t i;
	v[0] = (int)(lay->page_w * 10 + 0.5f);
	v[1] = (int)(lay->page_h * 10 + 0.5f);
	v[2] = (int)(lay->em * 10 + 0.5f);
	v[3] = (int)(lay->margin * 10 + 0.5f);
	v[4] = (int)(lay->line_spacing * 100 + 0.5f);
	v[5] = lay->publisher_css;
	for (i = 0; i < sizeof v; i++) {
		h ^= ((unsigned char *)v)[i];
		h *= 16777619u;
	}
	return h;
}

int folio_set_layout(folio_doc *doc, const folio_layout *lay)
{
	fz_context *ctx = doc->ctx;
	char css[256];

	doc->lay = *lay;
	if (!doc->reflow)
		return 0;
	/* The page margin goes into the layout page; MuPDF's EPUB layout uses
	 * @page margins. Line spacing only when it differs from the book's. */
	if (lay->line_spacing > 0.99f && lay->line_spacing < 1.01f)
		snprintf(css, sizeof css, "@page { margin: %gpt }", lay->margin);
	else
		snprintf(css, sizeof css, "@page { margin: %gpt } p, li { line-height: %g !important }",
			lay->margin, lay->line_spacing * 1.2f);
	fz_try(ctx) {
		fz_free(ctx, doc->css);
		doc->css = NULL;
		doc->css = fz_strdup(ctx, css);
		fz_style_document(ctx, doc->doc, lay->publisher_css, doc->css);
		fz_layout_document(ctx, doc->doc, lay->page_w, lay->page_h, lay->em);
		folio_forget_pages(doc);
		drop_outline(doc);
		doc->gen++;
		{
			int n = fz_count_chapters(ctx, doc->doc);
			if (!doc->chap_pages || n != doc->nchap) {
				fz_free(ctx, doc->chap_pages);
				doc->chap_pages = NULL;
				doc->nchap = n;
				doc->chap_pages = fz_malloc_array(ctx, n > 0 ? n : 1, int);
			}
		}
		reset_counts(doc);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return 0;
}

/* ---- Counting --------------------------------------------------------- */

int folio_chapters(folio_doc *doc) { return doc->nchap; }

/* Count one chapter now (lays it out if the accelerator doesn't know it). */
static int count_chapter(folio_doc *doc, int ch)
{
	if (doc->chap_pages[ch] < 0)
		doc->chap_pages[ch] = fz_count_chapter_pages(doc->ctx, doc->doc, ch);
	return doc->chap_pages[ch];
}

int folio_count_step(folio_doc *doc, int max_chapters)
{
	fz_context *ctx = doc->ctx;
	fz_var(max_chapters);
	fz_try(ctx) {
		while (max_chapters-- > 0 && doc->next_count < doc->nchap) {
			count_chapter(doc, doc->next_count);
			doc->next_count++;
		}
		/* Chapters counted out of order (by browsing) are skipped quickly. */
		while (doc->next_count < doc->nchap && doc->chap_pages[doc->next_count] >= 0)
			doc->next_count++;
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return doc->next_count >= doc->nchap;
}

int folio_chapter_pages(folio_doc *doc, int chapter)
{
	if (chapter < 0 || chapter >= doc->nchap)
		return -1;
	return doc->chap_pages[chapter];
}

int folio_page_count(folio_doc *doc, int *exact)
{
	int i, known = 0, pages = 0;
	for (i = 0; i < doc->nchap; i++)
		if (doc->chap_pages[i] >= 0) {
			known++;
			pages += doc->chap_pages[i];
		}
	if (exact)
		*exact = known == doc->nchap;
	if (known == doc->nchap || known == 0)
		return known ? pages : doc->nchap;  /* nothing counted: at least a page a chapter */
	/* Estimate: the average of the counted chapters for the rest. */
	return pages + (int)((float)pages / known * (doc->nchap - known) + 0.5f);
}

int folio_page_number(folio_doc *doc, folio_loc loc)
{
	int i, n = 0;
	if (loc.chapter < 0 || loc.chapter >= doc->nchap)
		return -1;
	for (i = 0; i < loc.chapter; i++) {
		if (doc->chap_pages[i] < 0)
			return -1;
		n += doc->chap_pages[i];
	}
	return n + loc.page;
}

int folio_loc_from_number(folio_doc *doc, int number, folio_loc *loc)
{
	int i;
	if (number < 0)
		return -1;
	for (i = 0; i < doc->nchap; i++) {
		if (doc->chap_pages[i] < 0)
			return -1;
		if (number < doc->chap_pages[i]) {
			loc->chapter = i;
			loc->page = number;
			return 0;
		}
		number -= doc->chap_pages[i];
	}
	return -1;
}

int folio_next(folio_doc *doc, folio_loc *loc)
{
	fz_context *ctx = doc->ctx;
	int moved = 0;
	fz_var(moved);
	fz_try(ctx) {
		int ch = loc->chapter, pg = loc->page + 1;
		while (ch < doc->nchap) {
			if (pg < count_chapter(doc, ch)) {
				loc->chapter = ch;
				loc->page = pg;
				moved = 1;
				break;
			}
			ch++;
			pg = 0;
		}
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return moved;
}

int folio_prev(folio_doc *doc, folio_loc *loc)
{
	fz_context *ctx = doc->ctx;
	int moved = 0;
	fz_var(moved);
	fz_try(ctx) {
		int ch = loc->chapter, pg = loc->page - 1;
		while (ch >= 0) {
			if (pg >= 0) {
				loc->chapter = ch;
				loc->page = pg;
				moved = 1;
				break;
			}
			ch--;
			if (ch >= 0)
				pg = count_chapter(doc, ch) - 1;
		}
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return moved;
}

int folio_save_accel(folio_doc *doc, const char *accel_path)
{
	fz_context *ctx = doc->ctx;
	if (!doc->reflow)
		return 0;
	fz_try(ctx) {
		if (fz_document_supports_accelerator(ctx, doc->doc))
			fz_save_accelerator(ctx, doc->doc, accel_path);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return 0;
}

/* The first words of a page: up to 'max'-1 bytes of UTF-8, white space
 * collapsed, ending on a whole word when there is more text. */
static void page_start_text(fz_context *ctx, fz_stext_page *st, char *out, size_t max)
{
	fz_stext_block *b;
	fz_stext_line *l;
	fz_stext_char *c;
	size_t n = 0, last_space = 0;
	int space = 0, full = 0;
	for (b = st->first_block; b && !full; b = b->next) {
		if (b->type != FZ_STEXT_BLOCK_TEXT)
			continue;
		for (l = b->u.t.first_line; l && !full; l = l->next) {
			for (c = l->first_char; c && !full; c = c->next) {
				char u[8];
				int len;
				if (c->c == ' ' || c->c == '\t' || c->c == 0xa0) {
					space = 1;
					continue;
				}
				if (space && n > 0) {
					if (n + 1 >= max) { full = 1; break; }
					last_space = n;
					out[n++] = ' ';
				}
				space = 0;
				len = fz_runetochar(u, c->c);
				if (n + len >= max) { full = 1; break; }
				memcpy(out + n, u, len);
				n += len;
			}
			space = 1;  /* line end */
		}
	}
	if (full && last_space > 0)
		n = last_space;
	out[n] = 0;
}

int folio_make_mark(folio_doc *doc, folio_loc loc, folio_mark *mark)
{
	fz_context *ctx = doc->ctx;
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		int pages = count_chapter(doc, loc.chapter);
		memset(mark, 0, sizeof *mark);
		mark->chapter = loc.chapter;
		mark->fraction = pages > 0 ? (float)loc.page / pages : 0;
		page_start_text(ctx, folio_get_text(doc, cp), mark->text, 40);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return 0;
}

/* Does the page contain needle? */
static int page_has(folio_doc *doc, folio_loc loc, const char *needle)
{
	fz_quad q;
	int mark;
	folio_cpage *cp = folio_get_page(doc, loc);
	return fz_search_stext_page(doc->ctx, folio_get_text(doc, cp), needle, &mark, &q, 1) > 0;
}

int folio_find_mark(folio_doc *doc, const folio_mark *mark, folio_loc *loc)
{
	fz_context *ctx = doc->ctx;
	fz_try(ctx) {
		int pages, est, d, len, found = -1;
		char needle[64];
		if (mark->chapter < 0 || mark->chapter >= doc->nchap)
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "position not in this book");
		pages = count_chapter(doc, mark->chapter);
		est = (int)(mark->fraction * pages + 0.5f);
		if (est >= pages)
			est = pages - 1;
		if (est < 0)
			est = 0;
		/* Try the words as saved, then a shorter start in case the new
		 * layout splits them across a page break. */
		for (len = 0; found < 0 && len < 2; len++) {
			size_t n = strlen(mark->text);
			if (n == 0)
				break;
			fz_strlcpy(needle, mark->text, sizeof needle);
			if (len == 1) {
				/* Cut to about 14 bytes, on a space if there is one. */
				char *sp;
				if (n <= 14)
					break;
				needle[14] = 0;
				sp = strrchr(needle, ' ');
				if (sp && sp > needle + 4)
					*sp = 0;
			}
			for (d = 0; found < 0 && d < pages; d++) {
				if (est - d >= 0 && page_has(doc, (folio_loc){ mark->chapter, est - d }, needle))
					found = est - d;
				else if (d > 0 && est + d < pages && page_has(doc, (folio_loc){ mark->chapter, est + d }, needle))
					found = est + d;
			}
		}
		loc->chapter = mark->chapter;
		loc->page = found >= 0 ? found : est;
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return 0;
}

/* ---- Contents --------------------------------------------------------- */

static void count_outline(fz_outline *o, int *n, size_t *bytes)
{
	for (; o; o = o->next) {
		(*n)++;
		*bytes += strlen(o->title ? o->title : "") + 1 + (o->uri ? strlen(o->uri) + 1 : 0);
		count_outline(o->down, n, bytes);
	}
}

static void fill_outline(folio_doc *doc, fz_outline *o, int depth, int *i, char **s)
{
	fz_context *ctx = doc->ctx;
	fz_var(o);
	for (; o; o = o->next) {
		folio_outline_item *it = &doc->outline[(*i)++];
		fz_location l = o->page;
		size_t len = strlen(o->title ? o->title : "") + 1;
		memcpy(*s, o->title ? o->title : "", len);
		it->title = *s;
		*s += len;
		it->depth = depth;
		it->uri = NULL;
		if (l.chapter < 0 && o->uri && !fz_is_external_link(ctx, o->uri)) {
			fz_try(ctx)
				l = fz_resolve_link(ctx, doc->doc, o->uri, NULL, NULL);
			fz_catch(ctx) {
				fz_ignore_error(ctx);
				l = fz_make_location(-1, -1);
			}
		}
		if (o->uri && fz_is_external_link(ctx, o->uri)) {
			len = strlen(o->uri) + 1;
			memcpy(*s, o->uri, len);
			it->uri = *s;
			*s += len;
		}
		it->loc.chapter = l.chapter;
		it->loc.page = l.page;
		if (l.chapter >= 0 && l.chapter < doc->nchap && doc->chap_pages[l.chapter] < 0) {
			fz_try(ctx)
				count_chapter(doc, l.chapter);
			fz_catch(ctx)
				fz_ignore_error(ctx);
		}
		fill_outline(doc, o->down, depth + 1, i, s);
	}
}

int folio_outline(folio_doc *doc, const folio_outline_item **items, int *count)
{
	fz_context *ctx = doc->ctx;
	fz_outline *o = NULL;
	fz_var(o);
	if (!doc->outline) {
		fz_try(ctx) {
			int n = 0, i = 0;
			size_t bytes = 0;
			char *s;
			o = fz_load_outline(ctx, doc->doc);
			count_outline(o, &n, &bytes);
			doc->outline = fz_calloc(ctx, n > 0 ? n : 1, sizeof *doc->outline);
			doc->outline_strings = fz_malloc(ctx, bytes + 1);
			s = doc->outline_strings;
			fill_outline(doc, o, 0, &i, &s);
			doc->outline_n = n;
		}
		fz_always(ctx)
			fz_drop_outline(ctx, o);
		fz_catch(ctx) {
			drop_outline(doc);
			folio_set_error(doc->fc);
			return -1;
		}
	}
	*items = doc->outline;
	*count = doc->outline_n;
	return 0;
}

int folio_page_bounds(folio_doc *doc, folio_loc loc, folio_rect *bounds)
{
	fz_context *ctx = doc->ctx;
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		bounds->x0 = 0;
		bounds->y0 = 0;
		bounds->x1 = cp->bounds.x1 - cp->bounds.x0;
		bounds->y1 = cp->bounds.y1 - cp->bounds.y0;
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return 0;
}
