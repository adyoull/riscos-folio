/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll
 * UnixLib has no getauxval(). Tesseract's SIMD detection uses it to find
 * NEON, so the program supplies one (wimp/ocr.c) that reports what the
 * machine has. Only used when building MuPDF for RISC OS. */
#ifndef FOLIO_SYS_AUXV_H
#define FOLIO_SYS_AUXV_H
#define AT_HWCAP 16
#ifdef __cplusplus
extern "C" {
#endif
unsigned long getauxval(unsigned long type);
#ifdef __cplusplus
}
#endif
#endif
