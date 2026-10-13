/* SPDX-License-Identifier: MIT
 * Copyright (C) 2026 Andrew Youll
 * The Linux ARM hwcap bit for NEON, for Tesseract's SIMD detection on
 * RISC OS (see sys/auxv.h). */
#ifndef FOLIO_ASM_HWCAP_H
#define FOLIO_ASM_HWCAP_H
#define HWCAP_NEON (1 << 12)
#endif
