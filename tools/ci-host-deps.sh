#!/bin/sh
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
# The Linux packages make ci needs beyond the riscos-crossdev image's:
# g++-arm-linux-gnueabihf, for the ARM Linux test build of MuPDF's
# Tesseract (C++). If it's missing and this runs as root (GitHub's build
# job), it's installed with apt-get; otherwise this says what's missing.
set -eu
need=
command -v arm-linux-gnueabihf-g++ >/dev/null 2>&1 || need="$need g++-arm-linux-gnueabihf"
[ -n "$need" ] || exit 0
if [ "$(id -u)" = 0 ] && command -v apt-get >/dev/null 2>&1; then
	echo "ci-host-deps.sh: installing$need"
	export DEBIAN_FRONTEND=noninteractive
	apt-get update -qq
	apt-get install -y -qq --no-install-recommends $need >/dev/null
	exit 0
fi
echo "ci-host-deps.sh: needs$need (install it, or run as root to have it installed)" >&2
exit 1
