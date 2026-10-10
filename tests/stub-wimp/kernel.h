/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll */
/* Test stand-in for RISC OS kernel.h. */
#ifndef STUB_KERNEL_H
#define STUB_KERNEL_H
typedef struct { int errnum; char errmess[252]; } _kernel_oserror;
#endif
