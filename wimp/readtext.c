/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Text for reading aloud (see readtext.h). */
#include "readtext.h"
#include <stdlib.h>
#include <string.h>

void rt_free(rt_chunk *c)
{
	free(c->text);
	free(c->page);
	free(c->chr);
	memset(c, 0, sizeof *c);
}

static int grow(rt_chunk *c, int more)
{
	int need = c->len + more + 1;
	char *t = realloc(c->text, need);
	int *p, *ch;
	if (!t)
		return -1;
	c->text = t;
	p = realloc(c->page, need * sizeof *p);
	if (!p)
		return -1;
	c->page = p;
	ch = realloc(c->chr, need * sizeof *ch);
	if (!ch)
		return -1;
	c->chr = ch;
	return 0;
}

int rt_append(rt_chunk *c, const folio_page_text *t, int page, int from, int to)
{
	int i;
	if (from < 0) from = 0;
	if (to > t->len) to = t->len;
	if (to < from) to = from;
	if (grow(c, to - from))
		return -1;
	for (i = from; i < to; i++) {
		c->text[c->len] = t->text[i];
		c->chr[c->len] = t->byte_char[i];
		c->page[c->len] = t->byte_char[i] >= 0 ? page : -1;
		c->len++;
	}
	c->text[c->len] = 0;
	return 0;
}

int rt_split(rt_chunk *c, int from, rt_chunk *rest)
{
	int n;
	memset(rest, 0, sizeof *rest);
	if (from < 0) from = 0;
	if (from > c->len) from = c->len;
	n = c->len - from;
	if (grow(rest, n))
		return -1;
	memcpy(rest->text, c->text + from, n);
	memcpy(rest->page, c->page + from, n * sizeof *rest->page);
	memcpy(rest->chr, c->chr + from, n * sizeof *rest->chr);
	rest->len = n;
	rest->text[n] = 0;
	c->len = from;
	if (c->text)
		c->text[from] = 0;
	return 0;
}

static int is_space(char c)
{
	return c == ' ' || c == '\n' || c == '\t' || c == '\r';
}

/* Does a sentence end just before byte 'at' (which is space, or the end)?
 * Returns the end's position (at), or -1. */
static int ends_sentence(const char *s, int at)
{
	int i = at - 1;
	/* Back over closing quotes and brackets. */
	for (;;) {
		if (i >= 0 && (s[i] == '"' || s[i] == '\'' || s[i] == ')' || s[i] == ']')) {
			i--;
			continue;
		}
		if (i >= 2 && (unsigned char)s[i - 2] == 0xe2 && (unsigned char)s[i - 1] == 0x80 &&
				((unsigned char)s[i] == 0x99 || (unsigned char)s[i] == 0x9d)) {
			i -= 3;
			continue;
		}
		break;
	}
	return i >= 0 && (s[i] == '.' || s[i] == '!' || s[i] == '?') ? at : -1;
}

int rt_unfinished(const char *s, int len)
{
	int i, last_end = 0, text_after = 0;
	for (i = 0; i < len; i++) {
		if (s[i] == '\n' && i + 1 < len && s[i + 1] == '\n') {
			last_end = i + 2;
			text_after = 0;
			i++;
			continue;
		}
		if (is_space(s[i])) {
			if (ends_sentence(s, i) >= 0) {
				last_end = i + 1;
				text_after = 0;
			}
			continue;
		}
		text_after = 1;
	}
	if (!text_after)
		return len;
	/* The text ends with a sentence's end and no space after it. */
	if (ends_sentence(s, len) >= 0)
		return len;
	return last_end;
}

int rt_sentence_start(const char *s, int at)
{
	int i;
	for (i = at; i > 0; i--) {
		if (i >= 2 && s[i - 1] == '\n' && s[i - 2] == '\n')
			break;
		if (is_space(s[i - 1]) && ends_sentence(s, i - 1) >= 0)
			break;
	}
	return i;
}

int rt_skip_space(const char *s, int len, int at)
{
	while (at < len && is_space(s[at]))
		at++;
	return at;
}

int rt_byte_of(const rt_chunk *c, int page, int ch)
{
	int i;
	for (i = 0; i < c->len; i++)
		if (c->page[i] == page && c->chr[i] >= ch)
			return i;
	return -1;
}

int rt_boxes(const rt_chunk *c, int start, int end, rt_quads_fn quads, void *ctx,
		int *pages, folio_quad *boxes, int max)
{
	int i, n = 0, last_page = -1, last_ch = -1, nq = 0;
	const folio_quad *q = NULL;
	if (start < 0) start = 0;
	if (end > c->len) end = c->len;
	for (i = start; i < end; i++) {
		int p = c->page[i], ch = c->chr[i];
		folio_quad cq;
		float x0, x1, y0, y1;
		if (p < 0 || ch < 0 || (ch == last_ch && p == last_page))
			continue;
		if (p != last_page) {
			q = quads(ctx, p, &nq);
			last_page = p;
			last_ch = -1;
		}
		if (!q || ch >= nq)
			continue;
		cq = q[ch];
		x0 = cq.ul.x < cq.ll.x ? cq.ul.x : cq.ll.x;
		x1 = cq.ur.x > cq.lr.x ? cq.ur.x : cq.lr.x;
		y0 = cq.ul.y < cq.ur.y ? cq.ul.y : cq.ur.y;
		y1 = cq.ll.y > cq.lr.y ? cq.ll.y : cq.lr.y;
		/* The same line as the last box (on this page, next character,
		 * overlapping vertically): extend it. */
		if (n > 0 && pages[n - 1] == p && ch == last_ch + 1) {
			folio_quad *b = &boxes[n - 1];
			float mid = (y0 + y1) / 2;
			if (mid > b->ul.y && mid < b->ll.y && x0 >= b->ul.x - 2) {
				if (x1 > b->ur.x)
					b->ur.x = b->lr.x = x1;
				last_ch = ch;
				continue;
			}
		}
		last_ch = ch;
		if (n == max)
			break;
		pages[n] = p;
		boxes[n].ul.x = boxes[n].ll.x = x0;
		boxes[n].ur.x = boxes[n].lr.x = x1;
		boxes[n].ul.y = boxes[n].ur.y = y0;
		boxes[n].ll.y = boxes[n].lr.y = y1;
		n++;
	}
	return n;
}
