/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* Placing a page on the paper (see printlay.h). */
#include "printlay.h"
#include <math.h>

/* 1 point = 1000 millipoints; 1 OS unit = 400 millipoints (1/180 inch). */
#define MP_PER_PT 1000.0
#define MP_PER_OS 400.0

int print_layout(float pw, float ph, const int printable[4], int sideways, int scale_pc,
		int dpi, size_t max_bytes, print_place *out)
{
	double across = printable[2] - printable[0], up = printable[3] - printable[1];
	double avail_w = sideways ? up : across;    /* along the page's own x and y */
	double avail_h = sideways ? across : up;
	double fit, wmp, hmp, x0, y0;
	if (pw <= 0 || ph <= 0 || avail_w <= 0 || avail_h <= 0)
		return -1;
	if (scale_pc <= 0)
		scale_pc = 100;
	fit = scale_pc / 100.0;
	if (fit * pw * MP_PER_PT > avail_w)
		fit = avail_w / (pw * MP_PER_PT);
	if (fit * ph * MP_PER_PT > avail_h)
		fit = avail_h / (ph * MP_PER_PT);
	wmp = pw * MP_PER_PT * fit;
	hmp = ph * MP_PER_PT * fit;
	out->w_os = (int)floor(wmp / MP_PER_OS);
	out->h_os = (int)floor(hmp / MP_PER_OS);
	if (out->w_os < 1 || out->h_os < 1)
		return -1;
	/* So the rectangle is exactly what's drawn: */
	wmp = out->w_os * MP_PER_OS;
	hmp = out->h_os * MP_PER_OS;
	if (dpi < 36)
		dpi = 36;
	for (;;) {
		out->px_w = (int)ceil(wmp / 72000.0 * dpi);
		out->px_h = (int)ceil(hmp / 72000.0 * dpi);
		if ((size_t)out->px_w * out->px_h * 4 <= max_bytes || dpi <= 36)
			break;
		dpi = dpi * 3 / 4;
	}
	out->zoom = (float)out->px_w / pw;
	if (!sideways) {
		out->matrix[0] = 0x10000; out->matrix[1] = 0;
		out->matrix[2] = 0;       out->matrix[3] = 0x10000;
		x0 = printable[0] + (across - wmp) / 2;
		y0 = printable[1] + (up - hmp) / 2;
		out->pos[0] = (int)x0;
		out->pos[1] = (int)y0;
	} else {
		/* (x, y) -> (-y, x): the page's x runs up the paper and its y
		 * leftwards, so its bottom left is at the right of where it lands. */
		out->matrix[0] = 0;        out->matrix[1] = 0x10000;
		out->matrix[2] = -0x10000; out->matrix[3] = 0;
		x0 = printable[0] + (across - hmp) / 2 + hmp;
		y0 = printable[1] + (up - wmp) / 2;
		out->pos[0] = (int)x0;
		out->pos[1] = (int)y0;
	}
	return 0;
}
