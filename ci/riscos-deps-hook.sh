#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
# Run by .github/workflows/riscos-deps-update.yml when a release named in
# .riscos-deps is newer than its pin:
#   ci/riscos-deps-hook.sh NAME OLD_TAG NEW_TAG
# riscos-pvoice: the new devkit's sha256, from that release's SHA256SUMS,
# goes into tools/pvoice.sha256 (tools/get-pvoice.sh checks against it).
set -eu
NAME=$1 NEW=$3
case $NAME in
riscos-pvoice|adyoull/riscos-pvoice)
	V=${NEW#v}
	F=riscos-pvoice-$V-devkit.tar.gz
	SUMS=$(curl -fsSL "https://github.com/adyoull/riscos-pvoice/releases/download/$NEW/SHA256SUMS")
	LINE=$(printf '%s\n' "$SUMS" | awk -v f="$F" '$2 == f || $2 == "*" f {print $1 "  " f}')
	[ -n "$LINE" ] || { echo "riscos-deps-hook.sh: the $NEW release's SHA256SUMS has no $F" >&2; exit 1; }
	echo "$LINE" > tools/pvoice.sha256
	echo "tools/pvoice.sha256: $LINE"
	;;
esac
