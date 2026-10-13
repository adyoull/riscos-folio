/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* The speech thread (speaker.c): loads a Piper voice (pvoice) and eSpeak NG,
 * then turns chunks of text into sentences of audio, a few seconds ahead of
 * what's playing. No RISC OS calls: tested on ARM Linux under qemu
 * (tests/speaker_test.c). */
#ifndef SPEAKER_H
#define SPEAKER_H

#include <stddef.h>

/* One sentence's audio: 16-bit mono at speaker_rate(). */
typedef struct sp_audio {
	struct sp_audio *next;
	int chunk;              /* the chunk's id */
	int start, end;         /* the sentence's bytes in the chunk */
	int paragraph;          /* a paragraph ends here (a longer pause after) */
	int clause;             /* part of a long sentence, more to come (a short pause) */
	short *pcm;
	int n;
	int cs;                 /* how long making it took (centiseconds) */
} sp_audio;

enum { SP_OFF, SP_LOADING, SP_READY, SP_FAILED };

/* The clock the thread times sentences with (centiseconds); set it first. */
extern unsigned (*speaker_clock)(void);

/* Starts the thread, which loads the voice and eSpeak's data. 0 or -1. */
int speaker_start(const char *voice_path, const char *espeak_dir);
/* SP_LOADING, SP_READY or SP_FAILED (with the message). */
int speaker_state(char *err, size_t err_size);
int speaker_rate(void);
/* Queues a chunk of text (copied). length_scale: 1 normal, > 1 slower. */
int speaker_push(int chunk, const char *text, int len, float length_scale);
/* Chunks queued or being spoken. */
int speaker_pending(void);
/* The next sentence's audio, or NULL. Free it with speaker_free. */
sp_audio *speaker_take(void);
void speaker_free(sp_audio *a);
/* How much is waiting to be heard (samples handed to the sound system but
 * not yet played); the thread keeps at most a few seconds ahead. */
void speaker_set_unplayed(int samples);
/* Sound made but not yet heard (samples): what's queued here plus what the
 * sound system still has. */
int speaker_ready(void);
/* The thread stops making sentences once this much sound is waiting to be
 * heard (made, queued or playing), and starts again below it. */
#define SPEAKER_AHEAD_SECONDS 12
/* Forgets everything queued and stops the sentence being made, at once. */
void speaker_stop(void);
/* The thread is working (worth giving it time). */
int speaker_busy(void);
void speaker_quit(void);

#endif
