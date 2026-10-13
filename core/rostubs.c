/* SPDX-License-Identifier: AGPL-3.0-or-later
 * Copyright (C) 2026 Andrew Youll */
/* What MuPDF's OCR (Tesseract and Leptonica) needs from the C library that
 * UnixLib doesn't have. Linked into RISC OS programs only. */
#include <errno.h>
#include <stdio.h>

/* Leptonica uses these only for reading and writing images in memory, which
 * MuPDF never asks of it. */
FILE *open_memstream(char **buf, size_t *size)
{
	(void)buf; (void)size;
	errno = ENOSYS;
	return NULL;
}

FILE *fmemopen(void *buf, size_t size, const char *mode)
{
	(void)buf; (void)size; (void)mode;
	errno = ENOSYS;
	return NULL;
}

/* Tesseract asks for the CPU's features (Linux AT_HWCAP) to choose its NEON
 * code. Every ARM that runs this hard-float build of RISC OS has NEON
 * (Cortex-A7, A8, A9, A15, A53, A72); Folio's speech code relies on it too. */
#define AT_HWCAP 16
#define HWCAP_NEON (1 << 12)
unsigned long getauxval(unsigned long type)
{
	return type == AT_HWCAP ? HWCAP_NEON : 0;
}
