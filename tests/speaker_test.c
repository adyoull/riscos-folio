/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* The speech thread (wimp/speaker.c) with a real voice, under qemu:
 *   speaker_test VOICE ESPEAK_DATA
 * Two chunks become sentences of audio in order, with their spans; a stop
 * throws away what's queued; a bad voice fails cleanly. */
#include "speaker.h"
#include <stdio.h>
#include <stdlib.h>
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

	/* It works until SPEAKER_AHEAD_SECONDS of sound is waiting to be heard,
	 * then waits (not busy) until there's room. */
	{
		const char *t5 = "One. Two. Three. Four. Five.";
		int got = 0;
		speaker_set_unplayed(SPEAKER_AHEAD_SECONDS * speaker_rate() - speaker_rate() / 10);
		speaker_push(5, t5, (int)strlen(t5), 1.0f);
		for (i = 0; i < 3000 && (speaker_busy() || speaker_pending() == 0); i++)
			usleep(100000);
		usleep(300000);
		CHECK(!speaker_busy(), "still busy when far enough ahead");
		while ((a = speaker_take()) != NULL) {
			got++;
			speaker_free(a);
		}
		CHECK(got >= 1 && got <= 2, "%d sentences made ahead (want 1 or 2)", got);
		/* The sound was heard: room again, and the rest follow. */
		speaker_set_unplayed(0);
		for (i = got; i < 5; i++) {
			a = wait_audio(300);
			CHECK(a && a->chunk == 5, "sentence %d after making room", i + 1);
			speaker_free(a);
		}
	}

	/* A long sentence comes a clause at a time: the same sentence span, a
	 * clause flag on all but the last, and no long silences inside. */
	{
		const char *t6 = "When the evening came, and the lamps were lit along the harbour wall, "
			"she walked slowly down to the water, wondering whether the ship would ever return.";
		int parts = 0, ok = 1, total = 0, last_clause = 1;
		speaker_push(6, t6, (int)strlen(t6), 1.0f);
		while ((a = wait_audio(300)) != NULL) {
			int k, run = 0, longest = 0;
			parts++;
			total += a->n;
			if (a->chunk != 6 || a->start != 0 || a->end < (int)strlen(t6) - 1)
				ok = 0;
			for (k = 0; k < a->n; k++) {
				run = abs(a->pcm[k]) < 300 ? run + 1 : 0;
				if (run > longest)
					longest = run;
			}
			printf("part %d: %.2f s, clause %d, longest quiet %.2f s\n", parts,
				(double)a->n / speaker_rate(), a->clause, (double)longest / speaker_rate());
			if (a->clause && longest > speaker_rate() * 6 / 10)
				ok = 0;
			last_clause = a->clause;
			speaker_free(a);
			if (!last_clause)
				break;
		}
		CHECK(parts >= 2 && parts <= 4, "%d parts", parts);
		CHECK(ok && !last_clause, "parts: spans, clause flags and silences");
		CHECK(total < speaker_rate() * 8, "%.2f s in all (the whole sentence is about 6.5 s)",
			(double)total / speaker_rate());
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
