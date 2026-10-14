/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Folio form and security tests (core/form.c, core/content.c), run under qemu
 * by `make test`:
 *   form_test tests/data
 * Uses form.pdf (tests/make-form.py) and doc.pdf. Writes tests/out/form-*.pdf. */
#include "folio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static char path_buf[512];
static const char *data(const char *dir, const char *leaf)
{
	snprintf(path_buf, sizeof path_buf, "%s/%s", dir, leaf);
	return path_buf;
}

static const folio_loc P0 = { 0, 0 };

/* The page's field called name: its index, and *f. -1 if none. */
static int field(folio_doc *d, const char *name, folio_field *f)
{
	int i, n = folio_field_count(d, P0);
	for (i = 0; i < n; i++)
		if (folio_field_get(d, P0, i, f) == 1 && !strcmp(f->name, name))
			return i;
	return -1;
}

/* A field's middle, for folio_field_at. */
static folio_point middle(const folio_field *f)
{
	folio_point p = { (f->rect.x0 + f->rect.x1) / 2, (f->rect.y0 + f->rect.y1) / 2 };
	return p;
}

static void test_content(folio_ctx *fc, const char *dir)
{
	folio_content c;
	folio_doc *d = folio_open(fc, data(dir, "form.pdf"), NULL);
	CHECK(d != NULL, "open form.pdf: %s", folio_error(fc));
	if (!d)
		return;
	folio_get_content(d, &c);
	CHECK(c.pdf, "a PDF");
	CHECK(c.javascript >= 2, "scripts: opening and the total (%d)", c.javascript);
	CHECK(c.files == 1, "one attached file (%d)", c.files);
	CHECK(c.fields >= 10, "fields (%d)", c.fields);
	CHECK(c.launch == 1, "one launch action (%d)", c.launch);
	CHECK(c.submit == 1, "one submit action (%d)", c.submit);
	CHECK(c.uri == 1, "one web link (%d)", c.uri);
	folio_close(d);
	d = folio_open(fc, data(dir, "doc.pdf"), NULL);
	if (d) {
		folio_get_content(d, &c);
		CHECK(c.pdf && !c.javascript && !c.files && !c.fields && !c.launch && !c.submit,
			"doc.pdf has none: js %d files %d fields %d launch %d submit %d",
			c.javascript, c.files, c.fields, c.launch, c.submit);
		folio_close(d);
	}
	d = folio_open(fc, data(dir, "book.epub"), NULL);
	if (d) {
		folio_get_content(d, &c);
		CHECK(!c.pdf, "an EPUB isn't a PDF");
		CHECK(folio_field_count(d, P0) == 0, "an EPUB has no fields");
		folio_close(d);
	}
}

static void test_fields(folio_ctx *fc, const char *dir)
{
	folio_field f, g;
	char opt[64];
	int i, n, name, agree, colour, fruit, addr, total;
	folio_doc *d = folio_open(fc, data(dir, "form.pdf"), NULL);
	CHECK(d != NULL, "open form.pdf: %s", folio_error(fc));
	if (!d)
		return;
	n = folio_field_count(d, P0);
	CHECK(n == 12, "12 widgets (%d)", n);
	name = field(d, "name", &f);
	CHECK(name >= 0 && f.kind == FOLIO_FIELD_TEXT && !strcmp(f.value, "Ada") && !f.multiline,
		"name: kind %d value '%s'", f.kind, f.value);
	CHECK(f.rect.x0 > 149 && f.rect.x1 < 451 && f.rect.y0 > 79 && f.rect.y1 < 103,
		"name's box from the top left: %g,%g %g,%g", f.rect.x0, f.rect.y0, f.rect.x1, f.rect.y1);
	CHECK(folio_field_at(d, P0, middle(&f), &g) == 1 && g.index == name, "found at its middle");
	CHECK(folio_field_at(d, P0, (folio_point){ 20, 20 }, &g) == 0, "nothing at the top left");
	addr = field(d, "address", &f);
	CHECK(addr >= 0 && f.multiline, "address is multi-line");
	agree = field(d, "agree", &f);
	CHECK(agree >= 0 && f.kind == FOLIO_FIELD_CHECK && !f.on, "agree: a check box, off");
	colour = field(d, "colour", &f);
	CHECK(colour >= 0 && f.kind == FOLIO_FIELD_COMBO && f.options == 3 && !strcmp(f.value, "Green"),
		"colour: combo of %d, '%s'", f.options, f.value);
	CHECK(folio_field_option(d, P0, colour, 2, opt, sizeof opt) == 0 && !strcmp(opt, "Blue"),
		"third option '%s'", opt);
	fruit = field(d, "fruit", &f);
	CHECK(fruit >= 0 && f.kind == FOLIO_FIELD_LIST && f.options == 3, "fruit: a list");
	total = field(d, "total", &f);
	CHECK(total >= 0 && f.read_only, "total is read-only");
	/* Radio buttons: three widgets of "size", M on. */
	{
		int on = -1, count = 0;
		for (i = 0; i < n; i++)
			if (folio_field_get(d, P0, i, &g) == 1 && !strcmp(g.name, "size")) {
				CHECK(g.kind == FOLIO_FIELD_RADIO, "size is radio");
				if (g.on)
					on = count;
				count++;
			}
		CHECK(count == 3 && on == 1, "three sizes, the second on (%d of %d)", on, count);
	}

	/* Changing. */
	CHECK(!folio_is_modified(d), "not modified when opened");
	CHECK(folio_field_set_text(d, P0, name, "Grace Hopper") == 0, "set name: %s", folio_error(fc));
	folio_field_get(d, P0, name, &f);
	CHECK(!strcmp(f.value, "Grace Hopper"), "name now '%s'", f.value);
	CHECK(folio_is_modified(d), "modified");
	CHECK(folio_field_set_text(d, P0, addr, "1 Long Lane\nCambridge") == 0, "set address");
	CHECK(folio_field_toggle(d, P0, agree) == 0, "tick agree");
	folio_field_get(d, P0, agree, &f);
	CHECK(f.on, "agree on");
	CHECK(folio_field_choose(d, P0, colour, 2) == 0, "choose Blue");
	folio_field_get(d, P0, colour, &f);
	CHECK(!strcmp(f.value, "Blue"), "colour '%s'", f.value);
	CHECK(folio_field_set_text(d, P0, total, "99") == -1, "a read-only field refuses");
	/* Radio: the third size. */
	for (i = 0; i < n; i++)
		if (folio_field_get(d, P0, i, &g) == 1 && !strcmp(g.name, "size") && g.rect.x0 > 240)
			break;
	CHECK(i < n && folio_field_toggle(d, P0, i) == 0, "choose L");
	folio_field_get(d, P0, i, &g);
	CHECK(g.on && !strcmp(g.value, "L"), "L on, size '%s'", g.value);
	/* Undo the colour, then the radio. */
	CHECK(folio_undo(d) == 0, "undo L");
	CHECK(folio_undo(d) == 0, "undo colour");
	folio_field_get(d, P0, colour, &f);
	CHECK(!strcmp(f.value, "Green"), "undone: colour '%s'", f.value);
	CHECK(folio_redo(d) == 0, "redo colour");
	folio_field_get(d, P0, colour, &f);
	CHECK(!strcmp(f.value, "Blue"), "redone: colour '%s'", f.value);

	/* Saved and opened again. */
	mkdir("tests/out", 0777);
	CHECK(folio_save_pdf(d, data(dir, "form.pdf"), "tests/out/form-filled.pdf") == 0,
		"save: %s", folio_error(fc));
	folio_close(d);
	d = folio_open(fc, "tests/out/form-filled.pdf", NULL);
	CHECK(d != NULL, "reopen");
	if (d) {
		folio_field_get(d, P0, field(d, "name", &f), &f);
		CHECK(!strcmp(f.value, "Grace Hopper"), "kept name '%s'", f.value);
		field(d, "address", &f);
		CHECK(strstr(f.value, "Cambridge") != NULL, "kept address '%s'", f.value);
		field(d, "agree", &f);
		CHECK(f.on, "kept agree");
		field(d, "colour", &f);
		CHECK(!strcmp(f.value, "Blue"), "kept colour '%s'", f.value);
		folio_close(d);
	}
}

static void test_js(folio_ctx *fc, const char *dir)
{
	folio_field f;
	int a, total, on;
	char buf[128];
	folio_doc *d = folio_open(fc, data(dir, "form.pdf"), NULL);
	if (!d)
		return;
	/* Without JavaScript the total doesn't follow. */
	a = field(d, "a", &f);
	total = field(d, "total", &f);
	CHECK(folio_field_set_text(d, P0, a, "10") == 0, "a = 10");
	field(d, "total", &f);
	CHECK(!strcmp(f.value, "5"), "no scripts: total stays '%s'", f.value);
	folio_close(d);

	d = folio_open(fc, data(dir, "form.pdf"), NULL);
	if (!d)
		return;
	on = folio_form_js(d, 1);
	printf("  JavaScript %s\n", on ? "on" : "not built in");
	if (on) {
		a = field(d, "a", &f);
		CHECK(folio_field_set_text(d, P0, a, "10") == 0, "a = 10 (js)");
		field(d, "total", &f);
		CHECK(!strcmp(f.value, "13"), "the script worked out the total: '%s'", f.value);
		CHECK(!folio_form_take_alert(d, buf, sizeof buf), "no alert");
	}
	(void)total;
	folio_close(d);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/data";
	folio_ctx *fc = folio_new(64 << 20);
	if (!fc)
		return 1;
	printf("Content\n");
	test_content(fc, dir);
	printf("Fields\n");
	test_fields(fc, dir);
	printf("JavaScript\n");
	test_js(fc, dir);
	folio_drop(fc);
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
