#!/usr/bin/env python3
"""Turn the keyboard and mouse button prompt pictures into the cells the recomp draws (runtime/hle/prompts.c).

    python tools/prompt_icons.py [-o runtime/hle/prompt_icons.h]

extras/prompts/mouse_icons.png is a 4 x 2 grid of mouse pictures (left to right, top to bottom: the mouse, left button,
right button, both buttons; middle button, wheel up, wheel down, moving); extras/prompts/keycap.png a key's cap, on
which the recomp writes the key's name. Each picture used is cut to its visible pixels, scaled down (an average over
the pixels each cell pixel covers, alpha-weighted) to fit the game's button glyph cell (24 x 24, the button font's;
CELL_FIT of it), centred, and written as straight-alpha RGBA rows. Pure Python: no image library needed.
"""
import argparse
import os
import struct
import zlib

ROOT = os.path.join(os.path.dirname(__file__), "..")
CELL = 24
MOUSE_FIT = 22  # (the game's own glyphs fill 22-23 of their 24)
KEYCAP_FIT = 24
# (name, cell of the 4 x 2 grid)
MOUSE = [("LMB", 1), ("RMB", 2), ("MMB", 4), ("WHEEL_UP", 5), ("WHEEL_DOWN", 6)]


def read_png(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", path
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        n, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and ctype in (2, 6) and inter == 0, "%s: 8-bit RGB(A), not interlaced" % path
            bpp = 4 if ctype == 6 else 3
        elif kind == b"IDAT":
            idat += body
        pos += 12 + n
    raw, stride = zlib.decompress(idat), w * bpp
    rows, prev = [], bytearray(stride)
    for y in range(h):
        f, line = raw[y * (stride + 1)], bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a = line[i - bpp] if i >= bpp else 0
            b = prev[i]
            c = prev[i - bpp] if i >= bpp else 0
            if f == 1: line[i] = (line[i] + a) & 255
            elif f == 2: line[i] = (line[i] + b) & 255
            elif f == 3: line[i] = (line[i] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    px = [[tuple(r[x * bpp:x * bpp + bpp]) + ((255,) if bpp == 3 else ()) for x in range(w)] for r in rows]
    return w, h, px


def crop(px, x0, y0, x1, y1):
    """The box of the visible pixels (alpha > 8) inside [x0, x1) x [y0, y1)."""
    xs = [x for y in range(y0, y1) for x in range(x0, x1) if px[y][x][3] > 8]
    ys = [y for y in range(y0, y1) for x in range(x0, x1) if px[y][x][3] > 8]
    return min(xs), min(ys), max(xs) + 1, max(ys) + 1


def fit(px, box, size):
    """The box scaled to fit size x size (aspect kept), centred in a CELL x CELL cell: RGBA bytes."""
    x0, y0, x1, y1 = box
    bw, bh = x1 - x0, y1 - y0
    s = size / max(bw, bh)
    ow, oh = max(1, round(bw * s)), max(1, round(bh * s))
    ox, oy = (CELL - ow) // 2, (CELL - oh) // 2
    out = bytearray(CELL * CELL * 4)
    for cy in range(oh):
        for cx in range(ow):
            # the source rectangle this pixel covers, with partial coverage at its edges
            sx0, sx1 = x0 + cx * bw / ow, x0 + (cx + 1) * bw / ow
            sy0, sy1 = y0 + cy * bh / oh, y0 + (cy + 1) * bh / oh
            acc, wsum = [0.0] * 4, 0.0
            for y in range(int(sy0), min(int(sy1 + 0.999), y1)):
                wy = min(y + 1, sy1) - max(y, sy0)
                for x in range(int(sx0), min(int(sx1 + 0.999), x1)):
                    wx = min(x + 1, sx1) - max(x, sx0)
                    wgt = wx * wy
                    if wgt <= 0: continue
                    r, g, b, a = px[y][x]
                    acc[0] += r * a * wgt; acc[1] += g * a * wgt; acc[2] += b * a * wgt; acc[3] += a * wgt
                    wsum += wgt
            a = acc[3] / wsum if wsum else 0
            rgb = [acc[i] / acc[3] if acc[3] else 0 for i in range(3)]
            o = ((oy + cy) * CELL + ox + cx) * 4
            out[o:o + 4] = bytes(int(round(min(255, max(0, v)))) for v in rgb + [a])
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("-o", "--out", default=os.path.join(ROOT, "runtime", "hle", "prompt_icons.h"))
    a = ap.parse_args()
    cells = []
    w, h, px = read_png(os.path.join(ROOT, "extras", "prompts", "mouse_icons.png"))
    cw, ch = w // 4, h // 2
    for name, i in MOUSE:
        cx, cy = i % 4 * cw, i // 4 * ch
        cells.append((name, fit(px, crop(px, cx, cy, cx + cw, cy + ch), MOUSE_FIT)))
    w, h, px = read_png(os.path.join(ROOT, "extras", "prompts", "keycap.png"))
    cells.append(("KEYCAP", fit(px, crop(px, 0, 0, w, h), KEYCAP_FIT)))
    with open(a.out, "w", encoding="utf-8", newline="\n") as o:
        o.write("// Keyboard and mouse button prompt cells (runtime/hle/prompts.c), %d x %d straight-alpha RGBA rows: made by\n" % (CELL, CELL))
        o.write("// tools/prompt_icons.py from extras/prompts/*.png; edit those, not this.\n#pragma once\n#include <stdint.h>\n\n")
        o.write("enum { %s, PROMPT_ICONS };\n" % ", ".join("PROMPT_" + n for n, _ in cells))
        o.write("#define PROMPT_CELL %d\n" % CELL)
        o.write("static const uint8_t prompt_icons[PROMPT_ICONS][PROMPT_CELL * PROMPT_CELL * 4] = {\n")
        for name, data in cells:
            o.write("    {  // %s\n" % name)
            for r in range(0, len(data), 24):
                o.write("        " + ", ".join(str(v) for v in data[r:r + 24]) + ",\n")
            o.write("    },\n")
        o.write("};\n")
    print("%d cells -> %s" % (len(cells), a.out))


if __name__ == "__main__":
    main()
