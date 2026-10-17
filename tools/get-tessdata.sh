#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
# Fetches Tesseract's English data (Apache-2.0) for the OCR plugin and the
# tests: the standard set and the fast set, into DIR/standard and DIR/fast.
#   tools/get-tessdata.sh [DIR]      (default deps/tessdata)
# Taken at fixed commits of tesseract-ocr/tessdata and tessdata_fast and
# checked by sha256; files already there and right are left alone.
set -eu
DIR=${1:-deps/tessdata}
RAW=https://raw.githubusercontent.com/tesseract-ocr
STD=$RAW/tessdata/ced78752cc61322fb554c280d13360b35b8684e4
FAST=$RAW/tessdata_fast/87416418657359cb625c412a48b6e1d6d41c29bd
LIC=cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30

get() {   # url file sha256
	if [ -f "$2" ] && echo "$3  $2" | sha256sum -c --quiet - 2>/dev/null; then
		return
	fi
	curl -fsSL -o "$2.part" "$1"
	echo "$3  $2.part" | sha256sum -c --quiet - || {
		echo "get-tessdata.sh: $1 isn't the expected file" >&2; rm -f "$2.part"; exit 1; }
	mv "$2.part" "$2"
}
mkdir -p "$DIR/standard" "$DIR/fast"
get $STD/eng.traineddata "$DIR/standard/eng.traineddata" daa0c97d651c19fba3b25e81317cd697e9908c8208090c94c3905381c23fc047
get $STD/LICENSE "$DIR/standard/LICENSE" $LIC
get $FAST/eng.traineddata "$DIR/fast/eng.traineddata" 7d4322bd2a7749724879683fc3912cb542f19906c83bcc1a52132556427170b2
get $FAST/LICENSE "$DIR/fast/LICENSE" $LIC
echo "get-tessdata.sh: $DIR/standard and $DIR/fast checked"
