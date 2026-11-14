#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
"""Fails if a RiscPC program has halfword loads or stores (LDRH, STRH, LDRSH).

The StrongARM has them, but the Risc PC's memory system has no 16-bit
transfers: on a real machine they read and write the wrong data (RPCEmu
does them properly, so it doesn't show). The RiscPC build is compiled for
ARMv3M, which has none; this checks nothing slipped through.
  tools/check-halfword.py OBJDUMP PROGRAM.elf
"""
import re
import subprocess
import sys

objdump, elf = sys.argv[1], sys.argv[2]
out = subprocess.run([objdump, "-d", elf], capture_output=True, text=True, check=True).stdout
fn, bad = None, {}
for line in out.splitlines():
    m = re.match(r"^[0-9a-f]+ <(.*)>:", line)
    if m:
        fn = m.group(1)
        continue
    m = re.match(r"\s+[0-9a-f]+:\t([0-9a-f]{8})\s+(\S+)", line)
    # (Jump tables and literal pools come out as .word.)
    if not m or not m.group(2).startswith(("ldr", "str")):
        continue
    i = int(m.group(1), 16)
    # Extra load/store encoding: bits 27-25 000, bit 7 and bit 4 set, SH != 00.
    if (i >> 28) != 0xF and (i & 0x0E000090) == 0x90 and (i >> 5) & 3:
        sh, load = (i >> 5) & 3, (i >> 20) & 1
        if (sh, load) in ((1, 0), (1, 1), (3, 1)):
            bad[fn] = bad.get(fn, 0) + 1
if bad:
    worst = sorted(bad.items(), key=lambda x: -x[1])[:10]
    print(f"{elf}: {sum(bad.values())} halfword transfers, in {len(bad)} functions, e.g.",
          ", ".join(f"{f} ({n})" for f, n in worst), file=sys.stderr)
    sys.exit(1)
print(f"{elf}: no halfword transfers")
