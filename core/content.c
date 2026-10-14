/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio core: what's in a PDF that a careful reader may want to know about
 * before going on: JavaScript, attached files, a form, and actions that
 * start other programs, open files or send the form somewhere. Read from the
 * PDF's objects without loading its pages. */
#include "folio_int.h"
#include <string.h>

#define MAX_FIELDS 5000
#define MAX_PAGES 5000

static int is_name(fz_context *ctx, pdf_obj *o, const char *name)
{
	return pdf_is_name(ctx, o) && !strcmp(pdf_to_name(ctx, o), name);
}

/* One action (and those chained after it with /Next). */
static void action(fz_context *ctx, pdf_obj *a, folio_content *c, int depth)
{
	pdf_obj *s, *next;
	int i;
	if (!pdf_is_dict(ctx, a) || depth > 8)
		return;
	s = pdf_dict_get(ctx, a, PDF_NAME(S));
	if (pdf_name_eq(ctx, s, PDF_NAME(JavaScript)))
		c->javascript++;
	else if (pdf_name_eq(ctx, s, PDF_NAME(Launch)))
		c->launch++;
	else if (is_name(ctx, s, "SubmitForm") || is_name(ctx, s, "ImportData"))
		c->submit++;
	else if (pdf_name_eq(ctx, s, PDF_NAME(URI)))
		c->uri++;
	else if (pdf_name_eq(ctx, s, PDF_NAME(GoToR)) || is_name(ctx, s, "GoToE"))
		c->other_files++;
	next = pdf_dict_get(ctx, a, PDF_NAME(Next));
	if (pdf_is_array(ctx, next))
		for (i = 0; i < pdf_array_len(ctx, next) && i < 16; i++)
			action(ctx, pdf_array_get(ctx, next, i), c, depth + 1);
	else
		action(ctx, next, c, depth + 1);
}

/* An additional-actions dictionary (/AA): each of its actions. */
static void actions(fz_context *ctx, pdf_obj *aa, folio_content *c)
{
	int i, n = pdf_dict_len(ctx, aa);
	for (i = 0; i < n && i < 32; i++)
		action(ctx, pdf_dict_get_val(ctx, aa, i), c, 0);
}

static int count_names(fz_context *ctx, pdf_obj *tree, int depth)
{
	pdf_obj *kids;
	int n = 0, i;
	if (!pdf_is_dict(ctx, tree) || depth > 8)
		return 0;
	n += pdf_array_len(ctx, pdf_dict_get(ctx, tree, PDF_NAME(Names))) / 2;
	kids = pdf_dict_get(ctx, tree, PDF_NAME(Kids));
	for (i = 0; i < pdf_array_len(ctx, kids) && i < 1000; i++)
		n += count_names(ctx, pdf_array_get(ctx, kids, i), depth + 1);
	return n;
}

/* The form's fields: terminal ones counted, and their actions. */
static void fields(fz_context *ctx, pdf_obj *list, folio_content *c, int depth, int *seen)
{
	int i;
	if (depth > 16)
		return;
	for (i = 0; i < pdf_array_len(ctx, list) && *seen < MAX_FIELDS; i++) {
		pdf_obj *f = pdf_array_get(ctx, list, i);
		pdf_obj *kids = pdf_dict_get(ctx, f, PDF_NAME(Kids));
		(*seen)++;
		actions(ctx, pdf_dict_get(ctx, f, PDF_NAME(AA)), c);
		action(ctx, pdf_dict_get(ctx, f, PDF_NAME(A)), c, 0);
		if (pdf_array_len(ctx, kids) > 0 && !pdf_dict_get(ctx, pdf_array_get(ctx, kids, 0), PDF_NAME(Subtype)))
			fields(ctx, kids, c, depth + 1, seen);
		else
			c->fields++;
	}
}

int folio_get_content(folio_doc *doc, folio_content *c)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf;
	memset(c, 0, sizeof *c);
	folio_begin(doc);
	pdf = pdf_specifics(ctx, doc->doc);
	if (!pdf)
		return 0;
	c->pdf = 1;
	fz_try(ctx) {
		pdf_obj *root = pdf_dict_get(ctx, pdf_trailer(ctx, pdf), PDF_NAME(Root));
		pdf_obj *names = pdf_dict_get(ctx, root, PDF_NAME(Names));
		pdf_obj *acro = pdf_dict_get(ctx, root, PDF_NAME(AcroForm));
		int i, n, seen = 0;
		c->javascript += count_names(ctx, pdf_dict_get(ctx, names, PDF_NAME(JavaScript)), 0);
		c->files += count_names(ctx, pdf_dict_get(ctx, names, PDF_NAME(EmbeddedFiles)), 0);
		action(ctx, pdf_dict_gets(ctx, root, "OpenAction"), c, 0);
		actions(ctx, pdf_dict_get(ctx, root, PDF_NAME(AA)), c);
		fields(ctx, pdf_dict_get(ctx, acro, PDF_NAME(Fields)), c, 0, &seen);
		c->xfa = pdf_dict_get(ctx, acro, PDF_NAME(XFA)) != NULL;
		n = pdf_count_pages(ctx, pdf);
		for (i = 0; i < n && i < MAX_PAGES; i++) {
			pdf_obj *page = pdf_lookup_page_obj(ctx, pdf, i);
			pdf_obj *annots = pdf_dict_get(ctx, page, PDF_NAME(Annots));
			int k;
			actions(ctx, pdf_dict_get(ctx, page, PDF_NAME(AA)), c);
			for (k = 0; k < pdf_array_len(ctx, annots) && k < 2000; k++) {
				pdf_obj *a = pdf_array_get(ctx, annots, k);
				pdf_obj *sub = pdf_dict_get(ctx, a, PDF_NAME(Subtype));
				if (pdf_name_eq(ctx, sub, PDF_NAME(FileAttachment)))
					c->files++;
				else if (pdf_name_eq(ctx, sub, PDF_NAME(Widget)))
					continue;          /* counted with the form */
				action(ctx, pdf_dict_get(ctx, a, PDF_NAME(A)), c, 0);
				actions(ctx, pdf_dict_get(ctx, a, PDF_NAME(AA)), c);
			}
		}
	}
	fz_catch(ctx) {
		/* what was found so far */
	}
	return 0;
}
