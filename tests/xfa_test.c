/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Folio XFA form tests (core/xfa.c), run under qemu by `make test`:
 *   xfa_test tests/data
 * Uses xfa-static.pdf and xfa-dynamic.pdf (tests/make-xfa.py) and form.pdf.
 * Writes tests/out/xfa-*.pdf. */
#include "folio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static folio_ctx *fc;
static const char *dir;
static folio_xfa_field F[32];
static const folio_loc P0 = { 0, 0 };

static const char *data(const char *leaf)
{
	static char b[4][512];
	static int k;
	k = (k + 1) & 3;
	snprintf(b[k], sizeof b[k], "%s/%s", dir, leaf);
	return b[k];
}

static const char *out(const char *leaf)
{
	static char b[512];
	snprintf(b, sizeof b, "%s/../out/%s", dir, leaf);
	return b;
}

/* The fields; *n how many. The one at path, or NULL. */
static folio_xfa_field *get(folio_doc *d, const char *path, int *count)
{
	int n = folio_xfa_fields(d, F, 32), i;
	if (count)
		*count = n;
	for (i = 0; i < n && i < 32; i++)
		if (!strcmp(F[i].path, path))
			return &F[i];
	return NULL;
}

static const char *value(folio_doc *d, const char *path)
{
	folio_xfa_field *f = get(d, path, NULL);
	return f ? f->value : "(none)";
}

static void list(folio_doc *d)
{
	int n = folio_xfa_fields(d, F, 32), i;
	for (i = 0; i < n && i < 32; i++)
		printf("  %-24s %-12s type %d ro %d '%s' (%d opts, chosen %d)\n", F[i].path, F[i].label, F[i].type,
			F[i].read_only, F[i].value, F[i].nopts, F[i].chosen);
}

/* The page's ordinary field called name: its value. */
static const char *acro(folio_doc *d, const char *name, int *index)
{
	static folio_field f;
	int i, n = folio_field_count(d, P0);
	for (i = 0; i < n; i++)
		if (folio_field_get(d, P0, i, &f) == 1 && !strcmp(f.name, name)) {
			if (index)
				*index = i;
			return f.value;
		}
	return "(none)";
}

static void common(folio_doc *d, const char *what)
{
	folio_xfa_field *f;
	int n;
	f = get(d, "form1.Page1.Name", &n);
	CHECK(n == 10, "%s: 10 fields (%d)", what, n);
	CHECK(f && !strcmp(f->label, "Your name") && f->type == FOLIO_XFA_TEXT && !strcmp(f->value, "Ada & Co"),
		"%s: Name '%s' '%s'", what, f ? f->label : "", f ? f->value : "");
	f = get(d, "form1.Page1.Agree", NULL);
	CHECK(f && f->type == FOLIO_XFA_CHECK && f->nopts == 2 && !strcmp(f->save[0], "1") && !strcmp(f->value, "0"),
		"%s: Agree", what);
	f = get(d, "form1.Page1.Colour", NULL);
	CHECK(f && f->type == FOLIO_XFA_CHOICE && f->nopts == 3 && !strcmp(f->opt[1], "Green") &&
		!strcmp(f->save[1], "G") && f->chosen == 1, "%s: Colour", what);
	f = get(d, "form1.Page1.Size", NULL);
	CHECK(f && f->type == FOLIO_XFA_CHOICE && f->nopts == 3 && !strcmp(f->opt[2], "Large") &&
		!strcmp(f->save[2], "L") && f->chosen == -1 && !f->value[0], "%s: Size (radio group)", what);
	f = get(d, "form1.Page1.Total", NULL);
	CHECK(f && f->read_only && f->type == FOLIO_XFA_NUMBER && !strcmp(f->value, "12"), "%s: Total", what);
	CHECK(!strcmp(value(d, "form1.Item.Price"), "5") && !strcmp(value(d, "form1.Item[1].Price"), "7"),
		"%s: repeated Item", what);
	f = get(d, "form1.Page1.Notes", NULL);
	CHECK(f && f->type == FOLIO_XFA_MULTILINE && !f->value[0], "%s: Notes, no data yet", what);
	f = get(d, "form1.Page1.Town", NULL);
	CHECK(f && !strcmp(f->label, "Town or city"), "%s: Town through an unnamed subform, tool tip", what);
	CHECK(get(d, "form1.Contact.Phone", NULL) != NULL, "%s: Phone by dataRef", what);
	CHECK(!get(d, "form1.Page1.Secret", NULL) && !get(d, "form1.Page1.Send", NULL), "%s: no Secret, no button", what);

	/* Changes. */
	CHECK(folio_xfa_set(d, "form1.Page1.Name", "Bea <B&B>") == 0, "%s: set Name", what);
	CHECK(!strcmp(value(d, "form1.Page1.Name"), "Bea <B&B>"), "%s: Name now '%s'", what,
		value(d, "form1.Page1.Name"));
	CHECK(folio_xfa_set(d, "form1.Page1.Size", "L") == 0 && (f = get(d, "form1.Page1.Size", NULL)) &&
		f->chosen == 2, "%s: Size set in <Size/>", what);
	CHECK(folio_xfa_set(d, "form1.Page1.Notes", "Line one\nLine two") == 0 &&
		!strcmp(value(d, "form1.Page1.Notes"), "Line one\nLine two"), "%s: Notes made", what);
	CHECK(folio_xfa_set(d, "form1.Contact.Phone", "01252 000000") == 0 &&
		!strcmp(value(d, "form1.Contact.Phone"), "01252 000000"), "%s: Phone made with its group", what);
	CHECK(folio_xfa_set(d, "form1.Item[1].Price", "9") == 0 && !strcmp(value(d, "form1.Item[1].Price"), "9") &&
		!strcmp(value(d, "form1.Item.Price"), "5"), "%s: second Item's Price", what);
	CHECK(folio_xfa_set(d, "form1.Page1.Total", "99") == -1, "%s: read-only refused", what);
	get(d, "form1.Page1.Name", &n);
	CHECK(n == 10, "%s: still 10 fields (%d)", what, n);
	CHECK(folio_is_modified(d), "%s: modified", what);
	CHECK(folio_undo(d) == 0 && !strcmp(value(d, "form1.Item[1].Price"), "7"), "%s: undo", what);
	CHECK(folio_redo(d) == 0 && !strcmp(value(d, "form1.Item[1].Price"), "9"), "%s: redo", what);
}

static void saved(const char *path, const char *what)
{
	folio_doc *d = folio_open(fc, path, NULL);
	CHECK(d != NULL, "%s: reopens", what);
	if (!d)
		return;
	CHECK(!strcmp(value(d, "form1.Page1.Name"), "Bea <B&B>") && !strcmp(value(d, "form1.Page1.Size"), "L") &&
		!strcmp(value(d, "form1.Contact.Phone"), "01252 000000") && !strcmp(value(d, "form1.Item[1].Price"), "9") &&
		!strcmp(value(d, "form1.Page1.Notes"), "Line one\nLine two") && !strcmp(value(d, "form1.Page1.Colour"), "G"),
		"%s: values kept", what);
	folio_close(d);
}

int main(int argc, char **argv)
{
	folio_doc *d;
	int i;
	dir = argc > 1 ? argv[1] : "tests/data";
	mkdir(out(""), 0777);
	fc = folio_new(0);

	d = folio_open(fc, data("form.pdf"), NULL);
	CHECK(d && folio_xfa_kind(d) == FOLIO_XFA_NONE && folio_xfa_fields(d, F, 32) == 0, "form.pdf: not XFA");
	if (d)
		folio_close(d);

	/* Static: the ordinary fields keep in step. */
	d = folio_open(fc, data("xfa-static.pdf"), NULL);
	CHECK(d && folio_xfa_kind(d) == FOLIO_XFA_HYBRID, "static: hybrid");
	if (d) {
		list(d);
		common(d, "static");
		CHECK(!strcmp(acro(d, "form1[0].Page1[0].Name[0]", NULL), "Bea <B&B>"), "static: page field follows (%s)",
			acro(d, "form1[0].Page1[0].Name[0]", NULL));
		CHECK(folio_field_set_text(d, P0, 0, "Cy") == 0 && !strcmp(value(d, "form1.Page1.Name"), "Cy"),
			"static: typing on the page goes into the data ('%s')", value(d, "form1.Page1.Name"));
		acro(d, "form1[0].Page1[0].Agree[0]", &i);
		CHECK(folio_field_toggle(d, P0, i) == 0 && !strcmp(value(d, "form1.Page1.Agree"), "1"),
			"static: tick on the page: Agree '%s'", value(d, "form1.Page1.Agree"));
		CHECK(folio_undo(d) == 0 && !strcmp(value(d, "form1.Page1.Agree"), "0"), "static: one undo for both");
		CHECK(folio_xfa_set(d, "form1.Page1.Agree", "1") == 0 &&
			strcmp(acro(d, "form1[0].Page1[0].Agree[0]", NULL), "Off"), "static: tick in the list ticks the page (%s)",
			acro(d, "form1[0].Page1[0].Agree[0]", NULL));
		CHECK(folio_xfa_set(d, "form1.Page1.Agree", "0") == 0 &&
			!strcmp(acro(d, "form1[0].Page1[0].Agree[0]", NULL), "Off"), "static: and off");
		CHECK(folio_xfa_set(d, "form1.Page1.Name", "Bea <B&B>") == 0, "static: Name again");
		CHECK(folio_save_pdf(d, data("xfa-static.pdf"), out("xfa-static.pdf")) == 0, "static: saved");
		folio_close(d);
		saved(out("xfa-static.pdf"), "static");
		d = folio_open(fc, out("xfa-static.pdf"), NULL);
		CHECK(d && !strcmp(acro(d, "form1[0].Page1[0].Name[0]", NULL), "Bea <B&B>"), "static: page field saved");
		if (d)
			folio_close(d);
	}

	/* Dynamic, one stream. */
	d = folio_open(fc, data("xfa-dynamic.pdf"), NULL);
	CHECK(d && folio_xfa_kind(d) == FOLIO_XFA_DYNAMIC, "dynamic");
	if (d) {
		common(d, "dynamic");
		CHECK(folio_save_pdf(d, data("xfa-dynamic.pdf"), out("xfa-dynamic.pdf")) == 0, "dynamic: saved");
		folio_close(d);
		saved(out("xfa-dynamic.pdf"), "dynamic");
	}
	folio_drop(fc);
	printf("xfa_test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
