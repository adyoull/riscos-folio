#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
# Fetches MuPDF 1.28.5, applies our patches and builds the static libraries
# for RISC OS (riscos-crossdev 1.4) and for ARM Linux (the qemu test twin).
#   mupdf/build.sh [riscos|linuxarm|all]      (default all)
# Needs: git, the riscos-crossdev toolchain on PATH (or RISCOS_TOOLCHAIN),
#        gcc/g++-arm-linux-gnueabihf for linuxarm.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
SRC=$HERE/src
TAG=1.28.5
WHAT=${1:-all}
RISCOS_TOOLCHAIN=${RISCOS_TOOLCHAIN:-/opt/xd/riscos-crossdev-toolchain-1.4-x86_64-linux}
export PATH=$RISCOS_TOOLCHAIN/bin:$PATH
JOBS=${JOBS:-$(nproc)}

if [ ! -d "$SRC" ]; then
	git clone -q --depth 1 --branch $TAG --recurse-submodules --shallow-submodules \
		https://github.com/ArtifexSoftware/mupdf "$SRC"
	(cd "$SRC" && git -c user.name=build -c user.email=build@localhost am -q "$HERE"/patches/*.patch)
fi
cd "$SRC"
[ -d generated/resources/fonts ] || make -j"$JOBS" generate

# No X11/GL/curl/crypto/threads; no Tesseract or barcodes. CJK fonts stay out
# of the program (the font pack supplies them, DESIGN.md §6).
COMMON="build=release HAVE_X11=no HAVE_GLUT=no HAVE_OBJCOPY=no HAVE_LIBCRYPTO=no
	HAVE_PTHREAD=no HAVE_CURL=no tesseract=no barcode=no tofu_cjk=yes"

if [ "$WHAT" = all ] || [ "$WHAT" = riscos ]; then
	P=arm-riscos-gnueabihf-
	# riscos-crossdev's flags (docs/RISCOS-RULES.md); LDFLAGS replaces MuPDF's
	# -Wl,-s so the checks can see symbols.
	make -j"$JOBS" OS=riscos build_prefix=ro- $COMMON \
		CC=${P}gcc CXX=${P}g++ AR=${P}ar LD=${P}ld RANLIB=${P}ranlib \
		XCFLAGS="-O2 -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -D_FILE_OFFSET_BITS=64" \
		LDFLAGS="-static" libs
fi
if [ "$WHAT" = all ] || [ "$WHAT" = linuxarm ]; then
	P=arm-linux-gnueabihf-
	make -j"$JOBS" OS=Linux build_prefix=armlinux- $COMMON shared=no \
		CC=${P}gcc CXX=${P}g++ AR=${P}ar LD=${P}ld RANLIB=${P}ranlib \
		XCFLAGS="-O2 -marm -mcpu=cortex-a8 -mfpu=vfpv3 -mfloat-abi=hard -mno-unaligned-access -fstack-clash-protection" \
		XLDFLAGS="-no-pie" libs apps
fi
