#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
"""Writes tests/sig/certified.pdf for sign_test: a PDF whose signature
dictionary (not a real signature: Contents is one zero byte) certifies it
with DocMDP P 2 (filling in and signing only), and whose signature field
locks the text field "locked" (/Lock Include). "name" stays free. Folio's
rules for signed PDFs read only these dictionaries.
Usage: make-certified.py [OUT]"""
import sys

objs = [
    "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [6 0 R 7 0 R 8 0 R] /DA (/Helv 12 Tf 0 g) "
    "/DR << /Font << /Helv 4 0 R >> >> /SigFlags 3 >> /Perms << /DocMDP 9 0 R >> >>",
    "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
    "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 4 0 R >> >> "
    "/Contents 5 0 R /Annots [6 0 R 7 0 R 8 0 R] >>",
    "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    None,
    "<< /Type /Annot /Subtype /Widget /FT /Sig /T (Cert) /V 9 0 R /Rect [72 600 288 672] /F 132 /P 3 0 R "
    "/Lock << /Type /SigFieldLock /Action /Include /Fields [(locked)] >> >>",
    "<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /V (Ada) /Rect [72 740 300 760] /F 4 /P 3 0 R "
    "/DA (/Helv 12 Tf 0 g) >>",
    "<< /Type /Annot /Subtype /Widget /FT /Tx /T (locked) /V (Fixed) /Rect [72 700 300 720] /F 4 /P 3 0 R "
    "/DA (/Helv 12 Tf 0 g) >>",
    "<< /Type /Sig /Filter /Adobe.PPKLite /SubFilter /adbe.pkcs7.detached /ByteRange [0 0 0 0] /Contents <00> "
    "/Reference [<< /Type /SigRef /TransformMethod /DocMDP /TransformParams << /Type /TransformParams /P 2 "
    "/V /1.2 >> >>] >>",
]
t = "BT /F1 18 Tf 72 790 Td (Certified test page: fill in only) Tj ET"
objs[4] = "<< /Length %d >>\nstream\n%s\nendstream" % (len(t), t)
out = bytearray(b"%PDF-1.7\n")
offs = []
for i, o in enumerate(objs):
    offs.append(len(out))
    out += ("%d 0 obj\n%s\nendobj\n" % (i + 1, o)).encode()
x = len(out)
out += ("xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)).encode()
for o in offs:
    out += ("%010d 00000 n \n" % o).encode()
out += ("trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, x)).encode()
open(sys.argv[1] if len(sys.argv) > 1 else "tests/sig/certified.pdf", "wb").write(out)
