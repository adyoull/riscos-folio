/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio's dictionary lookups (DESIGN §5), without any networking: the URLs to
 * try, reading an HTTP response, and turning the two sources' JSON into the
 * HTML the pop-up shows. Fetching is the front end's job (URL_Fetcher). */
#ifndef FOLIO_DICT_H
#define FOLIO_DICT_H

#include <stddef.h>

typedef enum {
	FOLIO_DICT_WIKTIONARY,      /* en.wiktionary.org REST: HTML fragments, any language */
	FOLIO_DICT_FREEDICT         /* api.dictionaryapi.dev: English only */
} folio_dict_source;

/* The word to look up from selected text: punctuation and quotes trimmed
 * from both ends, a curly apostrophe inside made straight. Returns 0, or -1
 * if what's left isn't a single word. */
int folio_dict_clean_word(const char *text, char *word, size_t size);

/* Lookup number 'attempt' (0, 1, 2...) for word: the URL and its source.
 * The order is Wiktionary with the word in lower case, Wiktionary as
 * written (if different), then the Free Dictionary. Returns -1 when there
 * are no more to try. */
int folio_dict_url(const char *word, int attempt, char *url, size_t size,
		folio_dict_source *source);

/* The page a reader can open for more: Wiktionary's entry for word. */
void folio_dict_page_url(const char *word, char *url, size_t size);

/* Splits an HTTP response ("HTTP/1.0 200 OK", headers, blank line, body).
 * A response without a status line is taken as a body alone (status 0).
 * location gets the Location header, if any. Returns 0, or -1 if the
 * headers are incomplete. */
int folio_http_parse(const char *resp, size_t len, int *status,
		const char **body, size_t *body_len, char *location, size_t loc_size);

/* The pop-up's HTML for a response body from source, or NULL if it holds no
 * definitions. lang is the book's language ("fr", "en-GB", "" if unknown);
 * Wiktionary's section for it is shown first. malloc'd. */
char *folio_dict_html(folio_dict_source source, const char *word, const char *body,
		size_t len, const char *lang);

/* The pop-up's HTML for a message instead of definitions ("Looking up...",
 * "No definition found", a network error). malloc'd. */
char *folio_dict_message_html(const char *word, const char *message);

/* A stable hash of word and language, for the lookup cache. */
unsigned folio_dict_key(const char *word, const char *lang);

#endif
