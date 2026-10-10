#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
"""Draws Folio's icons and writes a RISC OS sprite file (!Sprites).
   python3 -I tools/mksprites.py OUT
Sprites: '!folio' (34x34) and 'sm!folio' (18x18), 32bpp (type 6, 90 dpi) with a
1bpp mask (new-format sprite: PRM 'Format Of Sprite', p1177-1178).
The picture: an open book, cream pages with a red ribbon."""
import struct, sys
from PIL import Image, ImageDraw

def book(size):
    s = 8                       # draw big, then shrink
    W = size * s
    im = Image.new('RGBA', (W, W), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    m = W * 0.06
    mid = W / 2
    top, bot = W * 0.18, W * 0.86
    sag = W * 0.06
    cover = (40, 70, 140, 255)
    # Cover behind the pages.
    d.polygon([(m, top + sag), (mid, top + 2 * sag), (W - m, top + sag), (W - m, bot + sag * 0.6),
               (mid, bot + sag * 1.4), (m, bot + sag * 0.6)], fill=cover)
    page = (250, 244, 225, 255)
    edge = (90, 90, 90, 255)
    for side in (-1, 1):
        outer = mid + side * (mid - m * 1.6)
        pts = [(mid, top + sag * 1.2), (outer, top), (outer, bot - sag * 0.4), (mid, bot + sag * 0.4)]
        d.polygon(pts, fill=page, outline=edge)
        # Lines of text.
        for k in range(5):
            y = top + (bot - top) * (0.18 + 0.14 * k)
            x0 = mid + side * W * 0.06
            x1 = outer - side * W * 0.07
            d.line([(x0, y + sag * 0.5), (x1, y - sag * 0.2)], fill=(120, 120, 130, 255), width=max(1, s // 2))
    d.line([(mid, top + sag * 1.2), (mid, bot + sag * 0.4)], fill=edge, width=s)
    # Ribbon.
    rx = mid + W * 0.12
    d.polygon([(rx, top + sag * 0.6), (rx + W * 0.07, top + sag * 0.5), (rx + W * 0.07, bot + W * 0.08),
               (rx + W * 0.035, bot + W * 0.04), (rx, bot + W * 0.09)], fill=(200, 30, 40, 255))
    return im.resize((size, size), Image.LANCZOS)

def sprite(name, im):
    w, h = im.size
    words = w                    # 32bpp: one word per pixel
    mask_row = ((w + 31) // 32) * 4
    img = bytearray()
    mask = bytearray()
    px = im.load()
    for y in range(h):
        mrow = bytearray(mask_row)
        for x in range(w):
            r, g, b, a = px[x, y]
            if a >= 128:
                img += bytes((r, g, b, 0))
                mrow[x // 8] |= 1 << (x % 8)
            else:
                img += bytes((0, 0, 0, 0))
        mask += mrow
    hdr_size = 44
    img_off = hdr_size
    mask_off = hdr_size + len(img)
    total = hdr_size + len(img) + len(mask)
    mode = (6 << 27) | (90 << 14) | (90 << 1) | 1
    nm = name.encode().ljust(12, b'\0')
    hdr = struct.pack('<I', total) + nm + struct.pack('<8I', words - 1, h - 1, 0, 31, img_off, mask_off, mode, 0)[:28]
    return hdr + bytes(img) + bytes(mask)

def main():
    sprites = [sprite('!folio', book(34)), sprite('sm!folio', book(18))]
    body = b''.join(sprites)
    head = struct.pack('<3I', len(sprites), 16, 16 + len(body))
    open(sys.argv[1], 'wb').write(head + body)

main()
