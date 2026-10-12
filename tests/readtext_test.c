/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Reading-aloud text (wimp/readtext.c): sentence ends, carrying an
 * unfinished sentence to the next page, and the boxes shown for a sentence.
 *   readtext_test */
#include "readtext.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

/* A page of text: each character a box 6 pt wide on 12 pt lines; '\n' starts
 * a new line (and isn't a character); '|' is an added space (no character). */
typedef struct {
	folio_page_text t;
	folio_quad q[512];
} fake_page;

static void make_page(fake_page *p, const char *src)
{
	int i, n = (int)strlen(src), x = 0, y = 0;
	p->t.text = malloc(n + 1);
	p->t.byte_char = malloc((n + 1) * sizeof(int));
	p->t.quads = p->q;
	p->t.nchars = 0;
	for (i = 0; i < n; i++) {
		char c = src[i];
		if (c == '\n') {
			p->t.text[i] = '\n';
			p->t.byte_char[i] = -1;
			y += 12;
			x = 0;
			continue;
		}
		if (c == '|') {
			p->t.text[i] = ' ';
			p->t.byte_char[i] = -1;
			continue;
		}
		p->t.text[i] = c;
		p->t.byte_char[i] = p->t.nchars;
		p->q[p->t.nchars].ul.x = p->q[p->t.nchars].ll.x = (float)x;
		p->q[p->t.nchars].ur.x = p->q[p->t.nchars].lr.x = (float)x + 6;
		p->q[p->t.nchars].ul.y = p->q[p->t.nchars].ur.y = (float)y;
		p->q[p->t.nchars].ll.y = p->q[p->t.nchars].lr.y = (float)y + 10;
		p->t.nchars++;
		x += 6;
	}
	p->t.text[n] = 0;
	p->t.len = n;
}

static fake_page pages[2];

static const folio_quad *quads_of(void *ctx, int page, int *n)
{
	(void)ctx;
	*n = pages[page].t.nchars;
	return pages[page].q;
}

int main(void)
{
	rt_chunk c, rest;
	int b, pg[16], n;
	folio_quad box[16];
	const char *s;

	/* Sentence ends. */
	s = "One. Two three";
	CHECK(rt_unfinished(s, (int)strlen(s)) == 5, "unfinished 1: %d", rt_unfinished(s, (int)strlen(s)));
	s = "One. Two three.";
	CHECK(rt_unfinished(s, (int)strlen(s)) == (int)strlen(s), "finished");
	s = "He said \"Stop.\" And then";
	CHECK(rt_unfinished(s, (int)strlen(s)) == 16, "after a quote: %d", rt_unfinished(s, (int)strlen(s)));
	s = "Heading\n\nText goes";
	CHECK(rt_unfinished(s, (int)strlen(s)) == 9, "after a paragraph: %d", rt_unfinished(s, (int)strlen(s)));
	s = "Text ends.\n\n";
	CHECK(rt_unfinished(s, (int)strlen(s)) == (int)strlen(s), "ends with a paragraph");
	s = "no end at all";
	CHECK(rt_unfinished(s, (int)strlen(s)) == 0, "all unfinished");
	s = "First one. Second one is here. Third";
	CHECK(rt_sentence_start(s, 20) == 11, "sentence start: %d", rt_sentence_start(s, 20));
	CHECK(rt_sentence_start(s, 3) == 0, "first sentence");
	CHECK(rt_skip_space("  x", 3, 0) == 2, "skip space");

	/* Two pages; the second sentence runs over the page break. */
	make_page(&pages[0], "A cat sat.|The dog\nran far|");
	make_page(&pages[1], "away today.|Next.");
	memset(&c, 0, sizeof c);
	CHECK(rt_append(&c, &pages[0].t, 0, 0, pages[0].t.len) == 0, "append");
	CHECK(!strcmp(c.text, "A cat sat. The dog\nran far "), "chunk text '%s'", c.text);
	b = rt_unfinished(c.text, c.len);
	CHECK(b == 11, "carry from %d", b);
	CHECK(rt_split(&c, b, &rest) == 0 && !strcmp(c.text, "A cat sat. ") &&
		!strcmp(rest.text, "The dog\nran far "), "split '%s' | '%s'", c.text, rest.text);
	CHECK(rt_append(&rest, &pages[1].t, 1, 0, pages[1].t.len) == 0 &&
		!strcmp(rest.text, "The dog\nran far away today. Next."), "next chunk '%s'", rest.text);
	/* "The dog ran far away today." covers three lines on two pages. */
	n = rt_boxes(&rest, 0, 27, quads_of, NULL, pg, box, 16);
	CHECK(n == 3, "%d boxes", n);
	if (n == 3) {
		CHECK(pg[0] == 0 && box[0].ul.x == 60 && box[0].ur.x == 102 && box[0].ul.y == 0,
			"box 1: page %d %.0f-%.0f y %.0f", pg[0], box[0].ul.x, box[0].ur.x, box[0].ul.y);
		CHECK(pg[1] == 0 && box[1].ul.x == 0 && box[1].ur.x == 42 && box[1].ul.y == 12,
			"box 2: page %d %.0f-%.0f y %.0f", pg[1], box[1].ul.x, box[1].ur.x, box[1].ul.y);
		CHECK(pg[2] == 1 && box[2].ul.x == 0 && box[2].ur.x == 66,
			"box 3: page %d %.0f-%.0f", pg[2], box[2].ul.x, box[2].ur.x);
	}
	CHECK(rt_byte_of(&rest, 1, 0) == 16, "byte of page 2's first character: %d", rt_byte_of(&rest, 1, 0));
	CHECK(rt_boxes(&rest, 0, 27, quads_of, NULL, pg, box, 2) == 2, "max boxes");
	rt_free(&c);
	rt_free(&rest);
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
