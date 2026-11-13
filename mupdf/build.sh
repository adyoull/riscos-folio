#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
# Fetches MuPDF 1.28.5, applies our patches and builds the static libraries
# for RISC OS (riscos-crossdev) and for ARM Linux (the qemu test twin).
#   mupdf/build.sh [riscos|linuxarm|all|riscpc]   (default all; riscpc only on its own)
# Needs: git, the riscos-crossdev toolchain on PATH (or RISCOS_TOOLCHAIN),
#        gcc/g++-arm-linux-gnueabihf for linuxarm.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/src
TAG=1.28.5
WHAT=${1:-all}
RISCOS_TOOLCHAIN=${RISCOS_TOOLCHAIN:-${RCD_DIR:-/opt/riscos-crossdev}/toolchain/env}
export PATH=$RISCOS_TOOLCHAIN/bin:$PATH
JOBS=${JOBS:-$(nproc)}

if [ ! -d "$SRC" ]; then
	git clone -q --depth 1 --branch $TAG --recurse-submodules --shallow-submodules \
		https://github.com/ArtifexSoftware/mupdf "$SRC"
	(cd "$SRC" && git -c user.name=build -c user.email=build@localhost am -q "$HERE"/patches/*.patch)
fi
cd "$SRC"
[ -d generated/resources/fonts ] || make -j"$JOBS" generate

# No X11/GL/curl/crypto/threads or barcodes. CJK fonts stay out of the
# program (the font pack supplies them, DESIGN.md §6). Tesseract (OCR) is the
# bundled copy; its NEON kernels get -mfpu=neon and are used only when
# getauxval() reports NEON (on RISC OS the program supplies getauxval and
# riscos-include/ stands in for the Linux headers).
COMMON="build=release HAVE_X11=no HAVE_GLUT=no HAVE_OBJCOPY=no HAVE_LIBCRYPTO=no
	HAVE_PTHREAD=no HAVE_CURL=no tesseract=yes barcode=no tofu_cjk=yes
	HAVE_NEON=yes TESSERACT_NEON_CFLAGS=-mfpu=neon"

if [ "$WHAT" = all ] || [ "$WHAT" = riscos ]; then
	P=arm-riscos-gnueabihf-
	# riscos-crossdev's flags (docs/RISCOS-RULES.md); LDFLAGS replaces MuPDF's
	# -Wl,-s so the checks can see symbols.
	make -j"$JOBS" OS=riscos build_prefix=ro- $COMMON \
		CC=${P}gcc CXX=${P}g++ AR=${P}ar LD=${P}ld RANLIB=${P}ranlib \
		XCFLAGS="-O2 -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -D_FILE_OFFSET_BITS=64 -DHAVE_GETAUXVAL -I$HERE/riscos-include" \
		LDFLAGS="-static" libs
fi
# The RiscPC build (mupdf/build.sh riscpc; not part of "all"): GCCSDK GCC
# 4.7.4 (arm-unknown-riscos: APCS-32, soft float, UnixLib) for a StrongARM,
# from RPC_TOOLCHAIN. No Tesseract, no extract, and no Noto fonts beyond
# the base 14 (fontconfig finds the computer's fonts for other scripts), to
# keep the program small. MuPDF's HarfBuzz needs a newer C++ than GCC 4.7,
# so HarfBuzz 1.9.0 is used instead, with MuPDF's fzhb_ renaming and its
# allocator, built with byte structure rounding (APCS rounds structures to
# 4 bytes; HarfBuzz checks its font table layouts). MuPDF uses only
# HarfBuzz's long-standing API (buffers, hb_shape, hb_ft_font_create).
if [ "$WHAT" = riscpc ]; then
	RPC_TOOLCHAIN=${RPC_TOOLCHAIN:-$HOME/rpc/cross}
	P=$RPC_TOOLCHAIN/bin/arm-unknown-riscos-
	HB=$HERE/harfbuzz-1.9.0 OUT=build/rpc-release-tofu-tofu_cjk
	[ -d "$HB" ] || git clone -q --depth 1 --branch 1.9.0 https://github.com/harfbuzz/harfbuzz "$HB"
	RPC_FLAGS="-O2 -march=armv4 -mtune=strongarm"
	make -C "$HB/src" -f "$HERE/harfbuzz-rpc.mk" -j"$JOBS" OUT="$SRC/$OUT/hb19" \
		CC=${P}gcc CXX=${P}g++ AR=${P}ar \
		FLAGS="$RPC_FLAGS -mstructure-size-boundary=8 -w -I. -Ihb-ucdn -I$SRC/include/mupdf \
		-I$SRC/thirdparty/freetype/include -I$SRC/scripts/freetype -DHAVE_FALLBACK=1 -DHAVE_FREETYPE \
		-DHAVE_OT -DHAVE_ROUND -DHAVE_UCDN -DHB_NO_MT -DHB_NO_PRAGMA_GCC_DIAGNOSTIC \
		-Dhb_malloc_impl=fz_hb_malloc -Dhb_calloc_impl=fz_hb_calloc -Dhb_free_impl=fz_hb_free \
		-Dhb_realloc_impl=fz_hb_realloc -include $SRC/thirdparty/harfbuzz/src/hb-rename.h"
	make -j"$JOBS" OS=riscos build_prefix=rpc- build=release HAVE_X11=no HAVE_GLUT=no HAVE_OBJCOPY=no \
		HAVE_LIBCRYPTO=no HAVE_PTHREAD=no HAVE_CURL=no tesseract=no barcode=no extract=no tofu=yes tofu_cjk=yes \
		USE_SYSTEM_HARFBUZZ=yes SYS_HARFBUZZ_LIBS= \
		SYS_HARFBUZZ_CFLAGS="-I$HB/src -include $SRC/thirdparty/harfbuzz/src/hb-rename.h" \
		CC=${P}gcc CXX=${P}g++ AR=${P}ar LD=${P}ld RANLIB=${P}ranlib \
		XCFLAGS="$RPC_FLAGS -std=gnu99 -D_FILE_OFFSET_BITS=64" LDFLAGS="-static" libs
	cp "$OUT/hb19/libfzhb.a" "$OUT/libfzhb.a"
fi
if [ "$WHAT" = all ] || [ "$WHAT" = linuxarm ]; then
	P=arm-linux-gnueabihf-
	make -j"$JOBS" OS=Linux build_prefix=armlinux- $COMMON shared=no \
		CC=${P}gcc CXX=${P}g++ AR=${P}ar LD=${P}ld RANLIB=${P}ranlib \
		XCFLAGS="-O2 -marm -mcpu=cortex-a8 -mfpu=vfpv3 -mfloat-abi=hard -mno-unaligned-access -fstack-clash-protection -DHAVE_GETAUXVAL" \
		XLDFLAGS="-no-pie" libs apps
fi
