/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Define "word" (DESIGN §5): a transient pop-up near the word with its
 * definitions, fetched with URL_Fetcher (PRM p684-697, p1376) in null polls,
 * so the desktop never waits.
 *
 * The pop-up's content is HTML (core/dict.c) laid out and drawn by MuPDF
 * into a sprite, so it follows the page's dark or sepia mode. A Copy action
 * button copies the definitions; a click on "Wiktionary" opens the entry in
 * the browser (URI_Dispatch). Lookups that found something are kept in
 * <Choices$Write>.Folio.Dict, so repeats are instant and work offline. */
#include "app.h"
#include "worker.h"
#include "dict.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define URL_Register    0x83e00
#define URL_GetURL      0x83e01
#define URL_Status      0x83e02
#define URL_ReadData    0x83e03
#define URL_Stop        0x83e05
#define URL_Deregister  0x83e06
#define URI_Dispatch    0x4e381

#define POP_W_PT    260.0f    /* content width, points */
#define POP_EM      11.0f
#define OS_PER_PT   2.5f      /* actual size, as the document window at 100% */
#define MARGIN      20        /* OS units round the content */
#define BUTTON_ROW  100       /* below the content, for Copy */
#define MAX_VIS_H   880
#define TIMEOUT_CS  2500
#define MAX_RESP    (1024 * 1024)
#define MAX_REDIRECTS 3
#define COPY_BUTTON 1         /* component id */

#define USER_AGENT "Folio/" FOLIO_VERSION_NUM " (RISC OS; https://github.com/adyoull/riscos-folio)"

static struct {
	ObjectId obj;
	int wimp;
	char word[64], lang[16];
	folio_page_mode mode;
	/* The lookup in progress. */
	int fetching, session, attempt, redirects;
	unsigned t0;
	folio_dict_source source;
	char url[512];
	char *resp;
	size_t len, cap;
	char error[320];          /* the last network error, for the final message */
	/* What's shown. */
	folio_doc *doc;
	int *area;                /* sprite area */
	int px_w, px_h;
	int work_w, work_h;       /* OS units */
	float content_h;          /* points */
	int has_defs;
	char *text;               /* UTF-8, for Copy */
} pop;

static unsigned now_cs(void)
{
	unsigned t;
	_swix(OS_ReadMonotonicTime, _OUT(0), &t);
	return t;
}

/* The core for the main thread (see docwin.c). */
static void core_now(void)
{
	worker_stop_for(NULL);
	core_lock();
}

/* ---- The pop-up window ------------------------------------------------- */

typedef struct {
	int flags, type;
	int x0, y0, x1, y1;
	ComponentId id;
	const char *help;
	int max_help;
	/* Action button */
	const char *text;
	int max_text;
	const char *click_show;
	int event;
} action_button;

static ObjectId create_popup(void)
{
	static tbx_window t;
	static action_button copy;
	static char title[80] = "Define";
	ObjectId id = 0;
	_kernel_oserror *e;

	memset(&copy, 0, sizeof copy);
	copy.flags = 1;                      /* the default button (Return) */
	copy.type = (int)(sizeof copy << 16) | 128;
	copy.x0 = 400; copy.y0 = -168; copy.x1 = 600; copy.y1 = -100;
	copy.id = COPY_BUTTON;
	copy.help = "Click to copy the definitions to the clipboard.";
	copy.max_help = (int)strlen(copy.help) + 1;
	copy.text = "Copy";
	copy.max_text = 8;
	copy.event = EV_DICT_COPY;

	memset(&t, 0, sizeof t);
	t.help_message = "This shows what the word means, from Wiktionary.|MClick on 'Wiktionary' to open the whole entry in your browser.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.num_gadgets = 1;
	t.gadgets = &copy;
	t.default_focus = -1;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x0 = 0;
	t.window.vis_y0 = 0;
	t.window.vis_x1 = 700;
	t.window.vis_y1 = 300;
	t.window.behind = -1;
	/* Moveable, title, vertical scroll bar, new-format flags; not auto-redraw. */
	t.window.window_flags = 0x80000002u | (1u << 26) | (1u << 28);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 255;
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_x0 = 0;
	t.window.work_y0 = -300;
	t.window.work_x1 = 700;
	t.window.work_y1 = 0;
	t.window.title_flags = 0x00000119;
	t.window.work_flags = 3 << 12;       /* click */
	t.window.sprite_area = (void *)1;
	t.window.title_text = title;
	t.window.title_validation = "";
	t.window.title_buflen = sizeof title;
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		app_log("dict: can't create the pop-up: %s", e->errmess);
		return 0;
	}
	window_get_wimp_handle(id, &pop.wimp);
	return id;
}

static int popup_open(void)
{
	int st[9];
	st[0] = pop.wimp;
	if (!pop.obj || _swix(Wimp_GetWindowState, _IN(1), st))
		return 0;
	return (st[8] & (1 << 16)) != 0;
}

/* Size the window to its content, keeping its top left where it is. */
static void popup_resize(void)
{
	int st[9], box[4], vis_h;
	box[0] = 0;
	box[1] = -pop.work_h;
	box[2] = pop.work_w;
	box[3] = 0;
	tbx_method(pop.obj, 15 /* Window_SetExtent */, (int)box, 0, 0);
	box[0] = pop.work_w - MARGIN - 200;
	box[1] = -pop.work_h + 16;
	box[2] = pop.work_w - MARGIN;
	box[3] = -pop.work_h + 16 + 68;
	tbx_method(pop.obj, 71 /* Gadget_MoveGadget */, COPY_BUTTON, (int)box, 0);
	tbx_method(pop.obj, 65 /* Gadget_SetFlags */, COPY_BUTTON,
		1 | (pop.has_defs ? 0 : (1u << 31)), 0);
	st[0] = pop.wimp;
	if (_swix(Wimp_GetWindowState, _IN(1), st) || !(st[8] & (1 << 16)))
		return;
	vis_h = pop.work_h < MAX_VIS_H ? pop.work_h : MAX_VIS_H;
	st[3] = st[1] + pop.work_w;
	st[2] = st[4] - vis_h;
	if (st[2] < 64) {             /* off the bottom of the screen: move it up */
		st[4] += 64 - st[2];
		st[2] = 64;
	}
	st[5] = 0;
	st[6] = 0;
	_swix(Wimp_OpenWindow, _IN(1), st);
	_swix(Wimp_ForceRedraw, _INR(0, 4), pop.wimp, 0, -pop.work_h, pop.work_w, 0);
}

/* Lays out html and draws it into the pop-up's sprite. */
static void set_content(const char *html, int has_defs)
{
	folio_loc loc;
	float ppp = OS_PER_PT / (float)(1 << app.xeig);
	int *area = NULL;
	char *text = NULL;
	folio_doc *doc;

	core_now();
	if (pop.doc)
		folio_close(pop.doc);
	pop.doc = doc = folio_open_html(app.fc, html, POP_W_PT, POP_EM);
	if (doc && folio_loc_from_number(doc, 0, &loc) == 0 &&
			folio_content_height(doc, loc, &pop.content_h) == 0) {
		int px_w = (int)(POP_W_PT * ppp + 0.5f), px_h = (int)((pop.content_h + 2) * ppp + 0.5f);
		size_t img = (size_t)px_w * 4 * px_h, bytes = 16 + 44 + img;
		int dpi = 180 >> app.xeig;
		folio_irect tile = { 0, 0, px_w, px_h };
		area = malloc(bytes);
		if (area) {
			int *spr = area + 4;
			area[0] = (int)bytes;
			area[1] = 1;
			area[2] = 16;
			area[3] = (int)bytes;
			spr[0] = (int)(44 + img);
			memset(spr + 1, 0, 12);
			strcpy((char *)(spr + 1), "dict");
			spr[4] = px_w - 1;
			spr[5] = px_h - 1;
			spr[6] = 0;
			spr[7] = 31;
			spr[8] = 44;
			spr[9] = 44;
			spr[10] = (6 << 27) | (dpi << 14) | (dpi << 1) | 1;
			if (folio_render_tile(doc, loc, ppp, tile, pop.mode, 0,
					(uint32_t *)(spr + 11), px_w * 4)) {
				app_log("dict: render: %s", folio_error(app.fc));
				free(area);
				area = NULL;
			} else {
				folio_point a = { 0, 0 }, b = { POP_W_PT, pop.content_h };
				pop.px_w = px_w;
				pop.px_h = px_h;
				text = folio_selection_text(doc, loc, a, b);
				if (text) {
					char *copy = strdup(text);
					folio_free(doc, text);
					text = copy;
				}
			}
		}
	} else {
		app_log("dict: layout: %s", folio_error(app.fc));
	}
	core_unlock();

	free(pop.area);
	pop.area = area;
	free(pop.text);
	pop.text = text;
	pop.has_defs = has_defs && area;
	pop.work_w = (area ? pop.px_w << app.xeig : 650) + 2 * MARGIN;
	pop.work_h = (area ? pop.px_h << app.xeig : 100) + MARGIN + BUTTON_ROW;
	popup_resize();
}

static void set_message(const char *msg)
{
	char *html = folio_dict_message_html(pop.word, msg);
	if (html) {
		set_content(html, 0);
		free(html);
	}
}

static int page_colour(void)
{
	switch (pop.mode) {
	case FOLIO_PAGE_DARK: return 0x00000000;
	case FOLIO_PAGE_SEPIA: return 0xd8ecf400;   /* BBGGRR00 of #f4ecd8 */
	default: return 0xffffff00;
	}
}

void dict_redraw(int *b)
{
	int more = 0;
	b[0] = pop.wimp;
	_swix(Wimp_RedrawWindow, _IN(1) | _OUT(0), b, &more);
	while (more) {
		int ox = b[1] - b[5], oy = b[4] - b[6];
		_swix(ColourTrans_SetGCOL, _IN(0) | _INR(3, 4), page_colour(), 0, 0);
		_swix(OS_Plot, _INR(0, 2), 4, b[7], b[8]);
		_swix(OS_Plot, _INR(0, 2), 96 + 5, b[9], b[10]);
		if (pop.area)
			_swix(OS_SpriteOp, _INR(0, 7), 52 + 512, pop.area, pop.area + 4,
				ox + MARGIN, oy - MARGIN - (pop.px_h << app.xeig), 0, 0, 0);
		_swix(Wimp_GetRectangle, _IN(1) | _OUT(0), b, &more);
	}
}

int dict_owns(int wimp_handle)
{
	return pop.obj && wimp_handle == pop.wimp;
}

/* A click on the content: the Wiktionary link opens in the browser. */
void dict_click(int *b)
{
	int st[9];
	folio_point p;
	folio_loc loc;
	folio_link link;
	int hit = 0;
	char uri[512];
	if (!pop.doc || b[4] != -1)
		return;
	st[0] = pop.wimp;
	_swix(Wimp_GetWindowState, _IN(1), st);
	p.x = ((b[0] - (st[1] - st[5])) - MARGIN) / OS_PER_PT;
	p.y = (-(b[1] - (st[4] - st[6])) - MARGIN) / OS_PER_PT;
	core_now();
	if (folio_loc_from_number(pop.doc, 0, &loc) == 0 && folio_link_at(pop.doc, loc, p, &link) &&
			link.uri && !strncmp(link.uri, "https://", 8)) {
		snprintf(uri, sizeof uri, "%s", link.uri);
		hit = 1;
	}
	core_unlock();
	if (hit) {
		_kernel_oserror *e = _swix(URI_Dispatch, _INR(0, 2), 0, uri, app.task);
		app_log("dict: open %s%s%s", uri, e ? ": " : "", e ? e->errmess : "");
		if (e)
			tbx_report("Folio couldn't open the page: no web browser has been seen by the Filer yet.");
		else
			tbx_hide(pop.obj);
	}
}

void dict_copy(void)
{
	if (pop.text && pop.has_defs)
		clip_set_text(pop.text);
}

/* ---- The cache --------------------------------------------------------- */
/* <Choices$Write>.Folio.Dict.D<key>: "folio-dict 1", the word, the language,
 * then the pop-up's HTML. */

static char *cache_read(void)
{
	char path[128], line[128];
	FILE *f;
	char *html = NULL;
	long n;
	snprintf(path, sizeof path, "Choices:Folio.Dict.D%08x", folio_dict_key(pop.word, pop.lang));
	f = fopen(path, "r");
	if (!f)
		return NULL;
	if (!fgets(line, sizeof line, f) || strcmp(line, "folio-dict 1\n"))
		goto out;
	if (!fgets(line, sizeof line, f) || strncmp(line, pop.word, strlen(pop.word)) ||
			line[strlen(pop.word)] != '\n')
		goto out;
	if (!fgets(line, sizeof line, f) || strncmp(line, pop.lang, strlen(pop.lang)) ||
			line[strlen(pop.lang)] != '\n')
		goto out;
	{
		long start = ftell(f);
		fseek(f, 0, SEEK_END);
		n = ftell(f) - start;
		fseek(f, start, SEEK_SET);
	}
	if (n <= 0 || n > MAX_RESP)
		goto out;
	html = malloc((size_t)n + 1);
	if (html) {
		n = (long)fread(html, 1, (size_t)n, f);
		html[n] = 0;
	}
out:
	fclose(f);
	return html;
}

static void cache_write(const char *html)
{
	const char *cw = getenv("Choices$Write");
	char path[256];
	FILE *f;
	if (!cw || !*cw)
		return;
	snprintf(path, sizeof path, "%s.Folio", cw);
	_swix(OS_File, _INR(0, 1) | _IN(4), 8, path, 0);
	snprintf(path, sizeof path, "%s.Folio.Dict", cw);
	_swix(OS_File, _INR(0, 1) | _IN(4), 8, path, 0);
	snprintf(path, sizeof path, "%s.Folio.Dict.D%08x", cw, folio_dict_key(pop.word, pop.lang));
	f = fopen(path, "w");
	if (!f)
		return;
	fprintf(f, "folio-dict 1\n%s\n%s\n%s", pop.word, pop.lang, html);
	fclose(f);
	_swix(OS_File, _INR(0, 2), 18, path, 0xfff);
}

/* ---- Fetching ---------------------------------------------------------- */

static void fetch_end(void)
{
	if (pop.session) {
		_swix(URL_Stop, _INR(0, 1), 0, pop.session);
		_swix(URL_Deregister, _INR(0, 1), 0, pop.session);
	}
	pop.session = 0;
	pop.len = 0;
}

static void lookup_done(const char *msg)
{
	fetch_end();
	pop.fetching = 0;
	if (msg && popup_open())
		set_message(msg);
}

/* RISC OS 5 keeps the fetchers on disk, in !System (HardDisc4), not in ROM:
 * load them the first time a word is looked up, as Browse's !Run would.
 * AcornHTTP needs URL_Fetcher; https needs AcornSSL. */
static void load_fetchers(void)
{
	static const char *const cmds[] = {
		"RMEnsure URL_Fetcher 0 IfThere System:Modules.Network.URL.URL Then RMLoad System:Modules.Network.URL.URL",
		"RMEnsure AcornHTTP 0 IfThere System:Modules.Network.URL.AcornHTTP Then RMLoad System:Modules.Network.URL.AcornHTTP",
		"RMEnsure AcornSSL 0 IfThere System:Modules.Network.URL.AcornSSL Then RMLoad System:Modules.Network.URL.AcornSSL",
	};
	size_t i;
	for (i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
		_kernel_oserror *e = _swix(OS_CLI, _IN(0), cmds[i]);
		app_log("dict: %s%s%s", cmds[i] + 9, e ? ": " : "", e ? e->errmess : "");
	}
}

/* Starts fetching pop.url. Returns 0, or -1 with pop.error set. */
static int fetch_start(void)
{
	_kernel_oserror *e;
	int session = 0;
	pop.len = 0;
	pop.t0 = now_cs();
	app_log("dict: GET %s", pop.url);
	{
		/* Once per run: the modules may be there but AcornHTTP or AcornSSL not. */
		static int loaded;
		if (!loaded) {
			load_fetchers();
			loaded = 1;
		}
	}
	e = _swix(URL_Register, _IN(0) | _OUT(1), 0, &session);
	if (e) {
		if ((e->errnum & 0xffffff) == 0x1e6)
			snprintf(pop.error, sizeof pop.error, "Folio can't look words up: the URL_Fetcher module "
				"isn't loaded, and it isn't in System:Modules.Network.URL. Updating !System from "
				"RISC OS Open's HardDisc4 adds it.");
		else
			snprintf(pop.error, sizeof pop.error, "Folio can't look words up: %s", e->errmess);
		return -1;
	}
	pop.session = session;
	/* R0 bit 0: R6 is the User-Agent. R5 = 2: the response's headers and body. */
	e = _swix(URL_GetURL, _INR(0, 6), 1, session, 1, pop.url, "", 2, USER_AGENT);
	if (e) {
		snprintf(pop.error, sizeof pop.error, "Folio couldn't reach the dictionary: %s", e->errmess);
		fetch_end();
		return -1;
	}
	return 0;
}

/* Starts the next source to try, or ends the lookup. */
static void next_attempt(void)
{
	for (;;) {
		if (folio_dict_url(pop.word, pop.attempt, pop.url, sizeof pop.url, &pop.source)) {
			char msg[340];
			if (pop.error[0])
				snprintf(msg, sizeof msg, "%s", pop.error);
			else
				snprintf(msg, sizeof msg, "No definition was found for this word.");
			lookup_done(msg);
			return;
		}
		pop.redirects = 0;
		if (fetch_start() == 0)
			return;
		/* URL_Fetcher missing: no other source can work either. */
		if (strstr(pop.error, "isn't loaded")) {
			lookup_done(pop.error);
			return;
		}
		pop.attempt++;
	}
}

/* The response is all here. */
static void fetch_complete(int http_status)
{
	const char *body;
	size_t body_len;
	int status;
	char location[512];
	if (folio_http_parse(pop.resp, pop.len, &status, &body, &body_len, location, sizeof location)) {
		snprintf(pop.error, sizeof pop.error, "The dictionary's reply was cut short.");
		fetch_end();
		pop.attempt++;
		next_attempt();
		return;
	}
	if (status == 0)
		status = http_status;
	app_log("dict: HTTP %d, %d bytes", status, (int)body_len);
	if ((status == 301 || status == 302 || status == 303 || status == 307 || status == 308) &&
			location[0] && pop.redirects < MAX_REDIRECTS) {
		fetch_end();
		if (location[0] == '/') {
			/* Same scheme and host. */
			char *p = strstr(pop.url, "://");
			char *slash = p ? strchr(p + 3, '/') : NULL;
			if (slash)
				*slash = 0;
			snprintf(pop.url + strlen(pop.url), sizeof pop.url - strlen(pop.url), "%s", location);
		} else {
			snprintf(pop.url, sizeof pop.url, "%s", location);
		}
		pop.redirects++;
		if (fetch_start()) {
			pop.attempt++;
			next_attempt();
		}
		return;
	}
	if (status == 200) {
		char *html = folio_dict_html(pop.source, pop.word, body, body_len, pop.lang);
		if (html) {
			cache_write(html);
			lookup_done(NULL);
			if (popup_open())
				set_content(html, 1);
			free(html);
			return;
		}
	} else if (status != 404) {
		snprintf(pop.error, sizeof pop.error, "The dictionary replied with error %d.", status);
	}
	fetch_end();
	pop.attempt++;
	next_attempt();
}

int dict_background(void)
{
	int rounds;
	if (!pop.fetching)
		return 0;
	for (rounds = 0; rounds < 8 && pop.session; rounds++) {
		int status = 0, got = 0, left = 0;
		_kernel_oserror *e;
		if (pop.cap - pop.len < 8192) {
			size_t cap = pop.cap ? pop.cap * 2 : 32768;
			char *p;
			if (cap > MAX_RESP) {
				snprintf(pop.error, sizeof pop.error, "The dictionary's reply was too long.");
				fetch_end();
				pop.attempt++;
				next_attempt();
				return pop.fetching;
			}
			p = realloc(pop.resp, cap);
			if (!p) {
				lookup_done("Not enough memory to look the word up.");
				return 0;
			}
			pop.resp = p;
			pop.cap = cap;
		}
		e = _swix(URL_ReadData, _INR(0, 3) | _OUT(0) | _OUTR(4, 5), 0, pop.session,
			pop.resp + pop.len, (int)(pop.cap - pop.len - 1), &status, &got, &left);
		if (e) {
			snprintf(pop.error, sizeof pop.error, "Folio couldn't reach the dictionary: %s", e->errmess);
			app_log("dict: %s", e->errmess);
			fetch_end();
			pop.attempt++;
			next_attempt();
			return pop.fetching;
		}
		if (got > 0)
			pop.len += (size_t)got;
		pop.resp[pop.len] = 0;
		if (status & (1 << 6)) {
			snprintf(pop.error, sizeof pop.error, "Folio couldn't reach the dictionary (is the network on?).");
			app_log("dict: transfer aborted");
			fetch_end();
			pop.attempt++;
			next_attempt();
			return pop.fetching;
		}
		if ((status & (1 << 5)) && (left == 0 || got == 0)) {
			int http = 0;
			_swix(URL_Status, _INR(0, 1) | _OUT(2), 0, pop.session, &http);
			fetch_complete(http);
			return pop.fetching;
		}
		if (got <= 0)
			break;
	}
	if (pop.session && (int)(now_cs() - pop.t0) > TIMEOUT_CS) {
		snprintf(pop.error, sizeof pop.error, "The dictionary took too long to answer.");
		fetch_end();
		pop.attempt++;
		next_attempt();
	}
	return pop.fetching;
}

/* ---- Looking a word up -------------------------------------------------- */

void dict_define(const char *word, const char *lang, folio_page_mode mode, int x, int y)
{
	char title[100], *html;
	int pos[2], cached;
	if (!pop.obj && !(pop.obj = create_popup()))
		return;
	if (pop.fetching)
		lookup_done(NULL);
	snprintf(pop.word, sizeof pop.word, "%s", word);
	snprintf(pop.lang, sizeof pop.lang, "%s", lang ? lang : "");
	pop.mode = mode;
	pop.error[0] = 0;
	pop.attempt = 0;
	snprintf(title, sizeof title, "Define \"%s\"", word);
	window_set_title(pop.obj, title);

	html = cache_read();
	cached = html != NULL;
	if (cached) {
		app_log("dict: '%s' from the cache", word);
		set_content(html, 1);
		free(html);
	} else {
		set_message("Looking it up\xe2\x80\xa6");
	}
	/* Transient (Wimp_CreateMenu semantics: a click elsewhere removes it),
	 * top left just below the word. */
	pos[0] = x;
	pos[1] = y;
	tbx_show(pop.obj, 1, 2, pos, 0, -1);
	popup_resize();
	if (!cached) {
		pop.fetching = 1;
		next_attempt();
	}
}

void dict_quit(void)
{
	fetch_end();
	pop.fetching = 0;
}
