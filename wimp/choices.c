/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* The Choices window (Choices... on the icon bar menu; Style Guide p91-93):
 * how new documents open, reading aloud, text recognition and security.
 * Persistent, with Default (the built-in settings, shown but not yet used),
 * Cancel and Save (used at once and kept in <Choices$Write>.Folio.Choices).
 * Nothing changes until Save. */
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

enum {
	C_BOX_NEW = 0, C_SCALE_L, C_SCALE, C_PC, C_NORMAL, C_DARK, C_SEPIA, C_TWO, C_TOOLBAR,
	C_BOX_SPEECH, C_SLOW, C_MEDIUM, C_FAST,
	C_BOX_OCR, C_ACCURATE, C_QUICK, C_AUTO,
	C_BOX_SEC, C_OBEY, C_ASK, C_WARN, C_JS,
	C_DEFAULT, C_CANCEL, C_SAVE,
	C_COUNT
};

typedef struct {
	int flags, type;
	int x0, y0, x1, y1;
	ComponentId id;
	const char *help;
	int max_help;
} gadget_hdr;

typedef struct { gadget_hdr h; const char *label; } label_gadget;
typedef struct { gadget_hdr h; const char *label; } box_gadget;
typedef struct { gadget_hdr h; const char *label; int max_label; int event; } option_gadget;
typedef struct { gadget_hdr h; int group; const char *label; int max_label; int event; } radio_gadget;
typedef struct {
	gadget_hdr h;
	int lower, upper, step, initial, precision;
	ComponentId before, after;
	int display_length;
} range_gadget;
typedef struct {
	gadget_hdr h;
	const char *text;
	int max_text;
	const char *click_show;
	int event;
} button_gadget;

static struct {
	box_gadget box_new;
	label_gadget scale_l;
	range_gadget scale;
	label_gadget pc;
	radio_gadget normal, dark, sepia;
	option_gadget two, toolbar;
	box_gadget box_speech;
	radio_gadget slow, medium, fast;
	box_gadget box_ocr;
	radio_gadget accurate, quick;
	option_gadget autoocr;
	box_gadget box_sec;
	option_gadget obey, ask, warn, js;
	button_gadget def, cancel, save;
} g;

static ObjectId obj;

static void hdr(gadget_hdr *h, int type, int size, int id, int x0, int y0, int x1, int y1,
		const char *help)
{
	h->flags = 0;
	h->type = (size << 16) | type;
	h->x0 = x0; h->y0 = y0; h->x1 = x1; h->y1 = y1;
	h->id = id;
	h->help = help;
	h->max_help = help ? (int)strlen(help) + 1 : 0;
}

static void box(box_gadget *b, int id, int y1, int y0, const char *label)
{
	hdr(&b->h, 256, sizeof *b, id, 16, y0, 912, y1, NULL);
	b->h.flags = 1u << 30;            /* behind the other gadgets */
	b->label = label;
}

static void option(option_gadget *o, int id, int x0, int y1, int x1, const char *label,
		const char *help)
{
	hdr(&o->h, 192, sizeof *o, id, x0, y1 - 48, x1, y1, help);
	o->label = label;
	o->max_label = (int)strlen(label) + 1;
	o->event = 0;
}

static void radio(radio_gadget *r, int id, int group, int x0, int y1, int x1, const char *label,
		const char *help)
{
	hdr(&r->h, 384, sizeof *r, id, x0, y1 - 48, x1, y1, help);
	r->group = group;
	r->label = label;
	r->max_label = (int)strlen(label) + 1;
	r->event = 0;
}

static void button(button_gadget *b, int id, int flags, int x0, int x1, const char *text,
		int event, const char *help)
{
	hdr(&b->h, 128, sizeof *b, id, x0, -1092, x1, -1024, help);
	b->h.flags = flags;
	b->text = text;
	b->max_text = (int)strlen(text) + 1;
	b->event = event;
}

#define BOX_W 928
#define BOX_H 1116

static ObjectId create_window(void)
{
	static tbx_window t;
	static char title[32] = "Folio choices";
	ObjectId id = 0;
	_kernel_oserror *e;

	memset(&g, 0, sizeof g);
	/* New documents. */
	box(&g.box_new, C_BOX_NEW, -24, -300, "New documents");
	hdr(&g.scale_l.h, 320, sizeof g.scale_l, C_SCALE_L, 48, -120, 176, -72, NULL);
	g.scale_l.label = "Scale";
	hdr(&g.scale.h, 832, sizeof g.scale, C_SCALE, 176, -124, 416, -68,
		"Type the scale new documents open at, or click the arrows to change it.");
	g.scale.h.flags = (1 << 2) | (1 << 4) | (1 << 8);   /* writable, arrows, right-justified */
	g.scale.lower = 10;
	g.scale.upper = 800;
	g.scale.step = 10;
	g.scale.initial = 100;
	g.scale.precision = 0;
	g.scale.before = g.scale.after = -1;
	g.scale.display_length = 120;
	hdr(&g.pc.h, 320, sizeof g.pc, C_PC, 424, -120, 480, -72, NULL);
	g.pc.label = "%";
	radio(&g.normal, C_NORMAL, 1, 48, -148, 304, "Normal page",
		"Click for new documents to show their pages as they are.");
	radio(&g.dark, C_DARK, 1, 320, -148, 576, "Dark page",
		"Click for new documents to show a black page with light text.");
	radio(&g.sepia, C_SEPIA, 1, 592, -148, 880, "Sepia page",
		"Click for new documents to show a sepia-tinted page.");
	option(&g.two, C_TWO, 48, -212, 480, "Two pages side by side",
		"Click to choose whether new documents show two pages side by side.");
	option(&g.toolbar, C_TOOLBAR, 496, -212, 880, "Show the toolbar",
		"Click to choose whether new document windows have the toolbar.");
	/* Reading aloud. */
	box(&g.box_speech, C_BOX_SPEECH, -324, -444, "Reading aloud");
	radio(&g.slow, C_SLOW, 2, 48, -372, 304, "Slower",
		"Click to read aloud a little slower than normal.");
	radio(&g.medium, C_MEDIUM, 2, 320, -372, 576, "Normal",
		"Click to read aloud at the normal speed.");
	radio(&g.fast, C_FAST, 2, 592, -372, 880, "Faster",
		"Click to read aloud at the voice's own, faster speed.");
	/* Text recognition. */
	box(&g.box_ocr, C_BOX_OCR, -468, -652, "Text recognition (OCR plugin)");
	radio(&g.accurate, C_ACCURATE, 3, 48, -516, 304, "Accurate",
		"Click to recognise scanned text with fewer mistakes (about twice as long).");
	radio(&g.quick, C_QUICK, 3, 320, -516, 576, "Fast",
		"Click to recognise scanned text in about half the time.");
	option(&g.autoocr, C_AUTO, 48, -580, 880, "Recognise scanned pages as they're shown",
		"Click to choose whether Folio reads the text of scanned pages as you come to them.");
	/* Security. */
	box(&g.box_sec, C_BOX_SEC, -676, -1000, "Security");
	option(&g.obey, C_OBEY, 48, -724, 900, "Keep to a PDF's limits on printing and copying",
		"Click to choose whether Folio obeys a PDF's \"no printing\" and \"no copying\" settings. "
		"Its owner password lifts them.");
	option(&g.ask, C_ASK, 48, -788, 900, "Ask before following links out of a document",
		"Click to choose whether Folio asks before opening a web page, an email or another file "
		"that a link in a document leads to.");
	option(&g.warn, C_WARN, 48, -852, 900, "Warn about scripts, attached files and actions",
		"Click to choose whether Folio says, when it opens a PDF, that it has JavaScript, "
		"attached files, a form or actions that launch other programs.");
	option(&g.js, C_JS, 48, -916, 900, "Run JavaScript in PDF forms",
		"Click to let a form's JavaScript work out totals and check what's typed. "
		"Folio never lets it reach files or the network.");
	/* Default, Cancel, Save. */
	button(&g.def, C_DEFAULT, 1 << 2 /* local: stays open */, 24, 216, "Default",
		EV_CHOICES_DEFAULT, "Click to fill in Folio's own settings (Save to use them).");
	button(&g.cancel, C_CANCEL, 2 /* Escape */, 504, 696, "Cancel", EV_CHOICES_CANCEL,
		"Click to close this window without changing anything.");
	button(&g.save, C_SAVE, 1 /* default */, 712, 904, "Save", EV_CHOICES_SAVE,
		"Click (or press Return) to use these choices and keep them.");

	memset(&t, 0, sizeof t);
	t.flags = (1 << 1) | (1 << 2);
	t.help_message = "Folio's choices: click on a choice, then Save.";
	t.max_help = (int)strlen(t.help_message) + 1;
	t.num_gadgets = C_COUNT;
	t.gadgets = &g;
	t.default_focus = -1;
	t.show_event = -1;
	t.hide_event = -1;
	t.window.vis_x1 = BOX_W;
	t.window.vis_y1 = BOX_H;
	t.window.behind = -1;
	t.window.window_flags = 0x80000002u | (1u << 4) | (1u << 26);
	t.window.title_fore = 7;
	t.window.title_back = 2;
	t.window.work_fore = 7;
	t.window.work_back = 1;
	t.window.scroll_outer = 3;
	t.window.scroll_inner = 1;
	t.window.title_focus = 12;
	t.window.work_y0 = -BOX_H;
	t.window.work_x1 = BOX_W;
	t.window.title_flags = 0x00000119;
	t.window.work_flags = 0;
	t.window.sprite_area = (void *)1;
	t.window.title_text = title;
	t.window.title_validation = "";
	t.window.title_buflen = sizeof title;
	e = tbx_create(Class_Window, 102, 0, &t, sizeof t, &id);
	if (e) {
		tbx_report_error(e);
		return 0;
	}
	return id;
}

static void set_option(int id, int on) { tbx_method(obj, 196, id, on != 0, 0); }
static void set_radio(int id) { tbx_method(obj, 388, id, 1, 0); }
static int get_option(int id) { int v = 0; tbx_method_out(obj, 197, id, &v); return v != 0; }
static int get_radio(int id) { int v = 0; tbx_method_out(obj, 389, id, &v); return v != 0; }

static void fill(const choices *c)
{
	tbx_method(obj, 832 /* NumberRange_SetValue */, C_SCALE, (int)(c->zoom * 100 + 0.5f), 0);
	set_radio(c->mode == FOLIO_PAGE_DARK ? C_DARK : c->mode == FOLIO_PAGE_SEPIA ? C_SEPIA : C_NORMAL);
	set_option(C_TWO, c->cols == 2);
	set_option(C_TOOLBAR, c->toolbar);
	set_radio(c->speed == SPEED_SLOW ? C_SLOW : c->speed == SPEED_FAST ? C_FAST : C_MEDIUM);
	set_radio(c->ocr_fast ? C_QUICK : C_ACCURATE);
	set_option(C_AUTO, c->ocr_auto);
	set_option(C_OBEY, c->obey_restrictions);
	set_option(C_ASK, c->ask_links);
	set_option(C_WARN, c->warn_content);
	set_option(C_JS, c->run_js);
}

void choices_open(void)
{
	if (!obj && !(obj = create_window()))
		return;
	fill(&app.ch);
	/* In the middle of the screen. */
	{
		int pos[2];
		pos[0] = (app.screen_w - BOX_W) / 2;
		pos[1] = (app.screen_h + BOX_H) / 2;
		tbx_show(obj, 0, 2, pos, 0, -1);
	}
}

void choices_event(int code)
{
	choices c;
	int pc = 100;
	if (!obj)
		return;
	if (code == EV_CHOICES_DEFAULT) {
		choices_defaults(&c);
		fill(&c);
		return;
	}
	if (code != EV_CHOICES_SAVE)
		return;
	c = app.ch;
	tbx_method_out(obj, 833 /* NumberRange_GetValue */, C_SCALE, &pc);
	if (pc < 10) pc = 10;
	if (pc > 800) pc = 800;
	c.zoom = pc / 100.0f;
	c.mode = get_radio(C_DARK) ? FOLIO_PAGE_DARK : get_radio(C_SEPIA) ? FOLIO_PAGE_SEPIA :
		FOLIO_PAGE_NORMAL;
	c.cols = get_option(C_TWO) ? 2 : 1;
	c.toolbar = get_option(C_TOOLBAR);
	c.speed = get_radio(C_SLOW) ? SPEED_SLOW : get_radio(C_FAST) ? SPEED_FAST : SPEED_NORMAL;
	c.ocr_fast = get_radio(C_QUICK);
	c.ocr_auto = get_option(C_AUTO);
	c.obey_restrictions = get_option(C_OBEY);
	c.ask_links = get_option(C_ASK);
	c.warn_content = get_option(C_WARN);
	c.run_js = get_option(C_JS);
	choices_apply(&c);
	choices_save();
}

/* ---- Load and save ---------------------------------------------------------
 * <Choices$Write>.Folio.Choices, read back through Choices: (Style Guide
 * p92): "folio-choices 1", then a line per choice. Lines Folio doesn't know
 * are skipped, and missing ones keep the defaults. */

void choices_defaults(choices *c)
{
	memset(c, 0, sizeof *c);
	c->zoom = 1.0f;
	c->mode = FOLIO_PAGE_NORMAL;
	c->cols = 1;
	c->toolbar = 1;
	c->speed = SPEED_NORMAL;
	c->ocr_fast = 0;
	c->ocr_auto = 1;
	c->obey_restrictions = 1;
	c->ask_links = 1;
	c->warn_content = 1;
	c->run_js = 0;
}

void choices_load(void)
{
	FILE *f = fopen("Choices:Folio.Choices", "r");
	char line[80];
	choices *c = &app.ch;
	choices_defaults(c);
	if (f) {
		if (fgets(line, sizeof line, f) && !strncmp(line, "folio-choices 1", 15))
			while (fgets(line, sizeof line, f)) {
				int v;
				char word[16];
				if (sscanf(line, "zoom %d", &v) == 1 && v >= 10 && v <= 800)
					c->zoom = v / 100.0f;
				else if (sscanf(line, "page %15s", word) == 1)
					c->mode = !strcmp(word, "dark") ? FOLIO_PAGE_DARK :
						!strcmp(word, "sepia") ? FOLIO_PAGE_SEPIA : FOLIO_PAGE_NORMAL;
				else if (sscanf(line, "columns %d", &v) == 1)
					c->cols = v == 2 ? 2 : 1;
				else if (sscanf(line, "toolbar %d", &v) == 1)
					c->toolbar = v != 0;
				else if (sscanf(line, "speed %15s", word) == 1)
					c->speed = !strcmp(word, "slower") ? SPEED_SLOW :
						!strcmp(word, "faster") ? SPEED_FAST : SPEED_NORMAL;
				else if (!strncmp(line, "ocr fast", 8))
					c->ocr_fast = 1;
				else if (!strncmp(line, "ocr accurate", 12))
					c->ocr_fast = 0;
				else if (sscanf(line, "ocr-auto %d", &v) == 1)
					c->ocr_auto = v != 0;
				else if (sscanf(line, "obey-restrictions %d", &v) == 1)
					c->obey_restrictions = v != 0;
				else if (sscanf(line, "ask-links %d", &v) == 1)
					c->ask_links = v != 0;
				else if (sscanf(line, "warn-content %d", &v) == 1)
					c->warn_content = v != 0;
				else if (sscanf(line, "run-javascript %d", &v) == 1)
					c->run_js = v != 0;
			}
		fclose(f);
	}
	app_log("choices: new documents at %d%% (%s page, %d across%s); %s restrictions; links %s; "
		"warnings %s; JavaScript %s", (int)(c->zoom * 100 + 0.5f),
		c->mode == FOLIO_PAGE_DARK ? "dark" : c->mode == FOLIO_PAGE_SEPIA ? "sepia" : "normal",
		c->cols, c->toolbar ? "" : ", no toolbar", c->obey_restrictions ? "obey" : "ignore",
		c->ask_links ? "ask" : "followed", c->warn_content ? "on" : "off",
		c->run_js ? "runs" : "off");
}

void choices_save(void)
{
	const char *cw = getenv("Choices$Write");
	const choices *c = &app.ch;
	char path[220];
	FILE *f;
	if (!cw || !*cw)
		return;
	snprintf(path, sizeof path, "%s.Folio", cw);
	_swix(OS_File, _INR(0, 1) | _IN(4), 8, path, 0);
	snprintf(path, sizeof path, "%s.Folio.Choices", cw);
	f = fopen(path, "w");
	if (!f) {
		tbx_report("Folio couldn't save its choices.");
		return;
	}
	fprintf(f, "folio-choices 1\nzoom %d\npage %s\ncolumns %d\ntoolbar %d\nspeed %s\n"
		"ocr %s\nocr-auto %d\nobey-restrictions %d\nask-links %d\nwarn-content %d\n"
		"run-javascript %d\n",
		(int)(c->zoom * 100 + 0.5f),
		c->mode == FOLIO_PAGE_DARK ? "dark" : c->mode == FOLIO_PAGE_SEPIA ? "sepia" : "normal",
		c->cols, c->toolbar,
		c->speed == SPEED_SLOW ? "slower" : c->speed == SPEED_FAST ? "faster" : "normal",
		c->ocr_fast ? "fast" : "accurate", c->ocr_auto, c->obey_restrictions, c->ask_links,
		c->warn_content, c->run_js);
	fclose(f);
	_swix(OS_File, _INR(0, 2), 18, path, 0xfff);
}

void choices_apply(const choices *c)
{
	int ocr_changed = c->ocr_fast != app.ch.ocr_fast || c->ocr_auto != app.ch.ocr_auto;
	app.ch = *c;
	speech_set_speed(c->speed);
	if (ocr_changed)
		docwin_ocr_settings_changed();
	app_log("choices: saved");
}
