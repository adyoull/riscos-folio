#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
"""Draws the toolbar's icons and writes them as a RISC OS sprite file.
   python3 -I tools/mktoolsprites.py OUT [PREVIEW.png]
The Toolbox loads <Folio$Dir>.Sprites into Folio's own sprite area at start
(User Interface Toolbox p32), so these names don't touch the Wimp pool.
Each icon is 24x24 pixels at 90 dpi (48 OS units), 32bpp with a 1bpp mask,
drawn at 8x and shrunk. Dark grey line art with one accent colour, in the
spirit of the RISC OS 5 toolbars."""
import struct, sys
from PIL import Image, ImageDraw

S = 8
N = 24
W = N * S
INK = (50, 50, 60, 255)
ACC = (30, 90, 190, 255)
PAPER = (255, 255, 255, 255)
LW = S * 2


def canvas():
    im = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    return im, ImageDraw.Draw(im)


def page(d, x0, y0, x1, y1, fill=PAPER):
    d.rectangle([x0, y0, x1, y1], fill=fill, outline=INK, width=LW)


def magnifier(d, cx, cy, r):
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=PAPER, outline=INK, width=LW)
    k = r * 0.7
    d.line([(cx + k, cy + k), (cx + r * 1.9, cy + r * 1.9)], fill=INK, width=LW * 2)


def u(v):
    return v * S


def thumbs():
    im, d = canvas()
    page(d, u(2), u(3), u(8), u(21), fill=(225, 230, 240, 255))
    for i, y in enumerate((4.5, 10.5, 16.5)):
        page(d, u(3.2), u(y), u(6.8), u(y + 3.5), fill=ACC if i == 1 else PAPER)
    page(d, u(10), u(3), u(22), u(21))
    for y in (7, 10, 13, 16):
        d.line([(u(12.5), u(y)), (u(19.5), u(y))], fill=INK, width=S)
    return im


def contents():
    im, d = canvas()
    for i, y in enumerate((5, 10, 15, 20)):
        x = 4 if i % 2 == 0 else 8
        d.ellipse([u(x - 1.5), u(y - 1.5), u(x + 1.5), u(y + 1.5)], fill=ACC)
        d.line([(u(x + 3.5), u(y)), (u(21), u(y))], fill=INK, width=LW)
    return im


def arrow(up):
    im, d = canvas()
    if up:
        pts = [(u(12), u(5)), (u(20), u(15)), (u(4), u(15))]
        bar = (u(9), u(15), u(15), u(20))
    else:
        pts = [(u(12), u(19)), (u(20), u(9)), (u(4), u(9))]
        bar = (u(9), u(4), u(15), u(9))
    d.rectangle(bar, fill=ACC)
    d.polygon(pts, fill=ACC)
    return im


def zoom(plus):
    im, d = canvas()
    magnifier(d, u(10), u(10), u(7))
    d.line([(u(6.5), u(10)), (u(13.5), u(10))], fill=INK, width=LW)
    if plus:
        d.line([(u(10), u(6.5)), (u(10), u(13.5))], fill=INK, width=LW)
    return im


def fitwidth():
    im, d = canvas()
    page(d, u(5), u(3), u(19), u(21))
    y = u(12)
    d.line([(u(7), y), (u(17), y)], fill=ACC, width=LW)
    d.polygon([(u(6), y), (u(9.5), y - u(3)), (u(9.5), y + u(3))], fill=ACC)
    d.polygon([(u(18), y), (u(14.5), y - u(3)), (u(14.5), y + u(3))], fill=ACC)
    return im


def dark():
    im, d = canvas()
    page(d, u(4), u(3), u(20), u(21), fill=(20, 20, 25, 255))
    for y in (8, 11, 14, 17):
        d.line([(u(7), u(y)), (u(17), u(y))], fill=(235, 235, 235, 255), width=S)
    return im


def find():
    im, d = canvas()
    page(d, u(3), u(2), u(15), u(19))
    for y in (6, 9, 12):
        d.line([(u(5.5), u(y)), (u(12.5), u(y))], fill=INK, width=S)
    magnifier(d, u(15), u(14), u(4.5))
    return im


def define():
    im, d = canvas()
    # An open book with a question mark over it.
    d.polygon([(u(2), u(8)), (u(12), u(10)), (u(12), u(22)), (u(2), u(20))], fill=PAPER, outline=INK)
    d.polygon([(u(22), u(8)), (u(12), u(10)), (u(12), u(22)), (u(22), u(20))], fill=PAPER, outline=INK)
    d.line([(u(2), u(8)), (u(12), u(10)), (u(22), u(8))], fill=INK, width=LW)
    d.line([(u(12), u(10)), (u(12), u(22))], fill=INK, width=LW)
    d.arc([u(8.5), u(0.5), u(15.5), u(7)], 180, 90, fill=ACC, width=LW)
    d.line([(u(12), u(7)), (u(12), u(8))], fill=ACC, width=LW)
    return im


def highlight():
    im, d = canvas()
    d.rectangle([u(2), u(13), u(22), u(20)], fill=(255, 225, 40, 255))
    d.polygon([(u(9), u(15)), (u(18), u(4)), (u(22), u(8)), (u(13), u(18))], fill=ACC, outline=INK)
    d.polygon([(u(9), u(15)), (u(13), u(18)), (u(8), u(19))], fill=INK)
    return im


def underline():
    im, d = canvas()
    d.line([(u(7), u(3)), (u(7), u(13))], fill=INK, width=LW)
    d.line([(u(17), u(3)), (u(17), u(13))], fill=INK, width=LW)
    d.arc([u(7), u(8), u(17), u(18)], 0, 180, fill=INK, width=LW)
    d.line([(u(4), u(21)), (u(20), u(21))], fill=ACC, width=LW)
    return im


def strikeout():
    im, d = canvas()
    d.line([(u(6), u(20)), (u(12), u(4)), (u(18), u(20))], fill=INK, width=LW)
    d.line([(u(8.5), u(14)), (u(15.5), u(14))], fill=INK, width=LW)
    d.line([(u(3), u(11)), (u(21), u(11))], fill=(200, 30, 40, 255), width=LW)
    return im


def note():
    im, d = canvas()
    d.polygon([(u(3), u(3)), (u(21), u(3)), (u(21), u(15)), (u(11), u(15)), (u(6), u(20)), (u(6), u(15)),
               (u(3), u(15))], fill=(255, 235, 120, 255), outline=INK)
    for y in (7, 11):
        d.line([(u(6), u(y)), (u(18), u(y))], fill=INK, width=S)
    return im


ICONS = [
    ('tb_thumbs', thumbs), ('tb_contents', contents), ('tb_prev', lambda: arrow(True)),
    ('tb_next', lambda: arrow(False)), ('tb_zoomout', lambda: zoom(False)),
    ('tb_zoomin', lambda: zoom(True)), ('tb_fitw', fitwidth), ('tb_dark', dark),
    ('tb_find', find), ('tb_define', define), ('tb_hilite', highlight),
    ('tb_uline', underline), ('tb_strike', strikeout), ('tb_note', note),
]


def sprite(name, im):
    w, h = im.size
    mask_row = ((w + 31) // 32) * 4
    img = bytearray()
    mask = bytearray()
    px = im.load()
    for y in range(h):
        mrow = bytearray(mask_row)
        for x in range(w):
            r, g, b, a = px[x, y]
            if a >= 100:
                img += bytes((r, g, b, 0))
                mrow[x // 8] |= 1 << (x % 8)
            else:
                img += bytes((0, 0, 0, 0))
        mask += mrow
    hdr = 44
    mode = (6 << 27) | (90 << 14) | (90 << 1) | 1
    nm = name.encode().ljust(12, b'\0')
    head = struct.pack('<I', hdr + len(img) + len(mask)) + nm + struct.pack(
        '<7I', w - 1, h - 1, 0, 31, hdr, hdr + len(img), mode)
    return head + bytes(img) + bytes(mask)


def main():
    ims = [(n, f().resize((N, N), Image.LANCZOS)) for n, f in ICONS]
    body = b''.join(sprite(n, im) for n, im in ims)
    open(sys.argv[1], 'wb').write(struct.pack('<3I', len(ims), 16, 16 + len(body)) + body)
    if len(sys.argv) > 2:
        sheet = Image.new('RGBA', (len(ims) * (N * 3 + 8), N * 3 + 8), (221, 221, 221, 255))
        for i, (n, im) in enumerate(ims):
            sheet.alpha_composite(im.resize((N * 3, N * 3), Image.NEAREST), (i * (N * 3 + 8) + 4, 4))
        sheet.save(sys.argv[2])


main()
