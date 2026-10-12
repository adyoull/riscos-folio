#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (C) 2026 Andrew Youll
"""Draws a RISC OS Draw file as a PNG (paths and sprites, as Folio writes
them), to look at what core/drawfile.c made:

    python3 tests/draw_view.py FILE.aff OUT.png [pixels per point]

Needs pycairo. Only what Folio writes: path objects (fills, strokes, caps,
joins, dashes, winding rule), sprite objects (5) and transformed sprites
(13) in 32bpp with 1bpp masks, and groups (6)."""
import struct
import sys

import cairo

DU_PER_PT = 640.0


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def i32(b, o):
    return struct.unpack_from('<i', b, o)[0]


def colour(w):
    return ((w >> 8) & 255) / 255, ((w >> 16) & 255) / 255, ((w >> 24) & 255) / 255


def sprite_surface(b, o):
    """A 32bpp sprite at o (its control block) as an ARGB cairo surface."""
    w = i32(b, o + 16) + 1
    h = i32(b, o + 20) + 1
    img, mask = u32(b, o + 32), u32(b, o + 36)
    surf = cairo.ImageSurface(cairo.FORMAT_ARGB32, w, h)
    data = surf.get_data()
    stride = surf.get_stride()
    mw = (w + 31) // 32 * 4
    for y in range(h):
        row = o + img + y * w * 4
        for x in range(w):
            r, g, bl = b[row + 4 * x], b[row + 4 * x + 1], b[row + 4 * x + 2]
            a = 255
            if mask != img:
                a = 255 if (b[o + mask + y * mw + (x >> 3)] >> (x & 7)) & 1 else 0
            if a == 0:
                r = g = bl = 0
            data[y * stride + 4 * x:y * stride + 4 * x + 4] = bytes((bl, g, r, a))
    surf.mark_dirty()
    return surf, w, h


def draw_objects(cr, b, o, end, top):
    while o < end:
        t, size = u32(b, o), u32(b, o + 4)
        if t == 2:
            fill, outline, width, style = u32(b, o + 24), u32(b, o + 28), i32(b, o + 32), u32(b, o + 36)
            q = o + 40
            dash = None
            if style & 0x80:
                off, n = i32(b, q), u32(b, q + 4)
                dash = ([i32(b, q + 8 + 4 * k) / DU_PER_PT for k in range(n)], off / DU_PER_PT)
                q += 8 + 4 * n
            cr.new_path()
            while q < o + size:
                tag = u32(b, q) & 255
                q += 4
                if tag == 0:
                    break
                if tag == 2:
                    cr.move_to(i32(b, q) / DU_PER_PT, top - i32(b, q + 4) / DU_PER_PT)
                    q += 8
                elif tag == 8:
                    cr.line_to(i32(b, q) / DU_PER_PT, top - i32(b, q + 4) / DU_PER_PT)
                    q += 8
                elif tag == 6:
                    p = [i32(b, q + 4 * k) / DU_PER_PT for k in range(6)]
                    cr.curve_to(p[0], top - p[1], p[2], top - p[3], p[4], top - p[5])
                    q += 24
                elif tag == 5:
                    cr.close_path()
            cr.set_fill_rule(cairo.FILL_RULE_EVEN_ODD if style & 0x40 else cairo.FILL_RULE_WINDING)
            if fill != 0xffffffff:
                cr.set_source_rgb(*colour(fill))
                cr.fill_preserve()
            if outline != 0xffffffff:
                cr.set_source_rgb(*colour(outline))
                cr.set_line_width(max(width / DU_PER_PT, 0.25))
                cr.set_line_join([cairo.LINE_JOIN_MITER, cairo.LINE_JOIN_ROUND, cairo.LINE_JOIN_BEVEL][style & 3 if style & 3 < 3 else 0])
                cr.set_line_cap([cairo.LINE_CAP_BUTT, cairo.LINE_CAP_ROUND, cairo.LINE_CAP_SQUARE, cairo.LINE_CAP_BUTT][(style >> 2) & 3])
                if dash:
                    cr.set_dash(dash[0], dash[1])
                cr.stroke_preserve()
                cr.set_dash([])
            cr.new_path()
        elif t in (5, 13):
            x0, y0, x1, y1 = (i32(b, o + 8 + 4 * k) / DU_PER_PT for k in range(4))
            s = o + 24 + (24 if t == 13 else 0)
            surf, w, h = sprite_surface(b, s)
            cr.save()
            if t == 5:
                cr.translate(x0, top - y1)
                cr.scale((x1 - x0) / w, (y1 - y0) / h)
            else:
                a, bb, c, d = (i32(b, o + 24 + 4 * k) / 65536 for k in range(4))
                sx = cairo.Matrix(512, 0, 0, -512, 0, 512 * h)                  # pixel -> sprite space
                dr = cairo.Matrix(a, bb, c, d, i32(b, o + 40), i32(b, o + 44))  # sprite -> Draw units
                pg = cairo.Matrix(1 / DU_PER_PT, 0, 0, -1 / DU_PER_PT, 0, top)  # Draw units -> page
                cr.transform(sx.multiply(dr).multiply(pg))
            cr.set_source_surface(surf, 0, 0)
            cr.get_source().set_filter(cairo.FILTER_NEAREST)
            cr.paint()
            cr.restore()
        elif t == 6:
            draw_objects(cr, b, o + 36, o + size, top)
        o += size


def main():
    b = open(sys.argv[1], 'rb').read()
    out = sys.argv[2]
    k = float(sys.argv[3]) if len(sys.argv) > 3 else 1.5
    assert b[:4] == b'Draw'
    x0, y0, x1, y1 = (i32(b, 24 + 4 * j) / DU_PER_PT for j in range(4))
    w, h = int(x1 * k) + 2, int(y1 * k) + 2
    top = y1
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, w, h)
    cr = cairo.Context(surf)
    cr.set_source_rgb(1, 1, 1)
    cr.paint()
    cr.scale(k, k)
    draw_objects(cr, b, 40, len(b), top)
    surf.write_to_png(out)


if __name__ == '__main__':
    main()
