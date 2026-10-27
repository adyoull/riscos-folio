#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
# Builds mbedTLS 3.6.7 (Apache-2.0) for checking and making PDF signatures:
# crypto/out/{ro,al}/lib/libmbedfolio.a and crypto/out/include. Only what
# crypto/folio-mbedtls-config.h turns on (no TLS, networking or PSA). The
# source is the v3.6.7 tag, checked by its commit.
#   crypto/build.sh          (RISCOS_TOOLCHAIN, or RCD_DIR, as for MuPDF)
set -eu
cd "$(dirname "$0")"
HERE=$(pwd)
RISCOS_TOOLCHAIN=${RISCOS_TOOLCHAIN:-${RCD_DIR:-/opt/riscos-crossdev}/toolchain/env}
export PATH=$RISCOS_TOOLCHAIN/bin:$PATH
TAG=v3.6.7
COMMIT=068ff080b369adfac81509f9b57b2afabaf82dc5
SRC=$HERE/src/mbedtls-$TAG

if [ ! -f "$SRC/library/x509_crt.c" ]; then
	rm -rf "$SRC"
	mkdir -p "$HERE/src"
	git clone --quiet --depth 1 --branch "$TAG" https://github.com/Mbed-TLS/mbedtls.git "$SRC"
fi
got=$(git -C "$SRC" rev-parse HEAD)
if [ "$got" != "$COMMIT" ]; then
	echo "crypto/build.sh: mbedTLS $TAG is $got, not $COMMIT" >&2
	exit 1
fi

# The library's sources (the ones whose features are off compile to nothing).
LIBSRC=$(ls "$SRC"/library/*.c)
CFG="-I$HERE -DMBEDTLS_CONFIG_FILE=\"folio-mbedtls-config.h\""

build() {   # target, cc, ar, cflags
	T=$1 CC=$2 AR=$3 FLAGS=$4
	O=$HERE/out/$T
	rm -rf "$O/obj" && mkdir -p "$O/obj" "$O/lib"
	for f in $LIBSRC; do
		$CC $FLAGS -ffile-prefix-map="$SRC"=mbedtls -I"$SRC/include" $CFG -c "$f" -o "$O/obj/$(basename "$f" .c).o"
	done
	rm -f "$O/lib/libmbedfolio.a"
	$AR rcs "$O/lib/libmbedfolio.a" "$O"/obj/*.o
	rm -rf "$O/obj"
	echo "crypto/build.sh: $O/lib/libmbedfolio.a"
}

build ro arm-riscos-gnueabihf-gcc arm-riscos-gnueabihf-ar \
	"-O2 -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection"
build al arm-linux-gnueabihf-gcc arm-linux-gnueabihf-ar \
	"-O2 -g -marm -mcpu=cortex-a8 -mfpu=vfpv3 -mfloat-abi=hard -mno-unaligned-access -fstack-clash-protection"
rm -rf "$HERE/out/include" && mkdir -p "$HERE/out/include"
cp -r "$SRC/include/mbedtls" "$SRC/include/psa" "$HERE/out/include/"
cp "$HERE/folio-mbedtls-config.h" "$HERE/out/include/"
cp "$SRC/LICENSE" "$HERE/out/LICENSE"
