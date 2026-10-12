/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Where a page goes on the paper, and at what resolution it's drawn
 * (print.c). No RISC OS calls: tested by tests/printlay_test.c. */
#ifndef PRINTLAY_H
#define PRINTLAY_H

#include <stddef.h>

typedef struct {
	int w_os, h_os;         /* the page as a rectangle of user OS units */
	int px_w, px_h;         /* drawn this many pixels */
	float zoom;             /* pixels per point */
	int matrix[4];          /* PDriver_GiveRectangle's matrix (16.16) */
	int pos[2];             /* and the rectangle's bottom left, millipoints */
} print_place;

/* A page pw x ph points, on paper whose printable area is
 * printable[] = left, bottom, right, top (millipoints). scale_pc: 100 is the
 * page's real size; it's made smaller if it wouldn't fit. Centred.
 * Sideways turns it a quarter turn anticlockwise. dpi: how finely to draw,
 * lowered if the pixels would take more than max_bytes. 0, or -1 if the
 * page or the paper is empty. */
int print_layout(float pw, float ph, const int printable[4], int sideways, int scale_pc,
		int dpi, size_t max_bytes, print_place *out);

#endif
