/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* The speech thread (wimp/speaker.c) with a real voice, under qemu:
 *   speaker_test VOICE ESPEAK_DATA
 * Two chunks become sentences of audio in order, with their spans; a stop
 * throws away what's queued; a bad voice fails cleanly. */
#include "speaker.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)

static sp_audio *wait_audio(int seconds)
{
	int i;
	for (i = 0; i < seconds * 10; i++) {
		sp_audio *a = speaker_take();
		if (a)
			return a;
		usleep(100000);
	}
	return NULL;
}

int main(int argc, char **argv)
{
	const char *t1 = "Hello there. Short one!\n\nNew paragraph.";
	const char *t2 = "Second chunk.";
	sp_audio *a;
	char err[200];
	int i, st;
	if (argc < 3) {
		fprintf(stderr, "usage: speaker_test VOICE ESPEAK_DATA\n");
		return 2;
	}
	CHECK(speaker_start(argv[1], argv[2]) == 0, "start");
	for (i = 0; i < 600 && (st = speaker_state(err, sizeof err)) == SP_LOADING; i++)
		usleep(100000);
	CHECK(st == SP_READY, "state %d: %s", st, st == SP_FAILED ? err : "");
	if (st != SP_READY)
		return 1;
	CHECK(speaker_rate() == 48000, "rate %d (16 kHz voice, up-sampled x3)", speaker_rate());

	speaker_push(1, t1, (int)strlen(t1), 1.0f);
	speaker_push(2, t2, (int)strlen(t2), 1.0f);
	CHECK(speaker_pending() == 2 || speaker_pending() == 1, "pending %d", speaker_pending());
	{
		static const char *want[] = { "Hello there.", "Short one!", "New paragraph.", "Second chunk." };
		static const int chunk[] = { 1, 1, 1, 2 }, para[] = { 0, 1, 1, 1 };
		for (i = 0; i < 4; i++) {
			const char *src = chunk[i] == 1 ? t1 : t2;
			a = wait_audio(300);
			CHECK(a != NULL, "sentence %d missing", i + 1);
			if (!a)
				break;
			printf("chunk %d [%.*s] %d samples%s\n", a->chunk, a->end - a->start, src + a->start, a->n,
				a->paragraph ? ", paragraph" : "");
			CHECK(a->chunk == chunk[i] && !strncmp(src + a->start, want[i], strlen(want[i])) &&
				a->n > 4000 && a->paragraph == para[i], "sentence %d", i + 1);
			speaker_free(a);
		}
	}
	CHECK(speaker_pending() == 0, "pending after %d", speaker_pending());

	/* It works three sentences ahead, then waits (not busy) until one is taken. */
	{
		const char *t5 = "One. Two. Three. Four. Five.";
		int got = 0;
		speaker_push(5, t5, (int)strlen(t5), 1.0f);
		for (i = 0; i < 3000 && (speaker_busy() || speaker_pending() == 0); i++)
			usleep(100000);
		usleep(300000);
		CHECK(!speaker_busy(), "still busy when three ahead");
		while ((a = speaker_take()) != NULL) {
			got++;
			speaker_free(a);
			if (got == 3)
				break;
		}
		CHECK(got == 3, "%d sentences made ahead (want 3)", got);
		/* Taking them made room: the last two follow. */
		for (i = 0; i < 2; i++) {
			a = wait_audio(300);
			CHECK(a && a->chunk == 5, "sentence %d after making room", 4 + i);
			speaker_free(a);
		}
	}

	/* A stop throws away what's queued. */
	speaker_push(3, "This will be stopped. So will this.", 35, 1.0f);
	speaker_stop();
	usleep(500000);
	for (i = 0; i < 30 && speaker_busy(); i++)
		usleep(100000);
	a = speaker_take();
	CHECK(a == NULL, "audio after a stop (chunk %d)", a ? a->chunk : 0);
	speaker_free(a);
	/* And it speaks again afterwards. */
	speaker_push(4, "Again.", 6, 1.2f);
	a = wait_audio(300);
	CHECK(a && a->chunk == 4, "after the stop");
	speaker_free(a);
	speaker_quit();
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
