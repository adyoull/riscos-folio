/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* The speech thread (see speaker.h). eSpeak NG is used only here (it keeps
 * global state); the voice too. The main thread hands over chunks of text
 * and takes back sentences of audio; a generation number throws away
 * anything made before speaker_stop(). */
#include "speaker.h"
#include "pvoice.h"
#include "pv_espeak.h"
#include "pv_math.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* How far ahead of the listener it works: until SPEAKER_AHEAD_SECONDS of
 * sound isn't yet heard (and at most AHEAD_SENTENCES sentences waiting, to
 * bound memory). How fast it gets there is the main thread's choice: it
 * gives the thread time a little at a time (speech.c), so the work is
 * spread out rather than done in bursts. */
#define AHEAD_SENTENCES 16

typedef struct sp_chunk {
	struct sp_chunk *next;
	int id, gen;
	char *text;
	int len;
	float length_scale;
} sp_chunk;

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_t thread;
static int started, quitting, state = SP_OFF, busy, gen, rate = 48000, up_factor = 1;
static volatile int stop_now;
static char voice_path[256], espeak_dir[256], error[200];
static sp_chunk *in_head, *in_tail;
static sp_audio *out_head, *out_tail;
static int out_samples, out_count, unplayed, working_chunks, waiting_room;

/* Enough made for now? (with mu held) */
static int ahead_enough(void)
{
	return out_count >= AHEAD_SENTENCES || out_samples + unplayed >= SPEAKER_AHEAD_SECONDS * rate;
}
static pv_voice *voice;

static unsigned no_clock(void) { return 0; }
unsigned (*speaker_clock)(void) = no_clock;

static void free_chunk(sp_chunk *c)
{
	free(c->text);
	free(c);
}

/* Whether a paragraph ends between byte 'at' and the next text. */
static int paragraph_after(const char *t, int len, int at)
{
	int nl = 0, i;
	/* The sentence's span may already include the break (eSpeak reads it
	 * as part of the clause). */
	for (i = at - 1; i >= 0 && (t[i] == '\n' || t[i] == ' ' || t[i] == '\t' || t[i] == '\r'); i--)
		if (t[i] == '\n' && ++nl == 2)
			return 1;
	for (; at < len; at++) {
		if (t[at] == '\n') {
			if (++nl == 2)
				return 1;
		} else if (t[at] != ' ' && t[at] != '\t' && t[at] != '\r') {
			return 0;
		}
	}
	return 1;
}

static void speak_chunk(sp_chunk *c, int mygen)
{
	pv_sentence *s;
	pv_options o;
	int n = pv_phonemize(c->text, &s), i;
	if (n < 0)
		return;
	pv_default_options(voice, &o);
	o.length_scale *= c->length_scale;
	o.stop = &stop_now;
	for (i = 0; i < n; i++) {
		int ids[4096], k, ns = 0;
		unsigned t0;
		float *f, peak = 0.01f, gain;
		sp_audio *a;
		/* Not too far ahead of the listener. */
		pthread_mutex_lock(&mu);
		while (!quitting && gen == mygen && ahead_enough()) {
			busy = 0;          /* waiting isn't work: no need to give it time */
			waiting_room = 1;
			pthread_cond_wait(&cv, &mu);
		}
		waiting_room = 0;
		busy = 1;
		if (quitting || gen != mygen) {
			pthread_mutex_unlock(&mu);
			break;
		}
		pthread_mutex_unlock(&mu);

		k = pv_phonemes_to_ids(voice, s[i].ph, s[i].n, ids, 4096);
		t0 = speaker_clock();
		f = pv_synthesize(voice, ids, k, &o, &ns, NULL);
		if (!f)
			continue;           /* stopped, or out of memory */
		/* Up to a rate the sound system plays as it is (16 kHz x 3 = 48 kHz)
		 * with a proper filter: its own rate conversion left a tinny edge. */
		if (up_factor > 1) {
			int nu = 0;
			float *u = pv_upsample(f, ns, up_factor, &nu);
			if (u) {
				free(f);
				f = u;
				ns = nu;
			}
		}
		a = calloc(1, sizeof *a);
		if (a)
			a->pcm = malloc((ns ? ns : 1) * sizeof *a->pcm);
		if (!a || !a->pcm) {
			free(a);
			free(f);
			continue;
		}
		/* To 16 bits as Piper does: the sentence's peak to near full scale. */
		for (k = 0; k < ns; k++)
			if (fabsf(f[k]) > peak)
				peak = fabsf(f[k]);
		gain = 32767.0f * 0.9f / peak;
		for (k = 0; k < ns; k++)
			a->pcm[k] = (short)lrintf(f[k] * gain);
		free(f);
		a->n = ns;
		a->cs = (int)(speaker_clock() - t0);
		a->chunk = c->id;
		a->start = s[i].start;
		a->end = s[i].end;
		a->paragraph = paragraph_after(c->text, c->len, s[i].end);
		pthread_mutex_lock(&mu);
		if (gen == mygen && !quitting) {
			if (out_tail)
				out_tail->next = a;
			else
				out_head = a;
			out_tail = a;
			out_samples += ns;
			out_count++;
			a = NULL;
		}
		pthread_mutex_unlock(&mu);
		speaker_free(a);
	}
	pv_sentences_free(s, n);
}

static void *main_loop(void *arg)
{
	char err[200];
	(void)arg;
	err[0] = 0;
	voice = pv_load(voice_path, err, sizeof err);
	if (voice && pv_espeak_init(espeak_dir, pv_espeak_voice(voice), err, sizeof err)) {
		pv_free(voice);
		voice = NULL;
	}
	pthread_mutex_lock(&mu);
	if (voice) {
		state = SP_READY;
		up_factor = pv_upsample_factor(pv_sample_rate(voice));
		rate = pv_sample_rate(voice) * up_factor;
	} else {
		state = SP_FAILED;
		snprintf(error, sizeof error, "%s", err[0] ? err : "the voice couldn't be loaded");
	}
	pthread_cond_broadcast(&cv);
	while (!quitting && voice) {
		sp_chunk *c;
		int mygen;
		if (!in_head) {
			busy = 0;
			pthread_cond_wait(&cv, &mu);
			continue;
		}
		c = in_head;
		in_head = c->next;
		if (!in_head)
			in_tail = NULL;
		if (c->gen != gen) {
			working_chunks--;
			free_chunk(c);
			continue;
		}
		mygen = gen;
		stop_now = 0;
		busy = 1;
		pthread_mutex_unlock(&mu);
		speak_chunk(c, mygen);
		free_chunk(c);
		pthread_mutex_lock(&mu);
		working_chunks--;
		busy = in_head != NULL;
	}
	busy = 0;
	pthread_mutex_unlock(&mu);
	return NULL;
}

int speaker_start(const char *vp, const char *ed)
{
	pthread_attr_t attr;
	int rc;
	if (started)
		return 0;
	snprintf(voice_path, sizeof voice_path, "%s", vp);
	snprintf(espeak_dir, sizeof espeak_dir, "%s", ed);
	state = SP_LOADING;
	busy = 1;
	pthread_attr_init(&attr);
	/* eSpeak's clause translation has large local buffers. */
	pthread_attr_setstacksize(&attr, 1024 * 1024);
	rc = pthread_create(&thread, &attr, main_loop, NULL);
	pthread_attr_destroy(&attr);
	if (rc) {
		state = SP_FAILED;
		busy = 0;
		snprintf(error, sizeof error, "no thread for speech");
		return -1;
	}
	started = 1;
	return 0;
}

int speaker_state(char *err, size_t err_size)
{
	int s;
	pthread_mutex_lock(&mu);
	s = state;
	if (s == SP_FAILED && err)
		snprintf(err, err_size, "%s", error);
	pthread_mutex_unlock(&mu);
	return s;
}

int speaker_rate(void)
{
	return rate;
}

int speaker_push(int chunk, const char *text, int len, float length_scale)
{
	sp_chunk *c = calloc(1, sizeof *c);
	if (!c)
		return -1;
	c->text = malloc(len + 1);
	if (!c->text) {
		free(c);
		return -1;
	}
	memcpy(c->text, text, len);
	c->text[len] = 0;
	c->len = len;
	c->id = chunk;
	c->length_scale = length_scale > 0 ? length_scale : 1;
	pthread_mutex_lock(&mu);
	c->gen = gen;
	if (in_tail)
		in_tail->next = c;
	else
		in_head = c;
	in_tail = c;
	working_chunks++;
	busy = 1;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
	return 0;
}

int speaker_pending(void)
{
	int n;
	pthread_mutex_lock(&mu);
	n = working_chunks;
	pthread_mutex_unlock(&mu);
	return n;
}

sp_audio *speaker_take(void)
{
	sp_audio *a;
	pthread_mutex_lock(&mu);
	a = out_head;
	if (a) {
		out_head = a->next;
		if (!out_head)
			out_tail = NULL;
		out_samples -= a->n;
		out_count--;
		if (waiting_room && !ahead_enough())
			busy = 1;
		a->next = NULL;
		pthread_cond_broadcast(&cv);
	}
	pthread_mutex_unlock(&mu);
	return a;
}

void speaker_free(sp_audio *a)
{
	if (a) {
		free(a->pcm);
		free(a);
	}
}

void speaker_set_unplayed(int samples)
{
	pthread_mutex_lock(&mu);
	if (samples != unplayed) {
		unplayed = samples;
		/* Room again: it has work, so the main thread should give it time
		 * (it only runs while Folio has the machine). */
		if (waiting_room && !ahead_enough())
			busy = 1;
		pthread_cond_broadcast(&cv);
	}
	pthread_mutex_unlock(&mu);
}

int speaker_ready(void)
{
	int n;
	pthread_mutex_lock(&mu);
	n = out_samples + unplayed;
	pthread_mutex_unlock(&mu);
	return n;
}

void speaker_stop(void)
{
	sp_audio *a;
	sp_chunk *c;
	pthread_mutex_lock(&mu);
	gen++;
	stop_now = 1;
	while ((c = in_head) != NULL) {
		in_head = c->next;
		working_chunks--;
		free_chunk(c);
	}
	in_tail = NULL;
	a = out_head;
	out_head = out_tail = NULL;
	out_samples = 0;
	out_count = 0;
	unplayed = 0;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
	while (a) {
		sp_audio *next = a->next;
		speaker_free(a);
		a = next;
	}
}

int speaker_busy(void)
{
	return busy && state != SP_FAILED;
}

void speaker_quit(void)
{
	if (!started)
		return;
	speaker_stop();
	pthread_mutex_lock(&mu);
	quitting = 1;
	stop_now = 1;
	pthread_cond_broadcast(&cv);
	pthread_mutex_unlock(&mu);
	pthread_join(thread, NULL);
	pv_free(voice);
	voice = NULL;
	started = 0;
}
