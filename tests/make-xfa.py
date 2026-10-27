#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
"""Writes two XFA forms for Folio's tests (xfa_test):
  xfa-static.pdf   a "hybrid" form: ordinary fields form1[0].Page1[0].Name[0]
                   and .Agree[0] on the page, and the XFA as an array of
                   packets (preamble, template, datasets, postamble);
  xfa-dynamic.pdf  a dynamic form: NeedsRendering, no ordinary fields, a
                   "Please wait..." page, and the XFA as one stream.
Both have the same template and data:
  form1.Page1: Name (text; data "Ada & Co"), Notes (multi-line; no data),
    Agree (check box 1/0; data 0), Colour (choice Red/Green/Blue kept as
    R/G/B; data G), Size (radio group Small/Medium/Large kept as S/M/L;
    data empty, written <Size/>), Total (number, read-only; data 12),
    Secret (bind none), Town (inside an unnamed subform; no data), Phone
    (dataRef $record.Contact.Phone; no data);
  form1.Item (repeating): Price (number; data 5 and 7).
Usage: make-xfa.py DIR"""
import os
import sys

TEMPLATE = """<template xmlns="http://www.xfa.org/schema/xfa-template/3.3/">
<subform name="form1" layout="tb" locale="en_GB">
<pageSet><pageArea name="Page1"><contentArea x="0.25in" y="0.25in" w="8in" h="10.5in"/>
<medium stock="a4" short="210mm" long="297mm"/></pageArea></pageSet>
<subform name="Page1" layout="tb">
<field name="Name" w="80mm" h="9mm"><ui><textEdit/></ui>
<caption><value><text>Your name</text></value></caption></field>
<field name="Notes"><ui><textEdit multiLine="1"/></ui>
<caption><value><text>Notes</text></value></caption></field>
<field name="Agree"><ui><checkButton/></ui>
<caption><value><text>I agree</text></value></caption>
<items><integer>1</integer><integer>0</integer></items></field>
<field name="Colour"><ui><choiceList/></ui>
<caption><value><text>Colour</text></value></caption>
<items><text>Red</text><text>Green</text><text>Blue</text></items>
<items save="1"><text>R</text><text>G</text><text>B</text></items></field>
<exclGroup name="Size">
<field name="S"><ui><checkButton shape="round"/></ui><caption><value><text>Small</text></value></caption><items><text>S</text></items></field>
<field name="M"><ui><checkButton shape="round"/></ui><caption><value><text>Medium</text></value></caption><items><text>M</text></items></field>
<field name="L"><ui><checkButton shape="round"/></ui><caption><value><text>Large</text></value></caption><items><text>L</text></items></field>
</exclGroup>
<field name="Total" access="readOnly"><ui><numericEdit/></ui>
<caption><value><text>Total</text></value></caption></field>
<field name="Secret"><bind match="none"/><ui><textEdit/></ui></field>
<subform><field name="Town"><ui><textEdit/></ui><assist><toolTip>Town or city</toolTip></assist></field></subform>
<field name="Phone"><ui><textEdit/></ui><bind match="dataRef" ref="$record.Contact.Phone"/>
<caption><value><text>Phone</text></value></caption></field>
<field name="Send"><ui><button/></ui></field>
</subform>
<subform name="Item"><occur max="-1"/>
<field name="Price"><ui><numericEdit/></ui><caption><value><text>Price</text></value></caption></field>
</subform>
</subform>
</template>"""

DATASETS = """<xfa:datasets xmlns:xfa="http://www.xfa.org/schema/xfa-data/1.0/">
<xfa:data>
<form1><Page1><Name>Ada &amp; Co</Name><Agree>0</Agree><Colour>G</Colour><Size/><Total>12</Total></Page1>
<Item><Price>5</Price></Item><Item><Price>7</Price></Item></form1>
</xfa:data>
</xfa:datasets>"""

PRE = '<xdp:xdp xmlns:xdp="http://ns.adobe.com/xdp/" timeStamp="2026-10-27T12:00:00Z">'
POST = '</xdp:xdp>'


def pdf(objs, root=1):
    out = bytearray(b"%PDF-1.7\n%\xe2\xe3\xcf\xd3\n")
    offs = []
    for i, o in enumerate(objs):
        offs.append(len(out))
        out += b"%d 0 obj\n" % (i + 1) + (o if isinstance(o, bytes) else o.encode()) + b"\nendobj\n"
    x = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
    for o in offs:
        out += b"%010d 00000 n \n" % o
    out += b"trailer\n<< /Size %d /Root %d 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, root, x)
    return bytes(out)


def stream(data, extra=""):
    data = data.encode() if isinstance(data, str) else data
    return b"<< /Length %d%s >>\nstream\n" % (len(data), extra.encode()) + data + b"\nendstream"


def static(path):
    # 1 catalog 2 pages 3 page 4 font 5 contents 6..9 xfa packets
    # 10 form1[0] 11 Page1[0] 12 Name[0] 13 Agree[0] 14 on AP 15 off AP
    text = "BT /F1 12 Tf 72 780 Td (Your name) Tj 0 -40 Td (I agree) Tj ET"
    objs = [
        "<< /Type /Catalog /Pages 2 0 R /AcroForm << /Fields [10 0 R] /DA (/Helv 0 Tf 0 g) "
        "/DR << /Font << /Helv 4 0 R >> >> /XFA [(preamble) 6 0 R (template) 7 0 R (datasets) 8 0 R "
        "(postamble) 9 0 R] >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 4 0 R >> >> "
        "/Contents 5 0 R /Annots [12 0 R 13 0 R] >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>",
        stream(text),
        stream(PRE), stream(TEMPLATE), stream(DATASETS), stream(POST),
        "<< /T (form1[0]) /Kids [11 0 R] >>",
        "<< /T (Page1[0]) /Parent 10 0 R /Kids [12 0 R 13 0 R] >>",
        "<< /Type /Annot /Subtype /Widget /FT /Tx /T (Name[0]) /Parent 11 0 R /V (Ada & Co) "
        "/Rect [150 772 450 794] /F 4 /P 3 0 R /DA (/Helv 12 Tf 0 g) /MK << /BC [0 0 0] >> >>",
        "<< /Type /Annot /Subtype /Widget /FT /Btn /T (Agree[0]) /Parent 11 0 R /V /Off /AS /Off "
        "/Rect [150 732 166 748] /F 4 /P 3 0 R /DA (/ZaDb 12 Tf 0 g) /MK << /BC [0 0 0] /CA (4) >> "
        "/AP << /N << /1 14 0 R /Off 15 0 R >> >> >>",
        stream("q 0 g BT /ZaDb 12 Tf 2 3 Td (4) Tj ET Q", " /Type /XObject /Subtype /Form /BBox [0 0 16 16]"
               " /Resources << /Font << /ZaDb << /Type /Font /Subtype /Type1 /BaseFont /ZapfDingbats >> >> >>"),
        stream("", " /Type /XObject /Subtype /Form /BBox [0 0 16 16]"),
    ]
    open(path, "wb").write(pdf(objs))


def dynamic(path):
    text = "BT /F1 18 Tf 72 760 Td (Please wait...) Tj ET"
    xdp = PRE + TEMPLATE + DATASETS + POST
    objs = [
        "<< /Type /Catalog /Pages 2 0 R /NeedsRendering true /AcroForm << /Fields [] /XFA 6 0 R >> >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 595 842] /Resources << /Font << /F1 4 0 R >> >> "
        "/Contents 5 0 R >>",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        stream(text),
        stream(xdp),
    ]
    open(path, "wb").write(pdf(objs))


d = sys.argv[1] if len(sys.argv) > 1 else "."
static(os.path.join(d, "xfa-static.pdf"))
dynamic(os.path.join(d, "xfa-dynamic.pdf"))
