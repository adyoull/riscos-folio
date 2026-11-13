/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* The RiscPC build has no plugins: no reading aloud (it needs NEON) and no
 * text recognition (far too slow on a StrongARM). This file stands in for
 * speech.c, speaker.c, readtext.c and ocrui.c there. Folio never looks for
 * a Plugins directory, so the menus and toolbar are as they are without the
 * plugins on other machines: no speaker button, no Read aloud, no Text
 * recognition. */
#include "app.h"

int speech_init(void) { return 0; }
int speech_available(void) { return 0; }
void speech_read(docwin *w, int selection_only) { (void)w; (void)selection_only; }
void speech_stop(void) { }
docwin *speech_window(void) { return NULL; }
void speech_forget(docwin *w) { (void)w; }
int speech_background(unsigned *wake) { if (wake) *wake = 0; return 0; }
void speech_set_speed(int speed) { (void)speed; }
int speech_speed(void) { return 0; }
void speech_quit(void) { }

int ocr_plugin_init(void) { return 0; }
int ocr_available(void) { return 0; }
int ocr_has_accurate(void) { return 0; }
int ocr_has_fast(void) { return 0; }
ocr_win *ocr_win_new(docwin *w) { (void)w; return NULL; }
void ocr_win_close(ocr_win *o) { (void)o; }
void ocr_win_before_reopen(ocr_win *o) { (void)o; }
void ocr_win_after_reopen(ocr_win *o) { (void)o; }
int ocr_win_next(ocr_win *o) { (void)o; return 0; }
void ocr_win_take(ocr_win *o, const render_job *r) { (void)o; (void)r; }
int ocr_win_running(ocr_win *o) { (void)o; return 0; }
int ocr_win_wait(ocr_win *o, int page) { (void)o; (void)page; return 0; }
void ocr_win_this_page(ocr_win *o) { (void)o; }
void ocr_win_all(ocr_win *o) { (void)o; }
int ocr_win_all_on(ocr_win *o) { (void)o; return 0; }
void ocr_win_resync(ocr_win *o) { (void)o; }
void ocr_settings_changed(ocr_win *o) { (void)o; }
