/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Folio dictionary tests: words, URLs, HTTP responses, the pop-up's HTML
 * from trimmed copies of real responses (tests/dict, taken 2026-10-10), and
 * the HTML laid out and drawn by the core. Run under qemu by `make test`:
 *   dict_test tests/dict */
#include "folio.h"
#include "dict.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static char *slurp(const char *dir, const char *leaf, size_t *len)
{
	char path[512];
	FILE *f;
	char *s;
	long n;
	snprintf(path, sizeof path, "%s/%s", dir, leaf);
	f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	n = ftell(f);
	fseek(f, 0, SEEK_SET);
	s = malloc((size_t)n + 1);
	if (fread(s, 1, (size_t)n, f) != (size_t)n)
		n = 0;
	s[n] = 0;
	fclose(f);
	*len = (size_t)n;
	return s;
}

static int count(const char *s, const char *needle)
{
	int n = 0;
	while ((s = strstr(s, needle)) != NULL) {
		n++;
		s += strlen(needle);
	}
	return n;
}

static void test_words(void)
{
	char w[64];
	CHECK(folio_dict_clean_word("\xe2\x80\x9c" "Fox,\xe2\x80\x9d", w, sizeof w) == 0 && !strcmp(w, "Fox"), "quotes: '%s'", w);
	CHECK(folio_dict_clean_word("don\xe2\x80\x99t.", w, sizeof w) == 0 && !strcmp(w, "don't"), "apostrophe: '%s'", w);
	CHECK(folio_dict_clean_word("  well-known; ", w, sizeof w) == 0 && !strcmp(w, "well-known"), "hyphen: '%s'", w);
	CHECK(folio_dict_clean_word("na\xc3\xafve", w, sizeof w) == 0 && !strcmp(w, "na\xc3\xafve"), "accent: '%s'", w);
	CHECK(folio_dict_clean_word("two words", w, sizeof w) == -1, "two words accepted");
	CHECK(folio_dict_clean_word("...", w, sizeof w) == -1, "punctuation accepted");
	CHECK(folio_dict_clean_word("", w, sizeof w) == -1, "empty accepted");
	CHECK(folio_dict_clean_word("supercalifragilistic", w, 8) == -1, "overlong accepted");
}

static void test_urls(void)
{
	char url[512];
	folio_dict_source src;
	CHECK(folio_dict_url("Fox", 0, url, sizeof url, &src) == 0 && src == FOLIO_DICT_WIKTIONARY &&
		!strcmp(url, "https://en.wiktionary.org/api/rest_v1/page/definition/fox"), "Fox 0: %s", url);
	CHECK(folio_dict_url("Fox", 1, url, sizeof url, &src) == 0 && src == FOLIO_DICT_WIKTIONARY &&
		!strcmp(url, "https://en.wiktionary.org/api/rest_v1/page/definition/Fox"), "Fox 1: %s", url);
	CHECK(folio_dict_url("Fox", 2, url, sizeof url, &src) == 0 && src == FOLIO_DICT_FREEDICT &&
		!strcmp(url, "https://api.dictionaryapi.dev/api/v2/entries/en/fox"), "Fox 2: %s", url);
	CHECK(folio_dict_url("Fox", 3, url, sizeof url, &src) == -1, "Fox 3 exists");
	/* Already lower case: the as-written try is skipped. */
	CHECK(folio_dict_url("fox", 1, url, sizeof url, &src) == 0 && src == FOLIO_DICT_FREEDICT, "fox 1: %s", url);
	CHECK(folio_dict_url("\xc3\x89" "clair", 0, url, sizeof url, &src) == 0 &&
		!strcmp(url, "https://en.wiktionary.org/api/rest_v1/page/definition/%C3%A9clair"), "accent: %s", url);
	CHECK(folio_dict_url("don't", 0, url, sizeof url, &src) == 0 &&
		!strcmp(url, "https://en.wiktionary.org/api/rest_v1/page/definition/don%27t"), "apostrophe: %s", url);
	folio_dict_page_url("fox", url, sizeof url);
	CHECK(!strcmp(url, "https://en.wiktionary.org/wiki/fox"), "page: %s", url);
	CHECK(folio_dict_key("fox", "en") != folio_dict_key("fox", "fr"), "key ignores language");
	CHECK(folio_dict_key("fox", "en") == folio_dict_key("fox", "en"), "key unstable");
}

static void test_http(void)
{
	static const char ok[] = "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\n\r\n{\"a\":1}";
	static const char moved[] = "HTTP/1.0 301 Moved Permanently\nlocation:  https://x.org/y\n\nmoved";
	static const char part[] = "HTTP/1.0 200 OK\r\nContent-Type: appl";
	const char *body;
	size_t n;
	int status;
	char loc[64];
	CHECK(folio_http_parse(ok, strlen(ok), &status, &body, &n, loc, sizeof loc) == 0 &&
		status == 200 && n == 7 && !memcmp(body, "{\"a\":1}", 7) && !loc[0], "200: %d %d", status, (int)n);
	CHECK(folio_http_parse(moved, strlen(moved), &status, &body, &n, loc, sizeof loc) == 0 &&
		status == 301 && !strcmp(loc, "https://x.org/y") && n == 5, "301: %d '%s'", status, loc);
	CHECK(folio_http_parse(part, strlen(part), &status, &body, &n, loc, sizeof loc) == -1, "partial accepted");
	CHECK(folio_http_parse("[1]", 3, &status, &body, &n, loc, sizeof loc) == 0 && status == 0 && n == 3,
		"body only");
}

static char *test_html(const char *dir)
{
	size_t len;
	char *body, *html, *fox;

	body = slurp(dir, "wikt-fox.json", &len);
	CHECK(body != NULL, "no wikt-fox.json");
	if (!body)
		return NULL;
	fox = folio_dict_html(FOLIO_DICT_WIKTIONARY, "fox", body, len, "en-GB");
	CHECK(fox != NULL, "no html for fox");
	if (fox) {
		CHECK(strstr(fox, "<h1>fox</h1>") != NULL, "heading");
		CHECK(strstr(fox, "<h2>Noun</h2>") && strstr(fox, "<h2>Verb</h2>"), "parts of speech");
		CHECK(!strstr(fox, "Formosan"), "translingual shown");
		CHECK(!strstr(fox, "senseid") && !strstr(fox, "The red fox.</li>"), "nested senses kept");
		CHECK(count(fox, "<li>") == 6, "%d definitions (5 nouns + 1 verb)", count(fox, "<li>"));
		CHECK(!strstr(fox, "Seventh"), "more than 5 definitions");
		CHECK(strstr(fox, "href=\"https://en.wiktionary.org/wiki/fox\">Wiktionary</a>") != NULL, "attribution");
		CHECK(!strstr(fox, "Free Dictionary"), "wrong source named");
	}
	html = folio_dict_html(FOLIO_DICT_WIKTIONARY, "fox", body, len, "fr");
	CHECK(html && strstr(html, "fran\xc3\xa7" "ais") && strstr(html, "Noun \xc2\xb7 French") &&
		!strstr(html, "canine"), "French section");
	free(html);
	html = folio_dict_html(FOLIO_DICT_WIKTIONARY, "fox", body, len, "de");
	CHECK(html && strstr(html, "canine"), "unknown language: not English");
	free(html);
	free(body);

	body = slurp(dir, "wikt-404.json", &len);
	html = body ? folio_dict_html(FOLIO_DICT_WIKTIONARY, "qwzxv", body, len, "") : NULL;
	CHECK(body && !html, "404 gave definitions");
	free(html);
	free(body);

	body = slurp(dir, "freedict-running.json", &len);
	html = body ? folio_dict_html(FOLIO_DICT_FREEDICT, "running", body, len, "") : NULL;
	CHECK(html && strstr(html, "<li>To move swiftly.</li>") && strstr(html, "<h2>noun</h2>"),
		"free dictionary");
	CHECK(html && strstr(html, "&lt;to run&gt;"), "text not escaped");
	CHECK(html && strstr(html, "class=\"ph\">/\xcb\x88"), "phonetic");
	CHECK(html && strstr(html, "through the Free Dictionary API"), "attribution");
	free(html);
	free(body);

	body = slurp(dir, "freedict-404.json", &len);
	html = body ? folio_dict_html(FOLIO_DICT_FREEDICT, "qwzxv", body, len, "") : NULL;
	CHECK(body && !html, "free dictionary 404 gave definitions");
	free(html);
	free(body);

	html = folio_dict_message_html("a<b", "Looking it up\xe2\x80\xa6");
	CHECK(html && strstr(html, "<h1>a&lt;b</h1>") && strstr(html, "Looking it up"), "message");
	free(html);

	html = folio_dict_html(FOLIO_DICT_WIKTIONARY, "x", "not json", 8, "");
	CHECK(!html, "bad JSON gave definitions");
	return fox;
}

static void write_ppm(const char *name, const uint32_t *px, int w, int h)
{
	char p[256];
	FILE *f;
	int i;
	mkdir("tests/out", 0777);
	snprintf(p, sizeof p, "tests/out/%s.ppm", name);
	f = fopen(p, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", w, h);
	for (i = 0; i < w * h; i++)
		fwrite(px + i, 1, 3, f);
	fclose(f);
}

/* The pop-up as the front end draws it. */
static void test_render(const char *html)
{
	folio_ctx *fc = folio_new(32 << 20);
	folio_doc *d;
	folio_loc loc;
	float h = 0, zoom = 1.25f;
	const float width = 260;
	int pw, ph, dark_black = 0, i;
	uint32_t *px;
	folio_irect tile;
	char *text;
	folio_link link;

	CHECK(fc != NULL, "no context");
	if (!fc || !html)
		return;
	d = folio_open_html(fc, html, width, 11);
	CHECK(d != NULL, "open html: %s", folio_error(fc));
	if (!d) {
		folio_drop(fc);
		return;
	}
	CHECK(folio_loc_from_number(d, 0, &loc) == 0, "no page 0");
	CHECK(folio_content_height(d, loc, &h) == 0 && h > 100 && h < 900, "content height %g", h);
	printf("  pop-up content: %g x %g pt\n", width, h);

	pw = (int)(width * zoom + 0.5f);
	ph = (int)((h + 4) * zoom + 0.5f);
	px = malloc((size_t)pw * ph * 4);
	tile.x0 = 0; tile.y0 = 0; tile.x1 = pw; tile.y1 = ph;
	CHECK(folio_render_tile(d, loc, zoom, tile, FOLIO_PAGE_NORMAL, 0, px, pw * 4) == 0, "render");
	write_ppm("dict-fox", px, pw, ph);
	CHECK(folio_render_tile(d, loc, zoom, tile, FOLIO_PAGE_DARK, 0, px, pw * 4) == 0, "render dark");
	for (i = 0; i < pw * ph; i++)
		if ((px[i] & 0xffffff) == 0)
			dark_black++;
	CHECK(dark_black > pw * ph / 2, "dark pop-up isn't mostly black (%d of %d)", dark_black, pw * ph);
	write_ppm("dict-fox-dark", px, pw, ph);
	free(px);

	{
		folio_point a = { 0, 0 }, b = { width, h };
		text = folio_selection_text(d, loc, a, b);
		CHECK(text && strstr(text, "canine") && strstr(text, "CC BY-SA"), "text: %.60s", text ? text : "(none)");
		folio_free(d, text);
	}
	/* The link on "Wiktionary" in the last line. */
	{
		folio_point p;
		int found = 0;
		float x;
		p.y = h - 4;
		for (x = 2; x < width && !found; x += 4) {
			p.x = x;
			found = folio_link_at(d, loc, p, &link);
		}
		CHECK(found && link.uri && !strcmp(link.uri, "https://en.wiktionary.org/wiki/fox"),
			"attribution link: %s", found && link.uri ? link.uri : "(none)");
	}
	folio_close(d);
	folio_drop(fc);
}

int main(int argc, char **argv)
{
	const char *dir = argc > 1 ? argv[1] : "tests/dict";
	char *fox;
	test_words();
	test_urls();
	test_http();
	fox = test_html(dir);
	test_render(fox);
	free(fox);
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
