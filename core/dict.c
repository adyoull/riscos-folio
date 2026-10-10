/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio's dictionary lookups: URLs, HTTP responses and the pop-up's HTML.
 *
 * Wiktionary's REST "definition" endpoint returns, per language code, a list
 * of entries (part of speech, language name, definitions as small HTML
 * fragments). Senses that have sub-senses carry them twice, once nested in
 * the parent's <ol> and once as entries of their own, so each fragment is
 * cut at its first list. The Free Dictionary API returns plain text.
 * Both are Wiktionary data under CC BY-SA, which the pop-up says, with a
 * link to the entry. */
#include "dict.h"
#include "cJSON.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 4        /* parts of speech shown */
#define MAX_DEFS 5           /* definitions per part of speech */

/* ---- A growing string --------------------------------------------------- */

typedef struct {
	char *s;
	size_t len, cap;
	int failed;
} sbuf;

static void sb_add(sbuf *b, const char *s, size_t n)
{
	if (b->failed)
		return;
	if (b->len + n + 1 > b->cap) {
		size_t cap = b->cap ? b->cap * 2 : 1024;
		char *p;
		while (cap < b->len + n + 1)
			cap *= 2;
		p = realloc(b->s, cap);
		if (!p) {
			b->failed = 1;
			return;
		}
		b->s = p;
		b->cap = cap;
	}
	memcpy(b->s + b->len, s, n);
	b->len += n;
	b->s[b->len] = 0;
}

static void sb_str(sbuf *b, const char *s) { sb_add(b, s, strlen(s)); }

/* Text with &, <, > and " escaped. */
static void sb_text(sbuf *b, const char *s)
{
	for (; *s; s++) {
		switch (*s) {
		case '&': sb_str(b, "&amp;"); break;
		case '<': sb_str(b, "&lt;"); break;
		case '>': sb_str(b, "&gt;"); break;
		case '"': sb_str(b, "&quot;"); break;
		default: sb_add(b, s, 1);
		}
	}
}

static char *sb_done(sbuf *b)
{
	if (b->failed) {
		free(b->s);
		return NULL;
	}
	return b->s;
}

/* ---- Words and URLs ----------------------------------------------------- */

/* Lower case for ASCII and the Latin-1 letters (UTF-8 C3 80-9E). */
static void lower(const char *in, char *out, size_t size)
{
	size_t i = 0;
	const unsigned char *p = (const unsigned char *)in;
	while (*p && i + 1 < size) {
		if (p[0] == 0xc3 && p[1] >= 0x80 && p[1] <= 0x9e && p[1] != 0x97 && i + 2 < size) {
			out[i++] = (char)0xc3;
			out[i++] = (char)(p[1] + 0x20);
			p += 2;
		} else {
			out[i++] = (char)tolower(*p);
			p++;
		}
	}
	out[i] = 0;
}

static void url_encode(const char *in, char *out, size_t size)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t i = 0;
	const unsigned char *p;
	for (p = (const unsigned char *)in; *p && i + 4 < size; p++) {
		if (isalnum(*p) || *p == '-' || *p == '.' || *p == '_' || *p == '~') {
			out[i++] = (char)*p;
		} else {
			out[i++] = '%';
			out[i++] = hex[*p >> 4];
			out[i++] = hex[*p & 15];
		}
	}
	out[i] = 0;
}

/* Bytes of the punctuation character at p (UTF-8), or 0 if it isn't one. */
static int punct_at(const unsigned char *p)
{
	/* U+2018-201F quotes, U+2013/2014 dashes, U+2026 ellipsis, U+00AB/BB guillemets,
	 * U+00BF/A1 inverted marks. */
	if (p[0] == 0xe2 && p[1] == 0x80 && ((p[2] >= 0x98 && p[2] <= 0x9f) ||
			p[2] == 0x93 || p[2] == 0x94 || p[2] == 0xa6))
		return 3;
	if (p[0] == 0xc2 && (p[1] == 0xab || p[1] == 0xbb || p[1] == 0xbf || p[1] == 0xa1))
		return 2;
	if (p[0] < 0x80 && p[0] && (ispunct(p[0]) || isspace(p[0])))
		return 1;
	return 0;
}

int folio_dict_clean_word(const char *text, char *word, size_t size)
{
	const unsigned char *p = (const unsigned char *)text, *end;
	size_t i = 0;
	int n;
	/* Leading punctuation. */
	while (*p && (n = punct_at(p)) > 0)
		p += n;
	/* Trailing: find where the last run of punctuation starts. */
	end = p;
	{
		const unsigned char *q = p;
		while (*q) {
			n = punct_at(q);
			if (n == 0) {
				q++;
				while (*q && (*q & 0xc0) == 0x80)
					q++;
				end = q;
			} else {
				q += n;
			}
		}
	}
	while (p < end) {
		if (p[0] == 0xe2 && p[1] == 0x80 && p[2] == 0x99) {   /* ’ inside: don’t */
			if (i + 1 >= size)
				return -1;
			word[i++] = '\'';
			p += 3;
			continue;
		}
		if (isspace(*p))
			return -1;
		if (i + 1 >= size)
			return -1;
		word[i++] = (char)*p++;
	}
	word[i] = 0;
	return i ? 0 : -1;
}

int folio_dict_url(const char *word, int attempt, char *url, size_t size,
		folio_dict_source *source)
{
	char low[128], enc[400];
	lower(word, low, sizeof low);
	/* Attempt 1 (as written) is skipped when it's the same as attempt 0. */
	if (attempt == 1 && !strcmp(low, word))
		attempt = 2;
	switch (attempt) {
	case 0:
	case 1:
		url_encode(attempt == 0 ? low : word, enc, sizeof enc);
		snprintf(url, size, "https://en.wiktionary.org/api/rest_v1/page/definition/%s", enc);
		*source = FOLIO_DICT_WIKTIONARY;
		return 0;
	case 2:
		url_encode(low, enc, sizeof enc);
		snprintf(url, size, "https://api.dictionaryapi.dev/api/v2/entries/en/%s", enc);
		*source = FOLIO_DICT_FREEDICT;
		return 0;
	}
	return -1;
}

void folio_dict_page_url(const char *word, char *url, size_t size)
{
	char enc[400];
	url_encode(word, enc, sizeof enc);
	snprintf(url, size, "https://en.wiktionary.org/wiki/%s", enc);
}

unsigned folio_dict_key(const char *word, const char *lang)
{
	unsigned h = 2166136261u;
	const unsigned char *p;
	for (p = (const unsigned char *)word; *p; p++)
		h = (h ^ *p) * 16777619u;
	h = (h ^ 0) * 16777619u;
	for (p = (const unsigned char *)lang; *p; p++)
		h = (h ^ *p) * 16777619u;
	return h;
}

/* ---- HTTP --------------------------------------------------------------- */

int folio_http_parse(const char *resp, size_t len, int *status,
		const char **body, size_t *body_len, char *location, size_t loc_size)
{
	size_t i, line;
	if (location && loc_size)
		location[0] = 0;
	*status = 0;
	if (len < 5 || memcmp(resp, "HTTP/", 5)) {
		*body = resp;
		*body_len = len;
		return 0;
	}
	/* Status line: HTTP/1.x NNN reason */
	for (i = 5; i < len && resp[i] != ' '; i++)
		;
	if (i + 4 > len)
		return -1;
	*status = atoi(resp + i + 1);
	/* Headers, one per line, up to an empty line. */
	for (line = 0; line < len && resp[line] != '\n'; line++)
		;
	line++;
	while (line < len) {
		size_t end = line, n;
		while (end < len && resp[end] != '\n')
			end++;
		if (end >= len)
			return -1;
		n = end - line;
		if (n > 0 && resp[end - 1] == '\r')
			n--;
		if (n == 0) {
			*body = resp + end + 1;
			*body_len = len - (end + 1);
			return 0;
		}
		if (location && n > 9 && !strncasecmp(resp + line, "Location:", 9)) {
			size_t s = line + 9, k = 0;
			while (s < line + n && (resp[s] == ' ' || resp[s] == '\t'))
				s++;
			while (s < line + n && k + 1 < loc_size)
				location[k++] = resp[s++];
			location[k] = 0;
		}
		line = end + 1;
	}
	return -1;
}

/* ---- HTML --------------------------------------------------------------- */

static const char css[] =
	"body{margin:0}"
	"h1{font-size:1.35em;font-weight:bold;margin:0 0 0.15em 0}"
	"p.ph{margin:0 0 0.2em 0;color:#555555}"
	"h2{font-size:1em;font-style:italic;font-weight:normal;margin:0.6em 0 0.15em 0}"
	"ol{margin:0 0 0 1.7em;padding:0}"
	"li{margin:0 0 0.25em 0}"
	"p.src{font-size:0.8em;color:#555555;margin:0.9em 0 0 0}"
	"a{color:#1a4f9c;text-decoration:underline}"
	"p.msg{margin:0.4em 0 0 0}";

static void head(sbuf *b, const char *word)
{
	sb_str(b, "<!DOCTYPE html><html><head><meta charset=\"utf-8\"><style>");
	sb_str(b, css);
	sb_str(b, "</style></head><body><h1>");
	sb_text(b, word);
	sb_str(b, "</h1>");
}

static void attribution(sbuf *b, const char *word, int via_freedict)
{
	char url[512];
	folio_dict_page_url(word, url, sizeof url);
	sb_str(b, "<p class=\"src\">From <a href=\"");
	sb_text(b, url);
	sb_str(b, "\">Wiktionary</a>");
	if (via_freedict)
		sb_str(b, " (through the Free Dictionary API)");
	sb_str(b, ", CC BY-SA.</p>");
}

/* Does the fragment have any text outside its tags? */
static int has_text(const char *s, size_t n)
{
	size_t i;
	int in_tag = 0;
	for (i = 0; i < n; i++) {
		if (s[i] == '<')
			in_tag = 1;
		else if (s[i] == '>')
			in_tag = 0;
		else if (!in_tag && !isspace((unsigned char)s[i]))
			return 1;
	}
	return 0;
}

/* A Wiktionary definition up to its first nested list. */
static void add_fragment(sbuf *b, const char *s)
{
	const char *cut = NULL, *lists[] = { "<ol", "<ul", "<dl" };
	size_t i, n;
	for (i = 0; i < 3; i++) {
		const char *p = strstr(s, lists[i]);
		if (p && (!cut || p < cut))
			cut = p;
	}
	n = cut ? (size_t)(cut - s) : strlen(s);
	/* Trailing white space and newlines left before the list. */
	while (n > 0 && isspace((unsigned char)s[n - 1]))
		n--;
	/* Copy it without its links (<a>, </a>, <link>): their text stays, but
	 * only the attribution is a link in the pop-up. */
	for (i = 0; i < n; ) {
		if (s[i] == '<' && (i + 2 < n) &&
				((s[i + 1] == 'a' && (s[i + 2] == ' ' || s[i + 2] == '>')) ||
				 (s[i + 1] == '/' && s[i + 2] == 'a' && i + 3 < n && s[i + 3] == '>') ||
				 !strncmp(s + i, "<link", 5))) {
			while (i < n && s[i] != '>')
				i++;
			i++;
		} else {
			size_t j = i + 1;
			while (j < n && s[j] != '<')
				j++;
			sb_add(b, s + i, j - i);
			i = j;
		}
	}
}

static const char *json_str(const cJSON *o, const char *key)
{
	const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
	return cJSON_IsString(v) && v->valuestring ? v->valuestring : NULL;
}

/* The language section to show: the book's, else English, else the first. */
static const cJSON *wikt_section(const cJSON *root, const char *lang)
{
	char want[16];
	size_t i;
	const cJSON *s;
	for (i = 0; lang[i] && lang[i] != '-' && lang[i] != '_' && i + 1 < sizeof want; i++)
		want[i] = (char)tolower((unsigned char)lang[i]);
	want[i] = 0;
	if (want[0] && (s = cJSON_GetObjectItemCaseSensitive(root, want)) && cJSON_IsArray(s))
		return s;
	if ((s = cJSON_GetObjectItemCaseSensitive(root, "en")) && cJSON_IsArray(s))
		return s;
	for (s = root->child; s; s = s->next)
		if (cJSON_IsArray(s))
			return s;
	return NULL;
}

static int wikt_entry_has_defs(const cJSON *e)
{
	const cJSON *d, *defs = cJSON_GetObjectItemCaseSensitive(e, "definitions");
	cJSON_ArrayForEach(d, defs) {
		const char *h = json_str(d, "definition");
		if (h && has_text(h, strlen(h)))
			return 1;
	}
	return 0;
}

static int wiktionary(sbuf *b, const cJSON *root, const char *word, const char *lang)
{
	const cJSON *sec = wikt_section(root, lang), *e;
	int entries = 0, others = 0;
	if (!sec)
		return 0;
	/* Translingual entries (symbols, codes) only when there's nothing else. */
	cJSON_ArrayForEach(e, sec) {
		const char *l = json_str(e, "language");
		if (!(l && !strcmp(l, "Translingual")) && wikt_entry_has_defs(e))
			others++;
	}
	cJSON_ArrayForEach(e, sec) {
		const char *pos = json_str(e, "partOfSpeech");
		const char *l = json_str(e, "language");
		const cJSON *d, *defs = cJSON_GetObjectItemCaseSensitive(e, "definitions");
		int n = 0;
		if (entries >= MAX_ENTRIES)
			break;
		if (others && l && !strcmp(l, "Translingual"))
			continue;
		if (!wikt_entry_has_defs(e))
			continue;
		sb_str(b, "<h2>");
		sb_text(b, pos ? pos : "");
		if (l && strcmp(l, "English")) {
			sb_str(b, pos ? " \xc2\xb7 " : "");
			sb_text(b, l);
		}
		sb_str(b, "</h2><ol>");
		cJSON_ArrayForEach(d, defs) {
			const char *h = json_str(d, "definition");
			if (!h || !has_text(h, strlen(h)))
				continue;
			if (n++ >= MAX_DEFS)
				break;
			sb_str(b, "<li>");
			add_fragment(b, h);
			sb_str(b, "</li>");
		}
		sb_str(b, "</ol>");
		entries++;
	}
	return entries;
}

static int freedict(sbuf *b, const cJSON *root)
{
	const cJSON *e, *m;
	const char *phonetic = NULL;
	int entries = 0;
	if (!cJSON_IsArray(root))
		return 0;
	cJSON_ArrayForEach(e, root) {
		if (!phonetic)
			phonetic = json_str(e, "phonetic");
	}
	if (phonetic && *phonetic) {
		sb_str(b, "<p class=\"ph\">");
		sb_text(b, phonetic);
		sb_str(b, "</p>");
	}
	cJSON_ArrayForEach(e, root) {
		const cJSON *meanings = cJSON_GetObjectItemCaseSensitive(e, "meanings");
		cJSON_ArrayForEach(m, meanings) {
			const char *pos = json_str(m, "partOfSpeech");
			const cJSON *d, *defs = cJSON_GetObjectItemCaseSensitive(m, "definitions");
			int n = 0;
			if (entries >= MAX_ENTRIES)
				return entries;
			sb_str(b, "<h2>");
			sb_text(b, pos ? pos : "");
			sb_str(b, "</h2><ol>");
			cJSON_ArrayForEach(d, defs) {
				const char *t = json_str(d, "definition");
				if (!t || !*t)
					continue;
				if (n++ >= MAX_DEFS)
					break;
				sb_str(b, "<li>");
				sb_text(b, t);
				sb_str(b, "</li>");
			}
			sb_str(b, "</ol>");
			if (n)
				entries++;
		}
	}
	return entries;
}

char *folio_dict_html(folio_dict_source source, const char *word, const char *body,
		size_t len, const char *lang)
{
	sbuf b = { 0 };
	cJSON *root = cJSON_ParseWithLength(body, len);
	int n;
	if (!root)
		return NULL;
	head(&b, word);
	if (source == FOLIO_DICT_WIKTIONARY)
		n = wiktionary(&b, root, word, lang ? lang : "");
	else
		n = freedict(&b, root);
	cJSON_Delete(root);
	if (n == 0) {
		free(b.s);
		return NULL;
	}
	attribution(&b, word, source == FOLIO_DICT_FREEDICT);
	sb_str(&b, "</body></html>");
	return sb_done(&b);
}

char *folio_dict_message_html(const char *word, const char *message)
{
	sbuf b = { 0 };
	head(&b, word);
	sb_str(&b, "<p class=\"msg\">");
	sb_text(&b, message);
	sb_str(&b, "</p></body></html>");
	return sb_done(&b);
}
