/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Reading aloud, with the Speech plugin: a folder (Speech) in
 * <Folio$Dir>.Plugins or <Choices$Write>.Folio.Plugins holding a Piper voice
 * (Voices), eSpeak NG's data (espeak-ng-data) and LoadSound (an Obey file
 * that loads SharedSound, StreamManager and SharedSoundBuffer). Without the
 * plugin there's no speech button or menu entries, and nothing is loaded.
 *
 * The text comes a page at a time (folio_get_page_text), with any sentence
 * a page leaves unfinished carried to the next (readtext.c). The speech
 * thread (speaker.c) makes each sentence's audio a few seconds ahead; here,
 * in null polls, the audio goes to SharedSoundBuffer (as riscos-midisynth's
 * output_ssb.c and RDPClient do), and the sentence being heard is shown in
 * the window (docwin_set_spoken), which turns the pages as it goes. */
#include "app.h"
#include "readtext.h"
#include "speaker.h"
#include "worker.h"
#include <kernel.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <swis.h>

#define XSharedSoundBuffer_OpenStream         (0x20000 | 0x55FC0)
#define XSharedSoundBuffer_CloseStream        (0x20000 | 0x55FC1)
#define XSharedSoundBuffer_Volume             (0x20000 | 0x55FC4)
#define XSharedSoundBuffer_SampleRate         (0x20000 | 0x55FC5)
#define XSharedSoundBuffer_Pause              (0x20000 | 0x55FC9)
#define XSharedSoundBuffer_ReturnStreamHandle (0x20000 | 0x55FCE)
#define XStreamManager_AddBlock               (0x20000 | 0x57282)
#define XStreamManager_SetBuffer              (0x20000 | 0x57287)
#define XStreamManager_BufferStats            (0x20000 | 0x57288)

#define BLOCK_FRAMES 1024
#define BLOCK_BYTES (BLOCK_FRAMES * 4)       /* 16-bit stereo */
#define QUEUE_SECONDS 2                      /* kept queued in StreamManager */
#define CHUNKS 8                             /* chunks kept for their boxes */
#define MAX_BOXES 64
/* Time for the speech thread is given out steadily: in every PERIOD_CS, a
 * share ("duty") that depends on how much sound is ready. Nearly nothing
 * ready (BEHIND_SECONDS or less): all of it. Nearly SPEAKER_AHEAD_SECONDS
 * ready: a little (MIN_DUTY). In between, in proportion. So it settles
 * where its share matches what the voice costs (about 0.6 of real time on
 * a Pi 4), using a steady part of the machine instead of a burst of 100%
 * then nothing. */
#define PERIOD_CS 10
#define BEHIND_SECONDS 5
#define URGENT_SECONDS 7
#define MIN_DUTY 0.15f

typedef struct inflight {
	struct inflight *next;
	sp_audio *a;
	int pos;                  /* samples of it sent */
	int pause;                /* samples of silence after it */
	int stream_start, stream_end;   /* bytes in the stream */
	int shown;
} inflight;

static struct {
	int available;
	char dir[256], voice[320], data[320];
	int sound_loaded;
	int speed;                /* SPEED_* */
	/* The reading under way. */
	docwin *w;
	int selection_only;
	int next_page;            /* the next page to fetch */
	int text_done;            /* every page fetched */
	rt_chunk carry;           /* the unfinished sentence a page ended with */
	struct { int id; rt_chunk c; } chunk[CHUNKS];
	int next_id;
	/* Output. */
	int ssb, stream, open, started;
	int added;                /* bytes added to the stream */
	inflight *fly, *fly_tail; /* sent (or being sent), not yet heard */
	inflight *shown;
	short block[BLOCK_FRAMES * 2];
	int block_fill;           /* frames in block */
	int loading_said;
	/* Waiting for the first page's text to be recognised (a scan). */
	int start_wait, start_page;
	unsigned next_turn;       /* when the speech thread next gets time */
	unsigned given, window_start;   /* time given to it, for the log */
	float duty;               /* its share of each PERIOD_CS, smoothed */
	int shown_chunk, shown_start, shown_end;   /* the sentence highlighted */
	folio_point start_a, start_b;
} sp;

static unsigned now_cs(void)
{
	unsigned t;
	_swix(OS_ReadMonotonicTime, _OUT(0), &t);
	return t;
}

/* ---- The plugin ----------------------------------------------------------- */

static int object_type(const char *path)
{
	int type = 0;
	if (_swix(OS_File, _INR(0, 1) | _OUT(0), 17, path, &type))
		return 0;
	return type;
}

/* path made canonical (no system variables or paths), in out. */
static int canonical(const char *path, char *out, int size)
{
	int spare = 0;
	if (_swix(OS_FSControl, _INR(0, 5) | _OUT(5), 37, path, out, 0, 0, size, &spare) || spare <= 0)
		return -1;
	return 0;
}

/* The first file in dir (a voice), into out. */
static int first_file(const char *dir, char *out, int size)
{
	int buf[64];              /* word-aligned: OS_GBPB 10 writes words */
	int offset = 0;
	while (offset != -1) {
		int read = 0;
		struct { int load, exec, len, attr, type; char name[200]; } *e = (void *)buf;
		if (_swix(OS_GBPB, _INR(0, 6) | _OUTR(3, 4), 10, dir, buf, 1, offset, sizeof buf, 0,
				&read, &offset))
			return -1;
		if (read > 0 && e->type == 1) {
			snprintf(out, size, "%.200s.%.100s", dir, e->name);
			return 0;
		}
	}
	return -1;
}

static int try_plugin(const char *where)
{
	char dir[256], voices[280], data[280];
	if (canonical(where, dir, sizeof dir) || (object_type(dir) & 2) == 0)
		return 0;
	snprintf(voices, sizeof voices, "%s.Voices", dir);
	snprintf(data, sizeof data, "%s.espeak-ng-data", dir);
	if (!(object_type(voices) & 2) || !(object_type(data) & 2))
		return 0;
	if (first_file(voices, sp.voice, sizeof sp.voice))
		return 0;
	snprintf(sp.dir, sizeof sp.dir, "%s", dir);
	snprintf(sp.data, sizeof sp.data, "%s", data);
	return 1;
}

int speech_init(void)
{
	sp.speed = SPEED_NORMAL;
	sp.available = try_plugin("<Folio$Dir>.Plugins.Speech") ||
		try_plugin("<Choices$Write>.Folio.Plugins.Speech");
	if (sp.available)
		app_log("speech: plugin %s, voice %s", sp.dir, sp.voice);
	else
		app_log("speech: no plugin (Plugins.Speech)");
	return sp.available;
}

int speech_available(void)
{
	return sp.available;
}

/* ---- Sound out ------------------------------------------------------------- */

static int ssb_queued(void)
{
	_kernel_swi_regs r;
	r.r[0] = sp.stream;
	if (_kernel_swi(XStreamManager_BufferStats, &r, &r))
		return -1;
	return r.r[0] - r.r[1];
}

static int ssb_played(void)
{
	_kernel_swi_regs r;
	r.r[0] = sp.stream;
	if (_kernel_swi(XStreamManager_BufferStats, &r, &r))
		return -1;
	return r.r[1];
}

static void ssb_pause(int play)
{
	_kernel_swi_regs r;
	r.r[0] = sp.ssb;
	r.r[1] = play ? 1 : 0;
	_kernel_swi(XSharedSoundBuffer_Pause, &r, &r);
}

static int ssb_open(int rate)
{
	_kernel_swi_regs r;
	_kernel_oserror *e;
	r.r[0] = 2;                     /* R2 holds the block size */
	r.r[1] = (int)"Folio";
	r.r[2] = BLOCK_BYTES;
	if ((e = _kernel_swi(XSharedSoundBuffer_OpenStream, &r, &r)) != NULL) {
		char msg[400];
		snprintf(msg, sizeof msg, "Folio can't read aloud: %s. It needs SharedSound, StreamManager "
			"and SharedSoundBuffer (see !Help).", e->errmess);
		tbx_report(msg);
		return -1;
	}
	sp.ssb = r.r[0];
	r.r[0] = sp.ssb;
	if ((e = _kernel_swi(XSharedSoundBuffer_ReturnStreamHandle, &r, &r)) != NULL) {
		r.r[0] = sp.ssb;
		_kernel_swi(XSharedSoundBuffer_CloseStream, &r, &r);
		tbx_report_error(e);
		return -1;
	}
	sp.stream = r.r[0];
	r.r[0] = sp.stream;
	r.r[1] = rate * 4 * QUEUE_SECONDS * 4;
	_kernel_swi(XStreamManager_SetBuffer, &r, &r);
	r.r[0] = sp.ssb;
	r.r[1] = rate * 1024;
	_kernel_swi(XSharedSoundBuffer_SampleRate, &r, &r);
	r.r[0] = sp.ssb;
	r.r[1] = (int)0xffffffffu;
	_kernel_swi(XSharedSoundBuffer_Volume, &r, &r);
	ssb_pause(0);                   /* until two blocks are queued */
	sp.open = 1;
	sp.started = 0;
	sp.added = 0;
	sp.block_fill = 0;
	return 0;
}

static void ssb_close(void)
{
	_kernel_swi_regs r;
	if (!sp.open)
		return;
	r.r[0] = sp.ssb;
	_kernel_swi(XSharedSoundBuffer_CloseStream, &r, &r);
	sp.open = 0;
}

static int add_block(void)
{
	_kernel_swi_regs r;
	r.r[0] = sp.stream;
	r.r[1] = (int)sp.block;
	r.r[2] = BLOCK_BYTES;
	if (_kernel_swi(XStreamManager_AddBlock, &r, &r))
		return -1;
	sp.added += BLOCK_BYTES;
	sp.block_fill = 0;
	if (!sp.started && ssb_queued() >= BLOCK_BYTES * 2) {
		ssb_pause(1);
		sp.started = 1;
	}
	return 0;
}

/* ---- Chunks of text ------------------------------------------------------- */

static rt_chunk *chunk_by_id(int id)
{
	int i;
	for (i = 0; i < CHUNKS; i++)
		if (sp.chunk[i].id == id && sp.chunk[i].c.text)
			return &sp.chunk[i].c;
	return NULL;
}

/* Keeps c (taken over) under a new id, and gives it to the speech thread. */
static void push_chunk(rt_chunk *c)
{
	int slot = sp.next_id % CHUNKS, id = ++sp.next_id;
	if (!c->len || rt_skip_space(c->text, c->len, 0) == c->len) {
		rt_free(c);
		return;
	}
	rt_free(&sp.chunk[slot].c);
	sp.chunk[slot].id = id;
	sp.chunk[slot].c = *c;
	memset(c, 0, sizeof *c);
	speaker_push(id, sp.chunk[slot].c.text, sp.chunk[slot].c.len,
		sp.speed == SPEED_SLOW ? 1.4f : sp.speed == SPEED_FAST ? 1.0f : 1.2f);
}

/* Page 'page''s text, from character 'from_ch' (and to 'to_ch', or -1),
 * after the carried sentence; what it leaves unfinished is carried on. */
static int fetch_page(int page, int from_ch, int to_ch, int whole_sentence)
{
	folio_page_text t;
	folio_loc loc;
	folio_doc *doc = docwin_doc(sp.w);
	rt_chunk c;
	int from = 0, to, rc, i;
	core_now();
	rc = folio_loc_from_number(doc, page, &loc) == 0 ? folio_get_page_text(doc, loc, &t) : -1;
	core_unlock();
	if (rc)
		return -1;
	to = t.len;
	if (from_ch > 0 || to_ch >= 0) {
		for (i = 0; i < t.len; i++)
			if (t.byte_char[i] >= from_ch) {
				from = i;
				break;
			}
		if (i == t.len)
			from = t.len;
		if (whole_sentence)
			from = rt_sentence_start(t.text, from);
		if (to_ch >= 0)
			for (i = t.len - 1; i >= from; i--)
				if (t.byte_char[i] >= 0 && t.byte_char[i] <= to_ch) {
					/* to the end of that character's bytes */
					while (i + 1 < t.len && t.byte_char[i + 1] == t.byte_char[i])
						i++;
					to = i + 1;
					break;
				}
	}
	c = sp.carry;
	memset(&sp.carry, 0, sizeof sp.carry);
	rc = rt_append(&c, &t, page, from, to);
	folio_page_text_free(&t);
	if (rc) {
		rt_free(&c);
		return -1;
	}
	if (!sp.selection_only) {
		int u = rt_unfinished(c.text, c.len);
		if (u < c.len)
			rt_split(&c, u, &sp.carry);
	}
	push_chunk(&c);
	return 0;
}

/* The character on the page at or after point p (reading order). */
static int char_at(int page, folio_point p, int last)
{
	folio_page_text t;
	folio_loc loc;
	folio_doc *doc = docwin_doc(sp.w);
	int i, best = last ? -1 : 0, rc;
	core_now();
	rc = folio_loc_from_number(doc, page, &loc) == 0 ? folio_get_page_text(doc, loc, &t) : -1;
	core_unlock();
	if (rc)
		return 0;
	for (i = 0; i < t.nchars; i++) {
		const folio_quad *q = &t.quads[i];
		float cy = (q->ul.y + q->ll.y) / 2, h = q->ll.y - q->ul.y;
		int same_line = cy > p.y - h / 2 && cy < p.y + h / 2;
		int after = same_line ? (q->ul.x + q->ur.x) / 2 >= p.x : q->ul.y > p.y;
		if (!last && (after || (same_line && p.x == 0))) {
			best = i;
			break;
		}
		if (last && (same_line ? (q->ul.x + q->ur.x) / 2 <= p.x : q->ll.y < p.y))
			best = i;
	}
	folio_page_text_free(&t);
	return best < 0 ? 0 : best;
}

/* ---- Reading ---------------------------------------------------------------- */

static void clear_reading(void)
{
	int i;
	inflight *f = sp.fly;
	while (f) {
		inflight *n = f->next;
		speaker_free(f->a);
		free(f);
		f = n;
	}
	sp.fly = sp.fly_tail = sp.shown = NULL;
	sp.shown_chunk = -1;
	rt_free(&sp.carry);
	for (i = 0; i < CHUNKS; i++)
		rt_free(&sp.chunk[i].c);
}

void speech_stop(void)
{
	docwin *w = sp.w;
	if (!w)
		return;
	speaker_stop();
	ssb_close();
	clear_reading();
	sp.w = NULL;
	sp.start_wait = 0;
	app.speech_urgent = 0;
	sp.duty = 0;
	sp.window_start = 0;
	docwin_set_spoken(w, NULL, NULL, 0);
	docwin_refresh_toolbar(w);
	app_log("speech: stopped");
}

static void begin_at(int page, folio_point a, folio_point b);

void speech_read(docwin *w, int selection_only)
{
	int page, sel;
	folio_point a, b;
	if (!sp.available)
		return;
	if (sp.w) {
		docwin *was = sp.w;
		speech_stop();
		if (was == w && !selection_only)
			return;           /* Read aloud again: stop */
	}
	if (!sp.sound_loaded) {
		char cmd[300];
		snprintf(cmd, sizeof cmd, "%.256s.LoadSound", sp.dir);
		if (object_type(cmd) == 1) {
			_kernel_oserror *e;
			snprintf(cmd, sizeof cmd, "Obey %.256s.LoadSound", sp.dir);
			if ((e = _swix(OS_CLI, _IN(0), cmd)) != NULL) {
				tbx_report_error(e);
				return;
			}
		}
		sp.sound_loaded = 1;
	}
	speaker_clock = now_cs;
	if (speaker_start(sp.voice, sp.data))
		return;
	sel = docwin_read_start(w, &page, &a, &b);
	if (selection_only && !sel)
		return;
	sp.w = w;
	sp.selection_only = selection_only;
	sp.text_done = 0;
	sp.loading_said = 0;
	sp.start_wait = 0;
	if (docwin_ocr_wait(w, page)) {
		/* A scan: its text comes first (speech_background starts then). */
		sp.start_wait = 1;
		sp.start_page = page;
		sp.start_a = a;
		sp.start_b = b;
		app_log("speech: waiting for the text on page %d to be recognised", page + 1);
		docwin_refresh_toolbar(w);
		return;
	}
	begin_at(page, a, b);
}

/* Starts reading from point a on the page (to b, for Speak selection). */
static void begin_at(int page, folio_point a, folio_point b)
{
	docwin *w = sp.w;
	int from, to = -1, selection_only = sp.selection_only;
	from = char_at(page, a, 0);
	if (selection_only)
		to = char_at(page, b, 1);
	app_log("speech: reading %s from page %d character %d%s", docwin_title(w), page + 1, from,
		selection_only ? " (the selection)" : "");
	/* From the start of the sentence the selection or the view begins in
	 * (just the selection, for Speak selection). */
	if (fetch_page(page, from, to, !selection_only)) {
		speech_stop();
		return;
	}
	sp.next_page = page + 1;
	if (selection_only)
		sp.text_done = 1;
	docwin_refresh_toolbar(w);
}

docwin *speech_window(void)
{
	return sp.w;
}

void speech_forget(docwin *w)
{
	if (sp.w == w) {
		speaker_stop();
		ssb_close();
		clear_reading();
		sp.w = NULL;
	}
}

void speech_set_speed(int speed)
{
	sp.speed = speed;
}

int speech_speed(void)
{
	return sp.speed;
}

/* The next page's text, when the thread has finished the last. */
static void fetch_more(void)
{
	if (sp.text_done || speaker_pending() > 0)
		return;
	if (sp.next_page >= docwin_pages(sp.w) && !docwin_is_counting(sp.w)) {
		/* The end: whatever was carried, then nothing more. */
		if (sp.carry.len) {
			rt_chunk c = sp.carry;
			memset(&sp.carry, 0, sizeof sp.carry);
			push_chunk(&c);
		}
		sp.text_done = 1;
		return;
	}
	/* A scanned page: wait for its text. */
	if (sp.next_page < docwin_pages(sp.w) && docwin_ocr_wait(sp.w, sp.next_page))
		return;
	if (sp.next_page < docwin_pages(sp.w) && fetch_page(sp.next_page, 0, -1, 0) == 0)
		sp.next_page++;
}

/* Sentences from the thread into the stream, a block at a time, keeping
 * about a second queued. */
static void feed(void)
{
	int rate = speaker_rate(), target = rate * 4 * QUEUE_SECONDS, guard;
	for (guard = 0; guard < 64; guard++) {
		inflight *f;
		int q = ssb_queued();
		if (q < 0 || q >= target)
			break;
		/* The sentence being sent: the last in flight with samples left. */
		f = sp.fly_tail;
		if (!f || f->pos >= f->a->n + f->pause) {
			sp_audio *a = speaker_take();
			if (!a) {
				/* Nothing more yet. At the very end, send the last part block. */
				if (sp.text_done && speaker_pending() == 0 && sp.block_fill > 0) {
					memset(sp.block + sp.block_fill * 2, 0,
						(BLOCK_FRAMES - sp.block_fill) * 4);
					add_block();
				}
				break;
			}
			f = calloc(1, sizeof *f);
			if (!f) {
				speaker_free(a);
				break;
			}
			/* What it cost: the time given to the thread while it was made
			 * (the clock time is longer: it's paced). */
			app_log("speech: %.2f s of speech made with %.2f s of the machine (x%.2f), over %.2f s",
				(double)a->n / rate, a->given / 100.0,
				a->n ? a->given / 100.0 / ((double)a->n / rate) : 0.0, a->cs / 100.0);
			f->a = a;
			/* Pauses: after a paragraph, a sentence, or a clause of a long
			 * sentence (made separately: speaker.c). */
			f->pause = rate * (a->paragraph ? 55 : a->clause ? 15 : 20) / 100;
			f->stream_start = sp.added + sp.block_fill * 4;
			f->stream_end = f->stream_start + (a->n + f->pause) * 4;
			if (sp.fly_tail)
				sp.fly_tail->next = f;
			else
				sp.fly = f;
			sp.fly_tail = f;
		}
		/* Fill the block from it. */
		while (sp.block_fill < BLOCK_FRAMES && f->pos < f->a->n + f->pause) {
			short v = f->pos < f->a->n ? f->a->pcm[f->pos] : 0;
			sp.block[2 * sp.block_fill] = sp.block[2 * sp.block_fill + 1] = v;
			sp.block_fill++;
			f->pos++;
		}
		if (sp.block_fill == BLOCK_FRAMES && add_block())
			break;
	}
}

static const folio_quad *quads_for(void *ctx, int page, int *n)
{
	/* The page's character quads, cached for the page last asked for. */
	static folio_page_text t;
	static int cached = -1;
	static docwin *cached_w;
	folio_loc loc;
	(void)ctx;
	if (page != cached || cached_w != sp.w) {
		folio_page_text_free(&t);
		cached = -1;
		core_now();
		if (folio_loc_from_number(docwin_doc(sp.w), page, &loc) == 0 &&
				folio_get_page_text(docwin_doc(sp.w), loc, &t) == 0) {
			cached = page;
			cached_w = sp.w;
		}
		core_unlock();
	}
	*n = cached == page ? t.nchars : 0;
	return cached == page ? t.quads : NULL;
}

/* The sentence now being heard: show it; forget the ones heard. */
static void follow(void)
{
	int played = ssb_played();
	inflight *f;
	if (played < 0)
		return;
	while ((f = sp.fly) != NULL && f != sp.fly_tail && played >= f->stream_end) {
		sp.fly = f->next;
		if (sp.shown == f)
			sp.shown = NULL;
		speaker_free(f->a);
		free(f);
	}
	for (f = sp.fly; f; f = f->next)
		if (played >= f->stream_start && played < f->stream_end && sp.shown != f) {
			/* The next part of the same sentence: already shown. */
			if (sp.shown_chunk == f->a->chunk && sp.shown_start == f->a->start &&
					sp.shown_end == f->a->end) {
				sp.shown = f;
				break;
			}
			rt_chunk *c = chunk_by_id(f->a->chunk);
			int pages[MAX_BOXES], n = 0;
			folio_quad boxes[MAX_BOXES];
			if (c)
				n = rt_boxes(c, f->a->start, f->a->end, quads_for, NULL, pages, boxes, MAX_BOXES);
			docwin_set_spoken(sp.w, pages, boxes, n);
			sp.shown = f;
			sp.shown_chunk = f->a->chunk;
			sp.shown_start = f->a->start;
			sp.shown_end = f->a->end;
			break;
		}
	speaker_set_unplayed((sp.added + sp.block_fill * 4 - played) / 4);
}

int speech_background(unsigned *wake)
{
	char err[200];
	int st;
	*wake = 0;
	if (!sp.w) {
		app.speech_urgent = 0;
		return 0;
	}
	st = speaker_state(err, sizeof err);
	if (st == SP_FAILED) {
		char msg[300];
		snprintf(msg, sizeof msg, "Folio can't read aloud: %s.", err);
		speech_stop();
		tbx_report(msg);
		return 0;
	}
	if (sp.start_wait) {
		if (docwin_ocr_wait(sp.w, sp.start_page)) {
			*wake = now_cs() + 10;
			return 0;
		}
		sp.start_wait = 0;
		begin_at(sp.start_page, sp.start_a, sp.start_b);
		if (!sp.w)
			return 0;
	}
	if (st == SP_LOADING) {
		if (!sp.loading_said++)
			app_log("speech: loading the voice");
	} else {
		fetch_more();
		if (!sp.open && ssb_open(speaker_rate())) {
			speech_stop();
			return 0;
		}
		feed();
		follow();
		/* All made, sent and heard: done. */
		if (sp.text_done && speaker_pending() == 0 && sp.block_fill == 0 && ssb_queued() == 0 &&
				(!sp.fly_tail || sp.fly_tail->pos >= sp.fly_tail->a->n + sp.fly_tail->pause)) {
			app_log("speech: finished");
			speech_stop();
			return 0;
		}
	}
	/* Time for the speech thread (it only runs while Folio has the machine). */
	if (speaker_busy() || st == SP_LOADING) {
		int ready = speaker_ready(), rate = speaker_rate();
		unsigned now = now_cs();
		float duty, span;
		if (sp.fly_tail && sp.fly_tail->pos < sp.fly_tail->a->n + sp.fly_tail->pause)
			ready += sp.fly_tail->a->n + sp.fly_tail->pause - sp.fly_tail->pos;
		/* Below URGENT_SECONDS the rest of Folio holds back its own
		 * background work (docwin.c), so the voice has the machine. */
		app.speech_urgent = st == SP_READY && ready < URGENT_SECONDS * rate;
		span = (float)(SPEAKER_AHEAD_SECONDS - BEHIND_SECONDS) * rate;
		duty = st != SP_READY ? 1.0f : (SPEAKER_AHEAD_SECONDS * (float)rate - ready) / span;
		if (duty > 1.0f)
			duty = 1.0f;
		if (duty < MIN_DUTY)
			duty = MIN_DUTY;
		if ((int)(now - sp.next_turn) >= 0) {
			/* Smoothed (about 2 s), since sound becomes ready a whole
			 * sentence at a time; all of it at once when behind. */
			sp.duty = duty >= 1.0f || sp.duty <= 0 ? duty : sp.duty + 0.05f * (duty - sp.duty);
			duty = sp.duty;
			unsigned slice = (unsigned)(duty * PERIOD_CS + 0.5f), t0 = now;
			if (slice < 1)
				slice = 1;
			do
				pthread_yield();
			while (speaker_busy() && now_cs() - t0 < slice);
			sp.given += now_cs() - t0;
			speaker_given += now_cs() - t0;
			/* The next turn a period after this one started; if it ran
			 * late (other tasks), not before a fair gap. */
			sp.next_turn = t0 + PERIOD_CS;
			if ((int)(sp.next_turn - now_cs()) < (int)(PERIOD_CS - slice))
				sp.next_turn = now_cs() + (PERIOD_CS - slice);
		}
		/* Every 10 s: its share of the machine, and how far ahead it is. */
		if (now - sp.window_start >= 1000) {
			if (sp.window_start)
				app_log("speech: %d%% of the time to the voice over %u s; %.1f s ready",
					(int)(100 * sp.given / (now - sp.window_start)), (now - sp.window_start) / 100,
					(double)ready / rate);
			sp.window_start = now;
			sp.given = 0;
		}
		if (sp.duty >= 0.99f)
			return 1;           /* null events: it's behind */
		*wake = sp.next_turn;
		return 0;
	}
	app.speech_urgent = 0;     /* ahead, and resting */
	*wake = now_cs() + 10;
	return 0;
}

void speech_quit(void)
{
	if (sp.w) {
		speaker_stop();
		ssb_close();
		clear_reading();
		sp.w = NULL;
	}
	speaker_quit();
}
