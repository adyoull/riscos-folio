/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Folio annotation tests (core/annot.c), run under qemu by `make test`:
 *   annot_test tests/data
 * Uses doc.pdf page 2 ("Folio test page 2"; page 1 already has a highlight)
 * and book.epub (which can't be annotated). Writes tests/out/annot-*.pdf. */
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

static folio_point mid_left(folio_quad q) { folio_point p = { q.ul.x + 0.5f, (q.ul.y + q.ll.y) / 2 }; return p; }
static folio_point mid_right(folio_quad q) { folio_point p = { q.ur.x - 0.5f, (q.ur.y + q.lr.y) / 2 }; return p; }
static folio_point centre(folio_quad q) { folio_point p = { (q.ul.x + q.lr.x) / 2, (q.ul.y + q.lr.y) / 2 }; return p; }

/* The word's quad on the page. */
static int find_word(folio_doc *d, folio_loc loc, const char *w, folio_quad *q)
{
	int mark[4];
	return folio_find_on_page(d, loc, w, q, mark, 1) == 1 ? 0 : -1;
}

/* Average colour of a small box round p (pt, page space), drawn at 1 px/pt. */
static void colour_at(folio_doc *d, folio_loc loc, folio_point p, int *r, int *g, int *b)
{
	uint32_t px[16 * 6];
	folio_irect t = { (int)p.x - 8, (int)p.y - 3, (int)p.x + 8, (int)p.y + 3 };
	int i, sr = 0, sg = 0, sb = 0;
	*r = *g = *b = -1;
	if (folio_render_tile(d, loc, 1.0f, t, FOLIO_PAGE_NORMAL, 1, px, 16 * 4))
		return;
	for (i = 0; i < 16 * 6; i++) {
		const unsigned char *c = (const unsigned char *)&px[i];
		sr += c[0]; sg += c[1]; sb += c[2];
	}
	*r = sr / 96; *g = sg / 96; *b = sb / 96;
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/data";
	char src[512], out_inc[] = "tests/out/annot-inc.pdf", out_full[] = "tests/out/annot-full.pdf";
	folio_ctx *fc = folio_new(64 << 20);
	folio_doc *d, *e;
	folio_loc p2;
	folio_quad word, page_word;
	folio_annot_kind kind;
	folio_rect r;
	folio_point note_at = { 300, 400 };
	int n, red, green, blue, before_r = 0, before_b = 0;
	char *t;

	mkdir("tests/out", 0777);
	snprintf(src, sizeof src, "%s", data(dir, "doc.pdf"));
	d = folio_open(fc, src, NULL);
	CHECK(d != NULL, "open: %s", folio_error(fc));
	if (!d)
		return 1;
	folio_count_step(d, 100);
	CHECK(folio_can_annotate(d), "PDF can't be annotated");
	CHECK(!folio_is_modified(d) && !folio_can_undo(d), "modified before anything");
	CHECK(folio_loc_from_number(d, 1, &p2) == 0, "page 2");

	/* Highlight "test" on page 2. */
	CHECK(find_word(d, p2, "test", &word) == 0, "no 'test' on page 2");
	CHECK(folio_annot_at(d, p2, centre(word), &kind, &r) == -1, "an annotation there already");
	colour_at(d, p2, centre(word), &red, &green, &blue);
	CHECK(red > 60 && abs(red - blue) < 10 && abs(red - green) < 10, "not black on white: %d,%d,%d", red, green, blue);
	before_r = red;
	before_b = blue;
	CHECK(folio_annot_markup(d, p2, FOLIO_ANNOT_HIGHLIGHT, mid_left(word), mid_right(word)) == 0,
		"highlight: %s", folio_error(fc));
	CHECK(folio_is_modified(d) && folio_can_undo(d) && !folio_can_redo(d), "after highlight: state");
	n = folio_annot_at(d, p2, centre(word), &kind, &r);
	CHECK(n == 0 && kind == FOLIO_ANNOT_HIGHLIGHT, "highlight not found (%d, kind %d)", n, (int)kind);
	CHECK(r.x0 <= word.ul.x + 1 && r.x1 >= word.ur.x - 1, "highlight box %g..%g vs word %g..%g",
		r.x0, r.x1, word.ul.x, word.ur.x);
	/* Yellow multiplies the text's background: red stays, blue goes. */
	colour_at(d, p2, centre(word), &red, &green, &blue);
	CHECK(abs(red - before_r) < 20 && blue < before_b / 3, "not yellow under the word: %d,%d,%d", red, green, blue);
	/* Not on the next word ("page"): markup is hit on its quads only. */
	CHECK(find_word(d, p2, "page", &page_word) == 0, "no 'page'");
	CHECK(folio_annot_at(d, p2, centre(page_word), &kind, &r) == -1, "highlight hit beyond its quads");

	/* Underline and strike out "page"; the last added is on top. */
	CHECK(folio_annot_markup(d, p2, FOLIO_ANNOT_UNDERLINE, mid_left(page_word), mid_right(page_word)) == 0,
		"underline: %s", folio_error(fc));
	CHECK(folio_annot_markup(d, p2, FOLIO_ANNOT_STRIKEOUT, mid_left(page_word), mid_right(page_word)) == 0,
		"strike out: %s", folio_error(fc));
	n = folio_annot_at(d, p2, centre(page_word), &kind, NULL);
	CHECK(n == 2 && kind == FOLIO_ANNOT_STRIKEOUT, "top of page word: %d kind %d", n, (int)kind);

	/* The list: highlight (the word it marks), underline, strike-out. */
	{
		folio_annot_info info[8];
		int k = folio_annot_list(d, p2, info, 8);
		CHECK(k == 3, "listed %d", k);
		if (k == 3) {
			CHECK(info[0].n == 0 && info[0].kind == FOLIO_ANNOT_HIGHLIGHT && !strcmp(info[0].type, "Highlight"),
				"first: %d %s", info[0].n, info[0].type);
			CHECK(!strcmp(info[0].text, "test"), "marked text '%s'", info[0].text);
			CHECK(info[0].rgb == 0xffdb00, "yellow 0x%06x", info[0].rgb);
			CHECK(info[1].kind == FOLIO_ANNOT_UNDERLINE && info[1].rgb == 0x1a59d9, "underline blue 0x%06x", info[1].rgb);
			CHECK(info[2].n == 2 && info[2].kind == FOLIO_ANNOT_STRIKEOUT, "third: %s", info[2].type);
		}
		/* Recolour the highlight green, then undo. */
		CHECK(folio_annot_recolour(d, p2, 0, 0x33cc33) == 0, "recolour: %s", folio_error(fc));
		k = folio_annot_list(d, p2, info, 8);
		CHECK(k == 3 && info[0].rgb == 0x33cc33, "green now 0x%06x", k ? info[0].rgb : 0);
		colour_at(d, p2, centre(word), &red, &green, &blue);
		CHECK(green > red + 40, "drawn green: %d,%d,%d", red, green, blue);
		CHECK(folio_undo(d) == 0 && folio_annot_list(d, p2, info, 8) == 3 && info[0].rgb == 0xffdb00,
			"undo: yellow again 0x%06x", info[0].rgb);
		/* A chosen colour for new highlights. */
		folio_set_annot_colour(fc, FOLIO_ANNOT_HIGHLIGHT, 0xff88cc);
		CHECK(folio_annot_markup(d, p2, FOLIO_ANNOT_HIGHLIGHT, mid_left(word), mid_right(word)) == 0, "pink");
		k = folio_annot_list(d, p2, info, 8);
		CHECK(k == 4 && info[3].rgb == 0xff88cc, "new one pink 0x%06x", k == 4 ? info[3].rgb : 0);
		CHECK(folio_undo(d) == 0 && folio_annot_list(d, p2, info, 8) == 3, "undo the pink one");
		folio_set_annot_colour(fc, FOLIO_ANNOT_HIGHLIGHT, -1);
	}

	/* Nothing selected: refused, nothing changes. */
	{
		folio_point a = { 500, 800 }, b = { 501, 801 };
		CHECK(folio_annot_markup(d, p2, FOLIO_ANNOT_HIGHLIGHT, a, b) == -1, "markup of nothing accepted");
		CHECK(folio_annot_at(d, p2, a, NULL, NULL) == -1, "markup of nothing made one");
	}

	/* A note. */
	CHECK(folio_annot_note(d, p2, note_at, "First thought") == 0, "note: %s", folio_error(fc));
	{
		folio_point in = { note_at.x + 5, note_at.y + 5 };
		n = folio_annot_at(d, p2, in, &kind, &r);
		CHECK(n == 3 && kind == FOLIO_ANNOT_NOTE, "note not found: %d kind %d", n, (int)kind);
		t = folio_annot_text(d, p2, n);
		CHECK(t && !strcmp(t, "First thought"), "note text '%s'", t ? t : "(null)");
		folio_free(d, t);
		CHECK(folio_annot_set_text(d, p2, n, "Second thought") == 0, "set text");
		t = folio_annot_text(d, p2, n);
		CHECK(t && !strcmp(t, "Second thought"), "edited note '%s'", t ? t : "(null)");
		folio_free(d, t);

		/* Undo the edit, then the note; redo the note. */
		CHECK(folio_undo(d) == 0, "undo edit");
		t = folio_annot_text(d, p2, 3);
		CHECK(t && !strcmp(t, "First thought"), "undone edit '%s'", t ? t : "(null)");
		folio_free(d, t);
		CHECK(folio_undo(d) == 0, "undo note");
		CHECK(folio_annot_at(d, p2, in, NULL, NULL) == -1, "note still there after undo");
		CHECK(folio_can_redo(d), "no redo");
		CHECK(folio_redo(d) == 0, "redo note");
		CHECK(folio_annot_at(d, p2, in, &kind, NULL) == 3 && kind == FOLIO_ANNOT_NOTE, "note not back");
	}

	/* Delete the strike-out; the underline is on top again. */
	CHECK(folio_annot_delete(d, p2, 2) == 0, "delete: %s", folio_error(fc));
	n = folio_annot_at(d, p2, centre(page_word), &kind, NULL);
	CHECK(n == 1 && kind == FOLIO_ANNOT_UNDERLINE, "after delete: %d kind %d", n, (int)kind);

	/* Save incrementally and in full; both reopen with the annotations. */
	remove(out_inc);
	remove(out_full);
	CHECK(folio_save_pdf(d, src, out_inc) == 0, "save incremental: %s", folio_error(fc));
	CHECK(folio_save_pdf(d, NULL, out_full) == 0, "save full: %s", folio_error(fc));
	{
		const char *outs[2] = { out_inc, out_full };
		int k;
		for (k = 0; k < 2; k++) {
			folio_loc l;
			folio_point in = { note_at.x + 5, note_at.y + 5 };
			e = folio_open(fc, outs[k], NULL);
			CHECK(e != NULL, "reopen %s: %s", outs[k], folio_error(fc));
			if (!e)
				continue;
			folio_count_step(e, 100);
			folio_loc_from_number(e, 1, &l);
			CHECK(folio_annot_at(e, l, centre(word), &kind, NULL) >= 0 && kind == FOLIO_ANNOT_HIGHLIGHT,
				"%s: highlight lost", outs[k]);
			CHECK(folio_annot_at(e, l, centre(page_word), &kind, NULL) >= 0 && kind == FOLIO_ANNOT_UNDERLINE,
				"%s: underline lost", outs[k]);
			n = folio_annot_at(e, l, in, &kind, NULL);
			t = n >= 0 ? folio_annot_text(e, l, n) : NULL;
			CHECK(kind == FOLIO_ANNOT_NOTE && t && !strcmp(t, "First thought"), "%s: note '%s'", outs[k],
				t ? t : "(null)");
			folio_free(e, t);
			CHECK(!folio_is_modified(e), "%s: modified on opening", outs[k]);
			folio_close(e);
		}
	}
	{
		/* The incremental file starts with the original's bytes. */
		FILE *a = fopen(src, "rb"), *b = fopen(out_inc, "rb");
		long na, nb;
		int same = a && b;
		if (same) {
			int ca, cb;
			fseek(a, 0, SEEK_END);
			na = ftell(a);
			fseek(b, 0, SEEK_END);
			nb = ftell(b);
			rewind(a);
			rewind(b);
			same = nb > na;
			while (same && (ca = fgetc(a)) != EOF) {
				cb = fgetc(b);
				same = ca == cb;
			}
		}
		CHECK(same, "incremental save isn't the original plus more");
		if (a) fclose(a);
		if (b) fclose(b);
	}
	folio_close(d);

	/* EPUBs: not yet. */
	e = folio_open(fc, data(dir, "book.epub"), NULL);
	CHECK(e && !folio_can_annotate(e), "EPUB annotatable");
	if (e) {
		folio_loc l = { 0, 0 };
		folio_point a = { 10, 10 }, b = { 200, 200 };
		folio_count_step(e, 1);
		CHECK(folio_annot_markup(e, l, FOLIO_ANNOT_HIGHLIGHT, a, b) == -1, "EPUB markup accepted");
		CHECK(!folio_can_undo(e) && !folio_is_modified(e), "EPUB undo state");
		folio_close(e);
	}

	folio_drop(fc);
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
