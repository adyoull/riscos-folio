/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Test stand-in for RISC OS swis.h: only what worker.c uses (the clock). */
#ifndef STUB_SWIS_H
#define STUB_SWIS_H
#include <time.h>
#define OS_ReadMonotonicTime 0x42
#define _OUT(i) (1u << (31 - (i)))
static inline void *_swix(int swi, unsigned mask, unsigned *out)
{
	struct timespec ts;
	(void)swi; (void)mask;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	*out = (unsigned)(ts.tv_sec * 100 + ts.tv_nsec / 10000000);
	return 0;
}
#endif
