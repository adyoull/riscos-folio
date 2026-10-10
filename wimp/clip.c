/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Folio's side of the RISC OS global clipboard (PRM p1003-1008):
 *  - Copy: keep the text, broadcast Message_ClaimEntity (bit 2).
 *  - Another task claims the clipboard: drop ours.
 *  - Message_DataRequest (bit 2) while we hold it: reply Message_DataSave
 *    (Text, &FFF); on Message_DataSaveAck write the file it names and send
 *    Message_DataLoad.
 * Text is converted from MuPDF's UTF-8 to the desktop's alphabet (Latin-1
 * unless the alphabet is UTF-8). */
#include "app.h"
#include <stdlib.h>
#include <string.h>
#include <swis.h>

enum { Message_ClaimEntity = 0xf, Message_DataRequest = 0x10 };

static char *clip_text;      /* in the desktop's alphabet */
static size_t clip_len;
static int save_ref;          /* my_ref of our DataSave */

/* Is the desktop's alphabet UTF-8? OS_Byte 71, R1 127 reads it (111 = UTF8). */
static int alphabet_utf8(void)
{
	int a = 0;
	_swix(OS_Byte, _INR(0, 1) | _OUT(1), 71, 127, &a);
	return a == 111;
}

/* UTF-8 to Latin-1, with common punctuation spelt out. */
static char *to_latin1(const char *s, size_t *len)
{
	char *out = malloc(strlen(s) * 3 + 1), *o = out;
	const unsigned char *p = (const unsigned char *)s;
	if (!out)
		return NULL;
	while (*p) {
		unsigned c = *p++;
		if (c >= 0x80) {
			int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
			c &= extra == 3 ? 0x07 : extra == 2 ? 0x0f : 0x1f;
			while (extra-- > 0 && (*p & 0xc0) == 0x80)
				c = (c << 6) | (*p++ & 0x3f);
		}
		switch (c) {
		case 0x2018: case 0x2019: case 0x201a: *o++ = '\''; break;
		case 0x201c: case 0x201d: case 0x201e: *o++ = '"'; break;
		case 0x2013: case 0x2014: *o++ = '-'; break;
		case 0x2026: memcpy(o, "...", 3); o += 3; break;
		case 0xfb01: memcpy(o, "fi", 2); o += 2; break;
		case 0xfb02: memcpy(o, "fl", 2); o += 2; break;
		default:
			*o++ = c < 0x100 ? (char)c : '?';
		}
	}
	*o = 0;
	*len = (size_t)(o - out);
	return out;
}

void clip_set_text(const char *utf8)
{
	int b[6];
	free(clip_text);
	clip_text = NULL;
	if (alphabet_utf8()) {
		clip_len = strlen(utf8);
		clip_text = malloc(clip_len + 1);
		if (clip_text)
			memcpy(clip_text, utf8, clip_len + 1);
	} else {
		clip_text = to_latin1(utf8, &clip_len);
	}
	if (!clip_text)
		return;
	b[0] = 24;
	b[3] = 0;
	b[4] = Message_ClaimEntity;
	b[5] = 4;   /* the clipboard */
	_swix(Wimp_SendMessage, _INR(0, 2), Wimp_EUserMessage, b, 0);
	app_log("clipboard: %d bytes", (int)clip_len);
}

/* Returns 1 if the message was a clipboard one. */
int clip_message(int *b)
{
	switch (b[4]) {
	case Message_ClaimEntity:
		if (b[1] != app.task && (b[5] & 4)) {
			free(clip_text);
			clip_text = NULL;
		}
		return 1;
	case Message_DataRequest:
		if (clip_text && (b[9] & 4)) {
			/* Reply DataSave to the requester with its window, icon and
			 * position, as Text. */
			int r[64];
			memset(r, 0, sizeof r);
			r[3] = b[2];
			r[4] = Message_DataSave;
			r[5] = b[5];
			r[6] = b[6];
			r[7] = b[7];
			r[8] = b[8];
			r[9] = (int)clip_len;
			r[10] = 0xfff;
			strcpy((char *)(r + 11), "Selection");
			r[0] = (44 + 10 + 3) & ~3;
			_swix(Wimp_SendMessage, _INR(0, 2), Wimp_EUserMessage, r, b[1]);
			save_ref = r[2];
		}
		return clip_text != NULL;
	case Message_DataSaveAck:
		if (clip_text && save_ref && b[3] == save_ref) {
			const char *path = (const char *)(b + 11);
			FILE *f = fopen(path, "wb");
			save_ref = 0;
			if (!f)
				return 1;
			fwrite(clip_text, 1, clip_len, f);
			fclose(f);
			_swix(OS_File, _INR(0, 2), 18, path, 0xfff);
			b[3] = b[2];
			b[4] = Message_DataLoad;
			b[9] = (int)clip_len;
			_swix(Wimp_SendMessage, _INR(0, 2), Wimp_EUserMessageRecorded, b, b[1]);
			return 1;
		}
		return 0;
	}
	return 0;
}
