#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
"""Writes form.pdf: a one-page PDF form for Folio's tests, with
- a text field "name", a multi-line text field "address";
- a check box "agree";
- radio buttons "size" (S, M, L);
- a combo box "colour" (Red, Green, Blue) and a list box "fruit";
- a field "total" worked out by JavaScript from "a" and "b";
- JavaScript run on opening, an attached file, a link that launches a
  program, a button that submits the form, and a web link.
The objects are written plainly, with an xref table worked out here.
Usage: make-form.py out.pdf"""
import sys

objs = {}


def obj(n, body):
    objs[n] = body


def field_rect(x0, y0, x1, y1):
    return "[%d %d %d %d]" % (x0, y0, x1, y1)


# 1 catalog, 2 pages, 3 page, 4 font, 5 AcroForm
fields = []
annots = []
n = 10


def add(body, is_field=True, in_annots=True):
    global n
    obj(n, body)
    if is_field:
        fields.append(n)
    if in_annots:
        annots.append(n)
    n += 1
    return n - 1


add("<< /Type /Annot /Subtype /Widget /FT /Tx /T (name) /V (Ada) /Rect %s /F 4 /P 3 0 R "
    "/DA (/Helv 12 Tf 0 g) /MK << /BC [0 0 0] >> >>" % field_rect(150, 740, 450, 762))
add("<< /Type /Annot /Subtype /Widget /FT /Tx /T (address) /Ff 4096 /Rect %s /F 4 /P 3 0 R "
    "/DA (/Helv 10 Tf 0 g) /MK << /BC [0 0 0] >> >>" % field_rect(150, 660, 450, 730))
add("<< /Type /Annot /Subtype /Widget /FT /Btn /T (agree) /V /Off /AS /Off /Rect %s /F 4 /P 3 0 R "
    "/DA (/ZaDb 12 Tf 0 g) /MK << /BC [0 0 0] /CA (4) >> "
    "/AP << /N << /Yes 6 0 R /Off 7 0 R >> >> >>" % field_rect(150, 620, 166, 636))
# Radio group "size": a parent with three kids.
radio_parent = n
kids = [n + 1, n + 2, n + 3]
obj(n, "<< /FT /Btn /Ff 49152 /T (size) /V /M /Kids [%s] >>" % " ".join("%d 0 R" % k for k in kids))
fields.append(n)
n += 1
for i, (name, x) in enumerate((("S", 150), ("M", 200), ("L", 250))):
    st = "/M" if name == "M" else "/Off"
    add("<< /Type /Annot /Subtype /Widget /Parent %d 0 R /AS %s /Rect %s /F 4 /P 3 0 R "
        "/DA (/ZaDb 12 Tf 0 g) /MK << /BC [0 0 0] /CA (l) >> "
        "/AP << /N << /%s 6 0 R /Off 7 0 R >> >> >>" % (radio_parent, st, field_rect(x, 580, x + 16, 596), name),
        is_field=False)
add("<< /Type /Annot /Subtype /Widget /FT /Ch /Ff 131072 /T (colour) /V (Green) "
    "/Opt [(Red) (Green) (Blue)] /Rect %s /F 4 /P 3 0 R /DA (/Helv 12 Tf 0 g) "
    "/MK << /BC [0 0 0] >> >>" % field_rect(150, 540, 300, 562))
add("<< /Type /Annot /Subtype /Widget /FT /Ch /T (fruit) /Opt [(Apple) (Pear) (Plum)] "
    "/Rect %s /F 4 /P 3 0 R /DA (/Helv 10 Tf 0 g) /MK << /BC [0 0 0] >> >>" % field_rect(150, 470, 300, 530))
add("<< /Type /Annot /Subtype /Widget /FT /Tx /T (a) /V (2) /Rect %s /F 4 /P 3 0 R "
    "/DA (/Helv 12 Tf 0 g) /MK << /BC [0 0 0] >> >>" % field_rect(150, 430, 220, 452))
add("<< /Type /Annot /Subtype /Widget /FT /Tx /T (b) /V (3) /Rect %s /F 4 /P 3 0 R "
    "/DA (/Helv 12 Tf 0 g) /MK << /BC [0 0 0] >> >>" % field_rect(230, 430, 300, 452))
total = add("<< /Type /Annot /Subtype /Widget /FT /Tx /T (total) /V (5) /Ff 1 /Rect %s /F 4 /P 3 0 R "
            "/DA (/Helv 12 Tf 0 g) /MK << /BC [0 0 0] >> "
            "/AA << /C << /S /JavaScript /JS (event.value = Number(this.getField('a').value) + "
            "Number(this.getField('b').value);) >> >> >>" % field_rect(310, 430, 380, 452))
add("<< /Type /Annot /Subtype /Widget /FT /Btn /Ff 65536 /T (send) /Rect %s /F 4 /P 3 0 R "
    "/DA (/Helv 12 Tf 0 g) /MK << /BC [0 0 0] /CA (Send) >> "
    "/A << /S /SubmitForm /F << /FS /URL /F (https://example.com/form) >> >> >>" % field_rect(150, 390, 230, 412))
# Not fields: a launch link and a web link.
add("<< /Type /Annot /Subtype /Link /Rect %s /Border [0 0 0] "
    "/A << /S /Launch /F (!Edit) >> >>" % field_rect(50, 350, 200, 370), is_field=False)
add("<< /Type /Annot /Subtype /Link /Rect %s /Border [0 0 0] "
    "/A << /S /URI /URI (https://www.riscosopen.org/) >> >>" % field_rect(250, 350, 400, 370), is_field=False)

obj(1, "<< /Type /Catalog /Pages 2 0 R /AcroForm 5 0 R "
       "/Names << /EmbeddedFiles << /Names [(notes.txt) 8 0 R] >> >> "
       "/OpenAction << /S /JavaScript /JS (var opened = 1;) >> >>")
obj(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 >>")
obj(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /Helv 4 0 R >> >> "
       "/Contents 9 0 R /Annots [%s] >>" % " ".join("%d 0 R" % a for a in annots))
obj(4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>")
obj(5, "<< /Fields [%s] /DA (/Helv 12 Tf 0 g) /DR << /Font << /Helv 4 0 R "
       "/ZaDb << /Type /Font /Subtype /Type1 /BaseFont /ZapfDingbats >> >> >> "
       "/CO [%d 0 R] >>" % (" ".join("%d 0 R" % f for f in fields), total))
yes = b"q 0 g BT /ZaDb 12 Tf 2 3 Td (4) Tj ET Q"
obj(6, "<< /Type /XObject /Subtype /Form /BBox [0 0 16 16] /Resources << /Font << /ZaDb << /Type /Font "
       "/Subtype /Type1 /BaseFont /ZapfDingbats >> >> >> /Length %d >>\nstream\n%s\nendstream"
    % (len(yes), yes.decode()))
obj(7, "<< /Type /XObject /Subtype /Form /BBox [0 0 16 16] /Length 0 >>\nstream\n\nendstream")
note = b"A file attached to the form."
obj(8, "<< /Type /Filespec /F (notes.txt) /EF << /F 99 0 R >> >>")
obj(99, "<< /Type /EmbeddedFile /Length %d >>\nstream\n%s\nendstream" % (len(note), note.decode()))
text = ("BT /Helv 12 Tf 50 745 Td (Name) Tj 0 -40 Td (Address) Tj 0 -75 Td (I agree) Tj "
        "0 -40 Td (Size) Tj 0 -40 Td (Colour) Tj 0 -60 Td (Fruit) Tj 0 -50 Td (a + b =) Tj "
        "0 -40 Td (Send) Tj 0 -40 Td (Start Edit) Tj 200 0 Td (ROOL web site) Tj ET")
obj(9, "<< /Length %d >>\nstream\n%s\nendstream" % (len(text), text))

out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
offsets = {}
for k in sorted(objs):
    offsets[k] = len(out)
    out += ("%d 0 obj\n%s\nendobj\n" % (k, objs[k])).encode("latin-1")
size = max(objs) + 1
xref = len(out)
out += ("xref\n0 %d\n" % size).encode()
out += b"0000000000 65535 f \n"
for k in range(1, size):
    if k in offsets:
        out += ("%010d 00000 n \n" % offsets[k]).encode()
    else:
        out += b"0000000000 65535 f \n"
out += ("trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (size, xref)).encode()
with open(sys.argv[1], "wb") as f:
    f.write(out)
