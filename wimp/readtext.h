/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Text for reading aloud, a chunk at a time: a page's text (from
 * folio_get_page_text), plus any sentence the previous page left unfinished,
 * with each byte's page and character so a spoken sentence can be shown.
 * No RISC OS calls: tested on Linux (tests/readtext_test.c). */
#ifndef READTEXT_H
#define READTEXT_H

#include "folio.h"

typedef struct {
	char *text;            /* NUL-terminated UTF-8 */
	int len;
	int *page;             /* per byte: page number, or -1 (added space) */
	int *chr;              /* per byte: character on that page, or -1 */
} rt_chunk;

void rt_free(rt_chunk *c);
/* Appends text[from..to) of page 'page' (its byte_char map) to c. 0 or -1. */
int rt_append(rt_chunk *c, const folio_page_text *t, int page, int from, int to);
/* Moves c's bytes from 'from' on into a new chunk (the unfinished sentence
 * a page ends with, to go before the next page's text). */
int rt_split(rt_chunk *c, int from, rt_chunk *rest);

/* Where the last sentence starts if it's unfinished: after the last ". ",
 * "! ", "? " (closing quotes and brackets allowed) or paragraph break, if
 * there is non-space text after it; len if the text ends a sentence. */
int rt_unfinished(const char *text, int len);
/* The start of the sentence holding byte 'at' (back to the previous sentence
 * end or paragraph break). */
int rt_sentence_start(const char *text, int at);
/* The first byte at or after 'at' that isn't space. */
int rt_skip_space(const char *text, int len, int at);

/* The boxes covering bytes start..end of c: one per run of characters on a
 * line of one page. quads gives each page's character quads (from the
 * page's folio_page_text). Returns the number written (up to max). */
typedef const folio_quad *(*rt_quads_fn)(void *ctx, int page, int *nchars);
int rt_boxes(const rt_chunk *c, int start, int end, rt_quads_fn quads, void *ctx,
		int *pages, folio_quad *boxes, int max);

/* The byte holding character 'ch' of page 'page' (or the next one after it), or -1. */
int rt_byte_of(const rt_chunk *c, int page, int ch);

#endif
