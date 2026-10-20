/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio core: a document's properties for the Document info box, as label
 * and value pairs (UTF-8): the PDF's Info dictionary, its version, page
 * size, security and what its author allows, and what's inside it (forms,
 * attachments, JavaScript, tagging). */
#include "folio_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

static void add(folio_info *in, const char *label, const char *value)
{
	folio_info_item *it;
	if (in->n >= FOLIO_INFO_MAX || !value)
		return;
	it = &in->item[in->n++];
	snprintf(it->label, sizeof it->label, "%s", label);
	snprintf(it->value, sizeof it->value, "%s", value);
}

static void meta(fz_context *ctx, folio_doc *doc, folio_info *in, const char *label, const char *key)
{
	char buf[256];
	buf[0] = 0;
	fz_try(ctx)
		if (fz_lookup_metadata(ctx, doc->doc, key, buf, sizeof buf) <= 0)
			buf[0] = 0;
	fz_catch(ctx)
		buf[0] = 0;
	if (buf[0])
		add(in, label, buf);
}

/* "D:20230115093000+01'00'" to "15 Jan 2023, 09:30". */
void folio_pdf_date(const char *d, char *out, size_t size)
{
	static const char *mon[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep",
		"Oct", "Nov", "Dec" };
	int y = 0, m = 1, day = 1, h = -1, min = 0, n;
	if (d[0] == 'D' && d[1] == ':')
		d += 2;
	n = sscanf(d, "%4d%2d%2d%2d%2d", &y, &m, &day, &h, &min);
	if (n < 1 || y < 1000) {
		snprintf(out, size, "%s", d);
		return;
	}
	if (m < 1 || m > 12)
		m = 1;
	if (n >= 4 && h >= 0)
		snprintf(out, size, "%d %s %d, %02d:%02d", day, mon[m - 1], y, h, min);
	else if (n >= 3)
		snprintf(out, size, "%d %s %d", day, mon[m - 1], y);
	else
		snprintf(out, size, "%d", y);
}

static void date(fz_context *ctx, folio_doc *doc, folio_info *in, const char *label, const char *key)
{
	char buf[64], nice[64];
	buf[0] = 0;
	fz_try(ctx)
		if (fz_lookup_metadata(ctx, doc->doc, key, buf, sizeof buf) <= 0)
			buf[0] = 0;
	fz_catch(ctx)
		buf[0] = 0;
	if (buf[0]) {
		folio_pdf_date(buf, nice, sizeof nice);
		add(in, label, nice);
	}
}

/* A page size as a paper name if it is one, and in mm. */
static void paper(float w, float h, char *out, size_t size)
{
	static const struct { const char *name; float w, h; } p[] = {
		{ "A3", 842, 1191 }, { "A4", 595, 842 }, { "A5", 420, 595 }, { "A6", 298, 420 },
		{ "Letter", 612, 792 }, { "Legal", 612, 1008 }, { "B5", 499, 709 },
	};
	const char *name = NULL, *turn = "";
	int i;
	for (i = 0; i < (int)(sizeof p / sizeof p[0]); i++) {
		if (fabsf(w - p[i].w) < 3 && fabsf(h - p[i].h) < 3)
			name = p[i].name;
		else if (fabsf(w - p[i].h) < 3 && fabsf(h - p[i].w) < 3) {
			name = p[i].name;
			turn = " landscape";
		}
	}
	if (name)
		snprintf(out, size, "%s%s, %.0f x %.0f mm", name, turn, w * 25.4f / 72, h * 25.4f / 72);
	else
		snprintf(out, size, "%.0f x %.0f mm (%.0f x %.0f pt)", w * 25.4f / 72, h * 25.4f / 72, w, h);
}

static int name_tree_count(fz_context *ctx, pdf_document *pdf, pdf_obj *root, pdf_obj *which)
{
	pdf_obj *tree = pdf_dict_get(ctx, pdf_dict_get(ctx, root, PDF_NAME(Names)), which);
	pdf_obj *names, *kids;
	int n = 0, i;
	if (!tree)
		return 0;
	names = pdf_dict_get(ctx, tree, PDF_NAME(Names));
	if (pdf_is_array(ctx, names))
		n += pdf_array_len(ctx, names) / 2;
	kids = pdf_dict_get(ctx, tree, PDF_NAME(Kids));
	for (i = 0; i < pdf_array_len(ctx, kids) && i < 1000; i++) {
		pdf_obj *k = pdf_array_get(ctx, kids, i);
		pdf_obj *kn = pdf_dict_get(ctx, k, PDF_NAME(Names));
		if (pdf_is_array(ctx, kn))
			n += pdf_array_len(ctx, kn) / 2;
		else if (pdf_dict_get(ctx, k, PDF_NAME(Kids)))
			n++;              /* deeper trees: at least one */
	}
	(void)pdf;
	return n;
}

/* Bookmarks (outline entries), at most 10000 and a few levels deep. */
static int count_outline(fz_context *ctx, pdf_obj *first, int depth, int *budget)
{
	int n = 0;
	pdf_obj *o;
	for (o = first; o && *budget > 0 && depth < 32; o = pdf_dict_get(ctx, o, PDF_NAME(Next))) {
		(*budget)--;
		n++;
		n += count_outline(ctx, pdf_dict_get(ctx, o, PDF_NAME(First)), depth + 1, budget);
	}
	return n;
}

static const char *yes(int on) { return on ? "Allowed" : "Not allowed"; }

int folio_get_info(folio_doc *doc, folio_info *in)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_specifics(ctx, doc->doc);
	char buf[256];
	int pages = 0, exact = 0;
	memset(in, 0, sizeof *in);
	meta(ctx, doc, in, "Format", FZ_META_FORMAT);
	meta(ctx, doc, in, "Title", FZ_META_INFO_TITLE);
	if (!in->n || strcmp(in->item[in->n - 1].label, "Title"))
		if (doc->title && *doc->title)
			add(in, "Title", doc->title);
	meta(ctx, doc, in, "Author", FZ_META_INFO_AUTHOR);
	if (strcmp(in->item[in->n - 1].label, "Author") && doc->author && *doc->author)
		add(in, "Author", doc->author);
	meta(ctx, doc, in, "Subject", FZ_META_INFO_SUBJECT);
	meta(ctx, doc, in, "Keywords", FZ_META_INFO_KEYWORDS);
	meta(ctx, doc, in, "Made with", FZ_META_INFO_CREATOR);
	meta(ctx, doc, in, "PDF producer", FZ_META_INFO_PRODUCER);
	date(ctx, doc, in, "Created", FZ_META_INFO_CREATIONDATE);
	date(ctx, doc, in, "Modified", FZ_META_INFO_MODIFICATIONDATE);
	if (doc->lang && *doc->lang)
		add(in, "Language", doc->lang);
	pages = folio_page_count(doc, &exact);
	snprintf(buf, sizeof buf, "%d%s", pages, exact ? "" : " (still counting)");
	add(in, doc->reflow ? "Pages (as laid out)" : "Pages", buf);
	if (doc->reflow) {
		snprintf(buf, sizeof buf, "%d", doc->nchap);
		add(in, "Chapters", buf);
	} else {
		float w, h;
		if (folio_page_size(doc, (folio_loc){ 0, 0 }, &w, &h) == 0) {
			paper(w, h, buf, sizeof buf);
			add(in, "Page size", buf);
		}
	}
	if (pdf) {
		fz_var(pdf);
		fz_try(ctx) {
			pdf_obj *root = pdf_dict_get(ctx, pdf_trailer(ctx, pdf), PDF_NAME(Root));
			pdf_obj *acro = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
			pdf_obj *open = pdf_dict_gets(ctx, root, "OpenAction");
			int fields = pdf_array_len(ctx, pdf_dict_get(ctx, acro, PDF_NAME(Fields)));
			int files = name_tree_count(ctx, pdf, root, PDF_NAME(EmbeddedFiles));
			int js = name_tree_count(ctx, pdf, root, PDF_NAME(JavaScript)) > 0 ||
				pdf_name_eq(ctx, pdf_dict_get(ctx, open, PDF_NAME(S)), PDF_NAME(JavaScript));
			buf[0] = 0;
			if (fz_lookup_metadata(ctx, doc->doc, FZ_META_ENCRYPTION, buf, sizeof buf) <= 0 ||
					!strcmp(buf, "None"))
				snprintf(buf, sizeof buf, "None");
			add(in, "Security", buf);
			if (strcmp(buf, "None")) {
				add(in, "Printing", yes(folio_permitted(doc, FOLIO_PERM_PRINT)));
				add(in, "Copying", yes(folio_permitted(doc, FOLIO_PERM_COPY)));
				add(in, "Changing", yes(fz_has_permission(ctx, doc->doc, FZ_PERMISSION_EDIT)));
				add(in, "Notes and markings", yes(folio_permitted(doc, FOLIO_PERM_ANNOTATE)));
				add(in, "Filling in forms", yes(fz_has_permission(ctx, doc->doc, FZ_PERMISSION_FORM)));
				add(in, "Reading aloud", yes(folio_permitted(doc, FOLIO_PERM_ACCESSIBILITY)));
				add(in, "Page assembly", yes(fz_has_permission(ctx, doc->doc, FZ_PERMISSION_ASSEMBLE)));
			}
			add(in, "Tagged (structure)", pdf_dict_get(ctx, root, PDF_NAME(StructTreeRoot)) ? "Yes" : "No");
			add(in, "Fast web view", pdf_doc_was_linearized(ctx, pdf) ? "Yes" : "No");
			snprintf(buf, sizeof buf, "%s", fields ? "" : "None");
			if (fields)
				snprintf(buf, sizeof buf, "%d field%s", fields, fields == 1 ? "" : "s");
			add(in, "Form", buf);
			if (files)
				snprintf(buf, sizeof buf, "%d file%s", files, files == 1 ? "" : "s");
			else
				snprintf(buf, sizeof buf, "None");
			add(in, "Attachments", buf);
			add(in, "JavaScript", js ? "Yes" : "No");
			{
				int budget = 10000;
				int marks = count_outline(ctx, pdf_dict_getp(ctx, root, "Outlines/First"), 0, &budget);
				int layers = pdf_array_len(ctx, pdf_dict_getp(ctx, root, "OCProperties/OCGs"));
				if (marks)
					snprintf(buf, sizeof buf, "%d%s", marks, budget ? "" : " or more");
				else
					snprintf(buf, sizeof buf, "None");
				add(in, "Bookmarks", buf);
				if (layers) {
					snprintf(buf, sizeof buf, "%d", layers);
					add(in, "Layers", buf);
				}
			}
		}
		fz_catch(ctx) {
			/* what we have so far */
		}
	}
	return in->n;
}

/* ---- Fonts and pictures ------------------------------------------------- */
/* Every font a PDF's pages use, found the way a reader finds them: each
 * page's resources, the resources of the forms (Form XObjects) and Type 3
 * fonts drawn on it, the appearances of its notes and fields, and the
 * form's default resources. Each font once, by name, kind and how it is
 * held, sorted by name; subset prefixes ("ABCDEF+") are taken off. Picture
 * (image XObject) objects are counted on the way. Big documents stop after
 * a few seconds and say how many pages were looked at. */

typedef struct {
	fz_context *ctx;
	unsigned char *seen;     /* object numbers already walked */
	int nseen;
	folio_fonts *out;
} font_walk;

static int seen_obj(font_walk *fw, pdf_obj *o)
{
	int num;
	if (!pdf_is_indirect(fw->ctx, o))
		return 0;
	num = pdf_to_num(fw->ctx, o);
	if (num <= 0 || num >= fw->nseen)
		return 0;
	if (fw->seen[num])
		return 1;
	fw->seen[num] = 1;
	return 0;
}

/* Font names are bytes; keep printable ASCII (names from Japanese
 * programs are often in Shift-JIS, which no desktop font will show). */
static void clean_name(const char *s, char *out, size_t size)
{
	size_t n = 0;
	for (; *s && n + 1 < size; s++)
		out[n++] = (*s >= 32 && *s < 127) ? *s : '?';
	out[n] = 0;
}

static void font_add(font_walk *fw, pdf_obj *font)
{
	fz_context *ctx = fw->ctx;
	pdf_obj *sub = pdf_dict_get(ctx, font, PDF_NAME(Subtype));
	pdf_obj *d = font, *desc, *ff, *ff2, *ff3, *ff3sub;
	const char *base, *kind;
	char name[sizeof fw->out->font[0].name];
	int emb, i, subset = 0;
	folio_font *f;
	if (!pdf_is_dict(ctx, font))
		return;
	if (pdf_name_eq(ctx, sub, PDF_NAME(Type0)))
		d = pdf_array_get(ctx, pdf_dict_get(ctx, font, PDF_NAME(DescendantFonts)), 0);
	desc = pdf_dict_get(ctx, d, PDF_NAME(FontDescriptor));
	ff = pdf_dict_get(ctx, desc, PDF_NAME(FontFile));
	ff2 = pdf_dict_get(ctx, desc, PDF_NAME(FontFile2));
	ff3 = pdf_dict_get(ctx, desc, PDF_NAME(FontFile3));
	ff3sub = pdf_dict_get(ctx, ff3, PDF_NAME(Subtype));
	emb = ff || ff2 || ff3;
	if (pdf_name_eq(ctx, sub, PDF_NAME(Type0))) {
		pdf_obj *dsub = pdf_dict_get(ctx, d, PDF_NAME(Subtype));
		if (pdf_name_eq(ctx, dsub, PDF_NAME(CIDFontType2)))
			kind = "TrueType (CID)";
		else if (pdf_name_eq(ctx, ff3sub, PDF_NAME(OpenType)))
			kind = "OpenType (CID)";
		else
			kind = ff3 ? "Type 1 (CID, CFF)" : "Type 1 (CID)";
	} else if (pdf_name_eq(ctx, sub, PDF_NAME(Type3))) {
		kind = "Type 3";
		emb = 3;
	} else if (pdf_name_eq(ctx, ff3sub, PDF_NAME(OpenType)))
		kind = "OpenType";
	else if (pdf_name_eq(ctx, sub, PDF_NAME(TrueType)))
		kind = "TrueType";
	else if (pdf_name_eq(ctx, sub, PDF_NAME(MMType1)))
		kind = "Type 1 (multiple master)";
	else if (pdf_name_eq(ctx, sub, PDF_NAME(Type1)))
		kind = ff3 ? "Type 1 (CFF)" : "Type 1";
	else
		kind = "Unknown kind";
	base = pdf_to_name(ctx, pdf_dict_get(ctx, font, PDF_NAME(BaseFont)));
	if (!*base)
		base = pdf_to_name(ctx, pdf_dict_get(ctx, font, PDF_NAME(Name)));
	if (!*base)
		base = "(no name)";
	for (i = 0; i < 6 && base[i] >= 'A' && base[i] <= 'Z'; i++)
		;
	if (i == 6 && base[6] == '+' && base[7]) {
		base += 7;
		subset = 1;
	}
	clean_name(base, name, sizeof name);
	if (emb == 1 && subset)
		emb = 2;
	for (i = 0; i < fw->out->n; i++) {
		f = &fw->out->font[i];
		if (f->embedded == emb && !strcmp(f->name, name) && !strcmp(f->type, kind))
			return;
	}
	if (fw->out->n >= FOLIO_FONTS_MAX) {
		fw->out->more++;
		return;
	}
	f = &fw->out->font[fw->out->n++];
	snprintf(f->name, sizeof f->name, "%s", name);
	snprintf(f->type, sizeof f->type, "%s", kind);
	f->embedded = emb;
}

static void walk_res(font_walk *fw, pdf_obj *res, int depth);

static void walk_stream(font_walk *fw, pdf_obj *xobj, int depth)
{
	if (!pdf_is_dict(fw->ctx, xobj) || seen_obj(fw, xobj))
		return;
	walk_res(fw, pdf_dict_get(fw->ctx, xobj, PDF_NAME(Resources)), depth + 1);
}

static void walk_res(font_walk *fw, pdf_obj *res, int depth)
{
	fz_context *ctx = fw->ctx;
	pdf_obj *fonts, *xobjs;
	int i, n;
	if (depth > 12 || !pdf_is_dict(ctx, res) || seen_obj(fw, res))
		return;
	fonts = pdf_dict_get(ctx, res, PDF_NAME(Font));
	n = pdf_dict_len(ctx, fonts);
	for (i = 0; i < n; i++) {
		pdf_obj *font = pdf_dict_get_val(ctx, fonts, i);
		if (seen_obj(fw, font))
			continue;
		font_add(fw, font);
		/* A Type 3 font's glyphs are drawn with resources of their own. */
		if (pdf_name_eq(ctx, pdf_dict_get(ctx, font, PDF_NAME(Subtype)), PDF_NAME(Type3)))
			walk_res(fw, pdf_dict_get(ctx, font, PDF_NAME(Resources)), depth + 1);
	}
	xobjs = pdf_dict_get(ctx, res, PDF_NAME(XObject));
	n = pdf_dict_len(ctx, xobjs);
	for (i = 0; i < n; i++) {
		pdf_obj *x = pdf_dict_get_val(ctx, xobjs, i);
		pdf_obj *sub = pdf_dict_get(ctx, x, PDF_NAME(Subtype));
		if (pdf_name_eq(ctx, sub, PDF_NAME(Image))) {
			if (!seen_obj(fw, x))
				fw->out->pictures++;
		} else if (pdf_name_eq(ctx, sub, PDF_NAME(Form)))
			walk_stream(fw, x, depth);
	}
}

/* A note's or field's appearance: a stream, or a dictionary of them. */
static void walk_appearance(font_walk *fw, pdf_obj *ap)
{
	fz_context *ctx = fw->ctx;
	int k, i;
	static const char *which[] = { "N", "D", "R" };
	for (k = 0; k < 3; k++) {
		pdf_obj *a = pdf_dict_gets(ctx, ap, which[k]);
		if (pdf_is_stream(ctx, a))
			walk_stream(fw, a, 0);
		else if (pdf_is_dict(ctx, a))
			for (i = 0; i < pdf_dict_len(ctx, a) && i < 16; i++)
				walk_stream(fw, pdf_dict_get_val(ctx, a, i), 0);
	}
}

static int font_order(const void *a, const void *b)
{
	const folio_font *x = a, *y = b;
	int c = fz_strcasecmp(x->name, y->name);
	return c ? c : strcmp(x->type, y->type);
}

int folio_get_fonts(folio_doc *doc, folio_fonts *out, int seconds)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_specifics(ctx, doc->doc);
	font_walk fw;
	clock_t until = clock() + (clock_t)seconds * CLOCKS_PER_SEC;
	int i;
	memset(out, 0, sizeof *out);
	if (!pdf)
		return -1;
	memset(&fw, 0, sizeof fw);
	fw.ctx = ctx;
	fw.out = out;
	fz_var(i);
	fz_try(ctx) {
		fw.nseen = pdf_xref_len(ctx, pdf);
		fw.seen = fz_calloc(ctx, fw.nseen > 0 ? fw.nseen : 1, 1);
		out->pages = pdf_count_pages(ctx, pdf);
		for (i = 0; i < out->pages; i++) {
			pdf_obj *page, *annots;
			int a;
			if (seconds > 0 && i > 0 && clock() > until)
				break;
			fz_try(ctx) {
				page = pdf_lookup_page_obj(ctx, pdf, i);
				walk_res(&fw, pdf_dict_get_inheritable(ctx, page, PDF_NAME(Resources)), 0);
				annots = pdf_dict_get(ctx, page, PDF_NAME(Annots));
				for (a = 0; a < pdf_array_len(ctx, annots) && a < 2000; a++)
					walk_appearance(&fw, pdf_dict_get(ctx, pdf_array_get(ctx, annots, a), PDF_NAME(AP)));
			}
			fz_catch(ctx) {
				/* a broken page: on to the next */
			}
			out->pages_done = i + 1;
		}
		walk_res(&fw, pdf_dict_getp(ctx, pdf_dict_get(ctx, pdf_trailer(ctx, pdf), PDF_NAME(Root)),
			"AcroForm/DR"), 0);
	}
	fz_always(ctx)
		fz_free(ctx, fw.seen);
	fz_catch(ctx) {
		/* what was found so far */
	}
	qsort(out->font, out->n, sizeof out->font[0], font_order);
	return out->n;
}
