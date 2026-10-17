#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
# Fetches the riscos-pvoice devkit (the speech engine's headers, libpvoice.a
# for RISC OS and ARM Linux, eSpeak NG's English data and the voice) for the
# release .riscos-deps names, checks it against tools/pvoice.sha256, and
# unpacks it as deps/riscos-pvoice-VERSION (the Makefile's PVOICE_DIR).
#   tools/get-pvoice.sh
# PVOICE_TARBALL=file uses a devkit already downloaded (still checked).
# ci/riscos-deps-hook.sh updates tools/pvoice.sha256 when the pin moves.
set -eu
cd "$(dirname "$0")/.."
TAG=$(awk '$1 == "riscos-pvoice" || $1 == "adyoull/riscos-pvoice" {print $2}' .riscos-deps)
[ -n "$TAG" ] || { echo "get-pvoice.sh: .riscos-deps has no riscos-pvoice line" >&2; exit 1; }
V=${TAG#v}
NAME=riscos-pvoice-$V-devkit.tar.gz
OUT=deps/riscos-pvoice-$V
SUM=$(awk -v n="$NAME" '$2 == n {print $1}' tools/pvoice.sha256)
[ -n "$SUM" ] || { echo "get-pvoice.sh: tools/pvoice.sha256 has no line for $NAME" >&2; exit 1; }
if [ -f "$OUT/.sha256" ] && [ "$(cat "$OUT/.sha256")" = "$SUM" ]; then
	echo "get-pvoice.sh: $OUT is up to date"
	exit 0
fi
mkdir -p deps
T=deps/$NAME.part
if [ -n "${PVOICE_TARBALL:-}" ]; then
	cp "$PVOICE_TARBALL" "$T"
else
	curl -fsSL -o "$T" "https://github.com/adyoull/riscos-pvoice/releases/download/$TAG/$NAME"
fi
echo "$SUM  $T" | sha256sum -c --quiet - || {
	echo "get-pvoice.sh: $NAME isn't the one tools/pvoice.sha256 names" >&2; rm -f "$T"; exit 1; }
rm -rf "$OUT"
tar -xzf "$T" -C deps
rm -f "$T"
[ -f "$OUT/build/ro/libpvoice.a" ] || { echo "get-pvoice.sh: $NAME has no riscos-pvoice-$V/" >&2; exit 1; }
echo "$SUM" > "$OUT/.sha256"
echo "get-pvoice.sh: riscos-pvoice $TAG in $OUT"
