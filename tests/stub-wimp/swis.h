/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Test stand-in for RISC OS swis.h, for front-end code run on Linux: the
 * test supplies _swix (tests/find_test.c). */
#ifndef STUB_SWIS_H
#define STUB_SWIS_H
#include "kernel.h"
#define OS_ReadMonotonicTime 0x42
#define Wimp_GetWindowState  0x400cb
#define _IN(i)  (1u << (i))
#define _INR(a, b) (((~0u) << (a)) & ~((~0u) << ((b) + 1)))
#define _OUT(i) (1u << (31 - (i)))
#define _OUTR(a, b) 0
_kernel_oserror *_swix(int swi, unsigned mask, ...);
#endif
