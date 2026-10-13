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
		}
		fz_catch(ctx) {
			/* what we have so far */
		}
	}
	return in->n;
}
