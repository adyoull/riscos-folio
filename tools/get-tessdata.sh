#!/bin/bash
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
# Fetches Tesseract's English data (Apache-2.0) for the OCR plugin and the
# tests: the standard set and the fast set, into DIR/standard and DIR/fast.
#   tools/get-tessdata.sh [DIR]      (default ../tessdata)
set -eu
DIR=${1:-../tessdata}
BASE=https://raw.githubusercontent.com/tesseract-ocr
mkdir -p "$DIR/standard" "$DIR/fast"
curl -fsSL -o "$DIR/standard/eng.traineddata" $BASE/tessdata/main/eng.traineddata
curl -fsSL -o "$DIR/standard/LICENSE" $BASE/tessdata/main/LICENSE
curl -fsSL -o "$DIR/fast/eng.traineddata" $BASE/tessdata_fast/main/eng.traineddata
curl -fsSL -o "$DIR/fast/LICENSE" $BASE/tessdata_fast/main/LICENSE
sha256sum "$DIR"/*/eng.traineddata
