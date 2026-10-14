/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio core: filling in PDF forms.
 *
 * A page's fields are its widgets, numbered in the page's order (the order
 * Tab goes through them). Text fields take typed text; check boxes and
 * radio buttons toggle; combo and list boxes choose one of their options.
 * Each change is one operation in MuPDF's journal (undo and redo step
 * through them, as for notes and markings) and is saved with the PDF.
 *
 * JavaScript (the form's own checks, formats and totals) runs only when
 * folio_form_js turns it on. MuPDF's scripts can't reach files or the
 * network; what they ask of the program (an alert, opening a web page) is
 * kept here for the caller to show or ask about (folio_form_take_alert,
 * folio_form_take_url). Nothing is sent anywhere: a form's Submit is not
 * followed. */
#include "folio_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pdf_document *pdf_of(folio_doc *doc)
{
	return pdf_specifics(doc->ctx, doc->doc);
}

static void forget_all(folio_doc *doc)
{
	int i;
	for (i = 0; i < FOLIO_PAGE_CACHE; i++)
		if (doc->cache[i].used)
			folio_drop_cpage(doc, &doc->cache[i]);
}

/* The page's widget number 'index' (not kept), or NULL. */
static pdf_annot *widget_n(fz_context *ctx, pdf_page *page, int index)
{
	pdf_annot *w;
	int i = 0;
	for (w = pdf_first_widget(ctx, page); w; w = pdf_next_widget(ctx, w), i++)
		if (i == index)
			return w;
	return NULL;
}

static folio_field_kind kind_of(enum pdf_widget_type t)
{
	switch (t) {
	case PDF_WIDGET_TYPE_TEXT: return FOLIO_FIELD_TEXT;
	case PDF_WIDGET_TYPE_CHECKBOX: return FOLIO_FIELD_CHECK;
	case PDF_WIDGET_TYPE_RADIOBUTTON: return FOLIO_FIELD_RADIO;
	case PDF_WIDGET_TYPE_COMBOBOX: return FOLIO_FIELD_COMBO;
	case PDF_WIDGET_TYPE_LISTBOX: return FOLIO_FIELD_LIST;
	case PDF_WIDGET_TYPE_BUTTON: return FOLIO_FIELD_BUTTON;
	case PDF_WIDGET_TYPE_SIGNATURE: return FOLIO_FIELD_SIGNATURE;
	default: return FOLIO_FIELD_NONE;
	}
}

static void describe(fz_context *ctx, folio_cpage *cp, pdf_annot *w, int index, folio_field *f)
{
	fz_rect r = pdf_bound_widget(ctx, w);
	int flags = pdf_field_flags(ctx, pdf_annot_obj(ctx, w));
	char *name;
	const char *value;
	pdf_obj *as;
	memset(f, 0, sizeof *f);
	f->kind = kind_of(pdf_widget_type(ctx, w));
	f->index = index;
	f->rect.x0 = r.x0 - cp->bounds.x0;
	f->rect.y0 = r.y0 - cp->bounds.y0;
	f->rect.x1 = r.x1 - cp->bounds.x0;
	f->rect.y1 = r.y1 - cp->bounds.y0;
	name = pdf_load_field_name(ctx, pdf_annot_obj(ctx, w));
	snprintf(f->name, sizeof f->name, "%s", name ? name : "");
	fz_free(ctx, name);
	value = pdf_annot_field_value(ctx, w);
	snprintf(f->value, sizeof f->value, "%s", value ? value : "");
	f->read_only = pdf_widget_is_readonly(ctx, w);
	f->multiline = (flags & PDF_TX_FIELD_IS_MULTILINE) != 0;
	f->password = (flags & PDF_TX_FIELD_IS_PASSWORD) != 0;
	if (f->kind == FOLIO_FIELD_TEXT)
		f->max_len = pdf_text_widget_max_len(ctx, w);
	as = pdf_dict_get(ctx, pdf_annot_obj(ctx, w), PDF_NAME(AS));
	f->on = pdf_is_name(ctx, as) && strcmp(pdf_to_name(ctx, as), "Off") != 0;
	if (f->kind == FOLIO_FIELD_COMBO || f->kind == FOLIO_FIELD_LIST)
		f->options = pdf_choice_widget_options(ctx, w, 0, NULL);
	if (f->kind == FOLIO_FIELD_SIGNATURE)
		f->read_only = 1;
}

int folio_field_count(folio_doc *doc, folio_loc loc)
{
	fz_context *ctx = doc->ctx;
	int n = 0;
	pdf_annot *w;
	if (!pdf_of(doc))
		return 0;
	fz_var(n);
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		for (w = page ? pdf_first_widget(ctx, page) : NULL; w; w = pdf_next_widget(ctx, w))
			n++;
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return n;
}

int folio_field_get(folio_doc *doc, folio_loc loc, int index, folio_field *f)
{
	fz_context *ctx = doc->ctx;
	int found = 0;
	if (!pdf_of(doc))
		return 0;
	fz_var(found);
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		pdf_annot *w = page ? widget_n(ctx, page, index) : NULL;
		if (w) {
			describe(ctx, cp, w, index, f);
			found = 1;
		}
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return found;
}

int folio_field_at(folio_doc *doc, folio_loc loc, folio_point p, folio_field *f)
{
	fz_context *ctx = doc->ctx;
	int found = 0;
	if (!pdf_of(doc))
		return 0;
	fz_var(found);
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		fz_point q = fz_make_point(p.x + cp->bounds.x0, p.y + cp->bounds.y0);
		pdf_annot *w;
		int i = 0;
		/* The last one drawn (on top) wins. */
		for (w = page ? pdf_first_widget(ctx, page) : NULL; w; w = pdf_next_widget(ctx, w), i++) {
			fz_rect r = pdf_bound_widget(ctx, w);
			int hidden = pdf_annot_flags(ctx, w) & (PDF_ANNOT_IS_HIDDEN | PDF_ANNOT_IS_NO_VIEW);
			if (!hidden && fz_is_point_inside_rect(q, r)) {
				describe(ctx, cp, w, i, f);
				found = 1;
			}
		}
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return found;
}

int folio_field_option(folio_doc *doc, folio_loc loc, int index, int i, char *buf, size_t size)
{
	fz_context *ctx = doc->ctx;
	const char **opts = NULL;
	int ok = -1;
	if (!pdf_of(doc))
		return -1;
	fz_var(ok);
	fz_var(opts);
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		pdf_annot *w = page ? widget_n(ctx, page, index) : NULL;
		int n = w ? pdf_choice_widget_options(ctx, w, 0, NULL) : 0;
		if (i >= 0 && i < n) {
			opts = fz_malloc_array(ctx, n, const char *);
			pdf_choice_widget_options(ctx, w, 0, opts);
			snprintf(buf, size, "%s", opts[i] ? opts[i] : "");
			ok = 0;
		}
	}
	fz_always(ctx)
		fz_free(ctx, opts);
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		return -1;
	}
	return ok;
}

/* After a change: the page's appearances made again (and the form's totals,
 * if its scripts run), and every cached page dropped, as a total on one page
 * can change a field on another. */
static void changed(fz_context *ctx, folio_doc *doc, pdf_page *page)
{
	pdf_update_page(ctx, page);
	forget_all(doc);
}

int folio_field_set_text(folio_doc *doc, folio_loc loc, int index, const char *utf8)
{
	fz_context *ctx = doc->ctx;
	int rc = -1;
	fz_var(rc);
	if (!pdf_of(doc)) {
		snprintf(doc->fc->err, sizeof doc->fc->err, "this document has no form");
		return -1;
	}
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		pdf_annot *w = page ? widget_n(ctx, page, index) : NULL;
		enum pdf_widget_type t = w ? pdf_widget_type(ctx, w) : PDF_WIDGET_TYPE_UNKNOWN;
		if (!w || (t != PDF_WIDGET_TYPE_TEXT && t != PDF_WIDGET_TYPE_COMBOBOX))
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "not a text field");
		if (pdf_widget_is_readonly(ctx, w))
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "the field can't be changed");
		/* 1: taken; 0: the form's script turned it down. */
		rc = pdf_set_text_field_value(ctx, w, utf8 ? utf8 : "") ? 0 : 1;
		changed(ctx, doc, page);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		forget_all(doc);
		return -1;
	}
	return rc;
}

int folio_field_toggle(folio_doc *doc, folio_loc loc, int index)
{
	fz_context *ctx = doc->ctx;
	if (!pdf_of(doc))
		return -1;
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		pdf_annot *w = page ? widget_n(ctx, page, index) : NULL;
		if (!w || pdf_widget_is_readonly(ctx, w) || !pdf_toggle_widget(ctx, w))
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "not a check box or radio button that can change");
		changed(ctx, doc, page);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		forget_all(doc);
		return -1;
	}
	return 0;
}

int folio_field_choose(folio_doc *doc, folio_loc loc, int index, int option)
{
	fz_context *ctx = doc->ctx;
	const char **opts = NULL;
	fz_var(opts);
	if (!pdf_of(doc))
		return -1;
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		pdf_annot *w = page ? widget_n(ctx, page, index) : NULL;
		int n = w ? pdf_choice_widget_options(ctx, w, 1, NULL) : 0;
		if (!w || pdf_widget_is_readonly(ctx, w) || option < 0 || option >= n)
			fz_throw(ctx, FZ_ERROR_ARGUMENT, "no such choice");
		opts = fz_malloc_array(ctx, n, const char *);
		pdf_choice_widget_options(ctx, w, 1, opts);   /* export values */
		pdf_choice_widget_set_value(ctx, w, 1, &opts[option]);
		changed(ctx, doc, page);
	}
	fz_always(ctx)
		fz_free(ctx, opts);
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		forget_all(doc);
		return -1;
	}
	return 0;
}

int folio_field_press(folio_doc *doc, folio_loc loc, int index)
{
	fz_context *ctx = doc->ctx;
	int ran = 0;
	fz_var(ran);
	if (!pdf_of(doc))
		return -1;
	folio_begin(doc);
	fz_try(ctx) {
		folio_cpage *cp = folio_get_page(doc, loc);
		pdf_page *page = pdf_page_from_fz_page(ctx, cp->page);
		pdf_annot *w = page ? widget_n(ctx, page, index) : NULL;
		if (w && doc->js) {
			pdf_annot_event_down(ctx, w);
			pdf_annot_event_up(ctx, w);
			ran = 1;
			changed(ctx, doc, page);
		}
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		forget_all(doc);
		return -1;
	}
	return ran;
}

/* ---- JavaScript ------------------------------------------------------------ */

static void on_event(fz_context *ctx, pdf_document *pdf, pdf_doc_event *evt, void *data)
{
	folio_doc *doc = data;
	(void)pdf;
	switch (evt->type) {
	case PDF_DOCUMENT_EVENT_ALERT: {
		pdf_alert_event *a = pdf_access_alert_event(ctx, evt);
		snprintf(doc->js_alert, sizeof doc->js_alert, "%s", a->message ? a->message : "");
		a->button_pressed = PDF_ALERT_BUTTON_OK;
		break;
	}
	case PDF_DOCUMENT_EVENT_LAUNCH_URL: {
		pdf_launch_url_event *u = pdf_access_launch_url_event(ctx, evt);
		snprintf(doc->js_url, sizeof doc->js_url, "%s", u->url ? u->url : "");
		break;
	}
	default:
		/* Printing, mailing, submitting, menu items: not done. */
		break;
	}
}

int folio_form_js(folio_doc *doc, int on)
{
	fz_context *ctx = doc->ctx;
	pdf_document *pdf = pdf_of(doc);
	if (!pdf || !on || doc->js)
		return doc->js;
	folio_begin(doc);
	fz_try(ctx) {
		pdf_set_doc_event_callback(ctx, pdf, on_event, NULL, doc);
		pdf_enable_js(ctx, pdf);
		doc->js = pdf_js_supported(ctx, pdf);
	}
	fz_catch(ctx) {
		folio_set_error(doc->fc);
		doc->js = 0;
	}
	return doc->js;
}

int folio_form_take_alert(folio_doc *doc, char *buf, size_t size)
{
	if (!doc->js_alert[0])
		return 0;
	snprintf(buf, size, "%s", doc->js_alert);
	doc->js_alert[0] = 0;
	return 1;
}

int folio_form_take_url(folio_doc *doc, char *buf, size_t size)
{
	if (!doc->js_url[0])
		return 0;
	snprintf(buf, size, "%s", doc->js_url);
	doc->js_url[0] = 0;
	return 1;
}
