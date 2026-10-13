#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
"""Keeps only the parts of a Tesseract .traineddata file that the LSTM
(neural network) recogniser uses, as Tesseract's combine_tessdata -e would:

    tools/strip-tessdata.py eng.traineddata out.traineddata

The standard tessdata files carry the old Tesseract 3 engine too (18 MB of
the 23.5 MB English file). MuPDF runs Tesseract in its LSTM mode, so that
part is never used: stripped, the file gives the same results at a fifth of
the size. The file: an int32 count, that many int64 offsets (-1 = absent),
then the parts, in the order of Tesseract's TessdataType."""
import struct
import sys

NAMES = ["config", "unicharset", "ambigs", "inttemp", "pffmtable", "normproto",
         "punc-dawg", "word-dawg", "number-dawg", "freq-dawg", "fixed-length-dawgs",
         "cube-unicharset", "cube-word-dawg", "shapetable", "bigram-dawg",
         "unambig-dawg", "params-model", "lstm", "lstm-punc-dawg", "lstm-word-dawg",
         "lstm-number-dawg", "lstm-unicharset", "lstm-recoder", "version"]
KEEP = {"config", "lstm", "lstm-punc-dawg", "lstm-word-dawg", "lstm-number-dawg",
        "lstm-unicharset", "lstm-recoder", "version"}


def main():
    data = open(sys.argv[1], "rb").read()
    n = struct.unpack_from("<i", data, 0)[0]
    offs = list(struct.unpack_from("<%dq" % n, data, 4))
    parts = []
    for i, o in enumerate(offs):
        if o < 0:
            parts.append(None)
            continue
        end = min([x for x in offs[i + 1:] if x >= 0] or [len(data)])
        name = NAMES[i] if i < len(NAMES) else str(i)
        parts.append(data[o:end] if name in KEEP else None)
    if parts[NAMES.index("lstm")] is None:
        sys.exit("no LSTM model in " + sys.argv[1])
    head = 4 + 8 * n
    out_offs, body, at = [], b"", head
    for p in parts:
        if p is None:
            out_offs.append(-1)
        else:
            out_offs.append(at)
            body += p
            at += len(p)
    with open(sys.argv[2], "wb") as f:
        f.write(struct.pack("<i", n) + struct.pack("<%dq" % n, *out_offs) + body)
    print("%s: %.1f MB -> %.1f MB" % (sys.argv[1], len(data) / 1e6, at / 1e6))


if __name__ == "__main__":
    main()
