#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
# Runs tests/escl_test against the pretend scanner tests/fake-escl.py.
#   tests/escl-test.sh "RUNNER" build/al/escl_test
# RUNNER runs an ARM Linux program (qemu/aligntrap.sh), or is empty.
set -eu
RUN=$1 PROG=$2
OUT=tests/out/escl
rm -rf "$OUT"
mkdir -p "$OUT"
python3 -I tests/fake-escl.py "$OUT" &
FAKE=$!
trap 'touch "$OUT/stop"; wait $FAKE 2>/dev/null || true' EXIT
i=0
while [ ! -f "$OUT/ports" ] && [ $i -lt 100 ]; do
	sleep 0.1
	i=$((i + 1))
done
$RUN "$PROG" "$OUT"
