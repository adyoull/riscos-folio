#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
# Builds expat 2.2.9 and fontconfig 2.14.1 as static libraries for RISC OS
# (riscos-crossdev) and ARM Linux (the qemu test twin), against MuPDF's
# own FreeType, so the program has one FreeType (DESIGN.md §6).
#   fontconfig/build.sh [riscos|linuxarm|all]      (default all)
# The sources are the Debian tarballs that GCCSDK's autobuilder (and so
# PackMan's FontConfig 2.14.1) uses, in fontconfig/dl/ (or DL=dir):
#   fontconfig_2.14.1.orig.tar.xz  298e883f6e11d2c5e6d53c8a8394de58d563902cfab934e6be12fb5a5f361ef0
#   expat_2.2.9.orig.tar.gz        c341ac8c79e021cc3392a6d76e138e62d1dd287592cb455148540331756a2208
# Missing ones are downloaded (Debian's and Ubuntu's archives) and checked.
# Needs: gperf, MuPDF built (mupdf/build.sh), the riscos-crossdev toolchain
# (RISCOS_TOOLCHAIN) and gcc-arm-linux-gnueabihf.
# Output: fontconfig/out/{ro,al}/{include,lib}: libfontconfig.a, libexpat.a
# and libftextra.a (the few FreeType parts fontconfig uses that MuPDF leaves
# out, compiled from MuPDF's FreeType with MuPDF's settings).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
DL=${DL:-$HERE/dl}
WHAT=${1:-all}
RISCOS_TOOLCHAIN=${RISCOS_TOOLCHAIN:-${RCD_DIR:-/opt/riscos-crossdev}/toolchain/env}
export PATH=$RISCOS_TOOLCHAIN/bin:$PATH
JOBS=${JOBS:-$(nproc)}
MU=$HERE/../mupdf/src
SRC=$HERE/src

check() {   # file sha256 url...
	local f=$1 sum=$2 u
	shift 2
	if [ ! -f "$DL/$f" ]; then
		mkdir -p "$DL"
		for u in "$@"; do
			if curl -fsSL -o "$DL/$f.part" "$u" &&
			   echo "$sum  $DL/$f.part" | sha256sum -c --quiet - 2>/dev/null; then
				mv "$DL/$f.part" "$DL/$f"
				break
			fi
			echo "build.sh: $f from $u: no, or not the expected file" >&2
		done
		rm -f "$DL/$f.part"
	fi
	echo "$sum  $DL/$f" | sha256sum -c --quiet - || { echo "build.sh: $DL/$f is missing or changed" >&2; exit 1; }
}
check fontconfig_2.14.1.orig.tar.xz 298e883f6e11d2c5e6d53c8a8394de58d563902cfab934e6be12fb5a5f361ef0 \
	https://deb.debian.org/debian/pool/main/f/fontconfig/fontconfig_2.14.1.orig.tar.xz \
	https://snapshot.debian.org/archive/debian/20230612T000000Z/pool/main/f/fontconfig/fontconfig_2.14.1.orig.tar.xz \
	https://www.freedesktop.org/software/fontconfig/release/fontconfig-2.14.1.tar.xz
check expat_2.2.9.orig.tar.gz c341ac8c79e021cc3392a6d76e138e62d1dd287592cb455148540331756a2208 \
	http://archive.ubuntu.com/ubuntu/pool/main/e/expat/expat_2.2.9.orig.tar.gz

# MuPDF's FreeType settings (Makelists).
FT_CFLAGS="-I$MU/thirdparty/freetype/include -I$MU/scripts/freetype"
FT_BUILD="-DFT_CONFIG_MODULES_H=\"slimftmodules.h\" -DFT_CONFIG_OPTIONS_H=\"slimftoptions.h\" -DFT2_BUILD_LIBRARY"
# FreeType parts fontconfig calls that MuPDF's build doesn't compile.
FT_EXTRA="ftbdf.c ftmm.c fttype1.c"

build() {   # name cc ar host cflags mupdf-build-dir
	local name=$1 cc=$2 ar=$3 host=$4 cflags=$5 mubuild=$6
	local out=$HERE/out/$name b=$SRC/$name
	# No build paths in the program (assert() messages name their file).
	cflags="$cflags -ffile-prefix-map=$HERE/=fontconfig/ -DNDEBUG"
	rm -rf "$b"
	mkdir -p "$b" "$out/include" "$out/lib"

	# FreeType extras.
	for f in $FT_EXTRA; do
		$cc $cflags $FT_CFLAGS $FT_BUILD -c "$MU/thirdparty/freetype/src/base/$f" -o "$b/ft-${f%.c}.o"
	done
	rm -f "$out/lib/libftextra.a"
	$ar rcs "$out/lib/libftextra.a" "$b"/ft-*.o
	cp -r "$MU/thirdparty/freetype/include/." "$out/include/"
	cp "$MU/scripts/freetype/"*.h "$out/include/"

	# expat: its three sources, with a fixed configuration.
	tar xzf "$DL/expat_2.2.9.orig.tar.gz" -C "$b"
	local ex=$b/libexpat-R_2_2_9/expat/lib
	cat > "$ex/expat_config.h" <<'CFG'
#define BYTEORDER 1234
#define HAVE_MEMMOVE 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define HAVE_UNISTD_H 1
#define HAVE_FCNTL_H 1
#define XML_NS 1
#define XML_DTD 1
#define XML_CONTEXT_BYTES 1024
/* No getrandom()/arc4random(): the hash salt comes from the time, which is
 * enough for reading font configuration files. */
#define XML_POOR_ENTROPY 1
CFG
	for f in xmlparse xmlrole xmltok; do
		$cc $cflags -DHAVE_EXPAT_CONFIG_H -I"$ex" -c "$ex/$f.c" -o "$b/expat-$f.o"
	done
	rm -f "$out/lib/libexpat.a"
	$ar rcs "$out/lib/libexpat.a" "$b"/expat-*.o
	cp "$ex/expat.h" "$ex/expat_external.h" "$out/include/"

	# fontconfig, configured for the host with the patch applied.
	tar xJf "$DL/fontconfig_2.14.1.orig.tar.xz" -C "$b"
	local fc=$b/fontconfig-2.14.1
	(cd "$fc" && patch -s -p1 < "$HERE/patches/fontconfig-2.14.1-riscos.diff")
	for f in config.sub config.guess; do
		cp "$(ls /usr/share/automake-*/$f | tail -1)" "$fc/$f"
	done
	(cd "$fc" && \
	 CC=$cc AR=$ar RANLIB=${ar%ar}ranlib CFLAGS="$cflags" \
	 FREETYPE_CFLAGS="$FT_CFLAGS" \
	 FREETYPE_LIBS="$out/lib/libftextra.a $MU/build/$mubuild/libmupdf-third.a -lm" \
	 EXPAT_CFLAGS="-I$out/include" EXPAT_LIBS="$out/lib/libexpat.a" \
	 ./configure --host=$host --build=$(gcc -dumpmachine) --disable-shared --enable-static \
		--disable-docs --disable-nls --disable-cache-build --disable-iconv \
		--with-expat-includes="$out/include" --with-expat-lib="$out/lib" \
		--sysconfdir=/FontConfig --localstatedir=/FontConfig/var --datadir=/FontConfig/share \
		--with-default-fonts=/Fonts > "$b/fontconfig-configure.log" 2>&1) || {
		tail -30 "$b/fontconfig-configure.log"; exit 1; }
	make -C "$fc/src" -j"$JOBS" > "$b/fontconfig-make.log" 2>&1 || {
		tail -30 "$b/fontconfig-make.log"; exit 1; }
	cp "$fc/src/.libs/libfontconfig.a" "$out/lib/"
	mkdir -p "$out/include/fontconfig"
	cp "$fc/fontconfig/"*.h "$out/include/fontconfig/"
	echo "fontconfig/build.sh: $name done ($(ls "$out/lib"))"
}

if [ "$WHAT" = all ] || [ "$WHAT" = riscos ]; then
	build ro arm-riscos-gnueabihf-gcc arm-riscos-gnueabihf-ar arm-riscos-gnueabihf \
		"-O2 -mfpu=vfpv3 -mfloat-abi=hard -fstack-clash-protection -D_FILE_OFFSET_BITS=64" \
		ro-release-tesseract-tofu_cjk
fi
if [ "$WHAT" = all ] || [ "$WHAT" = linuxarm ]; then
	build al arm-linux-gnueabihf-gcc arm-linux-gnueabihf-ar arm-linux-gnueabihf \
		"-O2 -marm -mcpu=cortex-a8 -mfpu=vfpv3 -mfloat-abi=hard -mno-unaligned-access -fstack-clash-protection" \
		armlinux-release-tesseract-tofu_cjk
fi
