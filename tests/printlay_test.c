/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Placing pages on paper (wimp/printlay.c). */
#include "printlay.h"
#include <stdio.h>
#include <stdlib.h>

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
	printf(__VA_ARGS__); printf("\n"); } } while (0)
#define NEAR(a, b, tol) (abs((a) - (b)) <= (tol))

int main(void)
{
	/* A4 paper with 1/4 inch margins (18 pt = 18000 mp). */
	int a4[4] = { 18000, 18000, 595000 - 18000, 842000 - 18000 };
	print_place p;

	/* An A5 page at 100%: real size (420 x 595 pt), centred. */
	CHECK(print_layout(420, 595, a4, 0, 100, 300, 64u << 20, &p) == 0, "A5 lays out");
	CHECK(p.w_os == 1050 && p.h_os == 1487, "A5 real size in OS units (%d x %d)", p.w_os, p.h_os);
	CHECK(NEAR(p.pos[0], (595000 - 420000) / 2, 400) && NEAR(p.pos[1], (842000 - 595000) / 2, 400),
		"A5 centred (%d, %d)", p.pos[0], p.pos[1]);
	CHECK(p.px_w == 1750 && NEAR(p.px_h, 2479, 1), "A5 at 300 dpi (%d x %d)", p.px_w, p.px_h);
	CHECK(p.matrix[0] == 0x10000 && p.matrix[3] == 0x10000 && !p.matrix[1] && !p.matrix[2], "upright matrix");

	/* An A4 page doesn't fit inside the margins: made smaller, filling the height. */
	CHECK(print_layout(595, 842, a4, 0, 100, 300, 64u << 20, &p) == 0, "A4 lays out");
	CHECK(NEAR(p.w_os * 400, 595000 - 36000, 400), "A4 fills the width (%d)", p.w_os);
	CHECK(p.h_os * 400 <= 842000 - 36000, "and fits the height (%d)", p.h_os);
	CHECK(p.pos[1] >= 18000 && p.pos[0] >= 18000, "inside the margins (%d, %d)", p.pos[0], p.pos[1]);

	/* 50%: half size. */
	CHECK(print_layout(420, 595, a4, 0, 50, 300, 64u << 20, &p) == 0 && p.w_os == 525, "50%% (%d)", p.w_os);

	/* Sideways: an A4 landscape page (842 x 595) on A4 paper turned. */
	CHECK(print_layout(842, 595, a4, 1, 100, 300, 64u << 20, &p) == 0, "sideways lays out");
	CHECK(p.matrix[0] == 0 && p.matrix[1] == 0x10000 && p.matrix[2] == -0x10000 && p.matrix[3] == 0,
		"quarter-turn matrix");
	CHECK(NEAR(p.h_os * 400, 595000 - 36000, 400) && p.w_os * 400 <= 842000 - 36000,
		"its height fills the paper's width (%d x %d)", p.w_os, p.h_os);
	/* Its bottom left lands at the right; the page covers x pos-h .. pos. */
	CHECK(p.pos[0] <= 595000 - 18000 && p.pos[0] - p.h_os * 400 >= 18000 - 400,
		"sideways inside the margins across (%d - %d)", p.pos[0] - p.h_os * 400, p.pos[0]);
	CHECK(p.pos[1] >= 18000 - 400 && p.pos[1] + p.w_os * 400 <= 842000 - 18000 + 400,
		"and up (%d - %d)", p.pos[1], p.pos[1] + p.w_os * 400);

	/* Too many pixels: the resolution drops until they fit. */
	CHECK(print_layout(420, 595, a4, 0, 100, 600, 8u << 20, &p) == 0 &&
		(size_t)p.px_w * p.px_h * 4 <= 8u << 20 && p.px_w > 500, "fits 8 MB (%d x %d)", p.px_w, p.px_h);
	CHECK(p.zoom > 0 && NEAR((int)(p.zoom * 420 + 0.5f), p.px_w, 1), "zoom matches (%g)", p.zoom);

	CHECK(print_layout(0, 595, a4, 0, 100, 300, 64u << 20, &p) == -1, "an empty page");
	printf("%d checks, %d failed\n", checks, fails);
	return fails != 0;
}
