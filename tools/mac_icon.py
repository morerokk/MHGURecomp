#!/usr/bin/env python3
"""The macOS app icon (AppIcon.icns) from the game's pixel-art icon (extras/icon/mhgurecomp.ico, as Windows, Linux and
Android use it): the sprite on a dark rounded tile, laid out on Apple's icon grid (an 824 px tile in a 1024 px canvas,
with its drop shadow), the ground the Android launcher icon's (android/res/values/colors.xml). Each size is drawn on its
own: the sprite's pixels stay square and sharp, scaled by a whole number where the size allows it.

  mac_icon.py <out.icns>          (tools/make_app.py; needs iconutil)
  mac_icon.py <out.png> [size]    (one size, to look at)

Plain Python: no imaging library."""
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ICO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "extras", "icon", "mhgurecomp.ico")
TOP, BOTTOM = (0x3A, 0x3C, 0x44), (0x1D, 0x1E, 0x22)  # the tile's gradient, into Android's ground at the bottom


def png_decode(data):
    """(width, height, rows of (r, g, b, a)) of an 8-bit RGBA or RGB PNG, not interlaced."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    at, idat = 8, b""
    while at < len(data):
        n, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + n]
        at += 12 + n
        if kind == b"IHDR":
            w, h, depth, color, _, _, interlace = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and color in (2, 6) and not interlace, "the icon's PNG: 8-bit RGB(A) only"
        elif kind == b"IDAT":
            idat += body
    ch = 4 if color == 6 else 3
    raw, stride, prev, rows, p = zlib.decompress(idat), w * ch, bytearray(w * ch), [], 0
    for _ in range(h):
        f, line = raw[p], bytearray(raw[p + 1:p + 1 + stride])
        p += 1 + stride
        for x in range(stride):
            a = line[x - ch] if x >= ch else 0
            b = prev[x]
            c = prev[x - ch] if x >= ch else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append([tuple(line[x * ch:x * ch + ch]) + ((255,) if ch == 3 else ()) for x in range(w)])
        prev = line
    return w, h, rows


def png_encode(w, h, rgba):
    raw = b"".join(b"\0" + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    chunk = lambda k, b: struct.pack(">I", len(b)) + k + b + struct.pack(">I", zlib.crc32(k + b))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def sprite():
    """The pixel art at its own size: rows of (r, g, b, a), from the .ico's largest image (drawn in square blocks)."""
    d = open(ICO, "rb").read()
    n = struct.unpack("<H", d[4:6])[0]
    best = max((struct.unpack("<BBBBHHII", d[6 + 16 * i:22 + 16 * i]) for i in range(n)), key=lambda e: e[0] or 256)
    w, h, px = png_decode(d[best[7]:best[7] + best[6]])
    xs = sorted({x for y in range(h) for x in range(1, w) if px[y][x] != px[y][x - 1]})
    ys = sorted({y for x in range(w) for y in range(1, h) if px[y][x] != px[y - 1][x]})
    block = min(b - a for e in (xs, ys) for a, b in zip(e, e[1:]))
    cols, lines = (xs[-1] - xs[0]) // block, (ys[-1] - ys[0]) // block
    return [[px[ys[0] + j * block + block // 2][xs[0] + i * block + block // 2] for i in range(cols)]
            for j in range(lines)]


def tile_distance(x, y, c, half, r):
    """Signed distance from (x, y) to a rounded square of centre c, half-size half, corner radius r (< 0 inside)."""
    qx, qy = abs(x - c) - half + r, abs(y - c) - half + r
    return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0) - r


def render(size, art):
    s = size / 1024
    c, half, r = size / 2, 412 * s, 185 * s  # (Apple's grid: 824 px tile, ~185 px corners)
    sh_dy, sh_blur = 12 * s, max(28 * s, 0.5)  # (its drop shadow: 12 px down, 28 px soft, 50% black)
    tile_top, tile_h = c - half, 2 * half
    lines, cols = len(art), len(art[0])
    k = 0.58 * tile_h / lines  # (the sword ~58% of the tile's height; whole pixels when it is that big)
    if k >= 2: k = math.floor(k)
    ox, oy = round(c - cols * k / 2), round(c - lines * k / 2)
    out = bytearray(size * size * 4)
    for y in range(size):
        t = min(max((y + 0.5 - tile_top) / tile_h, 0), 1)
        ground = [TOP[i] + (BOTTOM[i] - TOP[i]) * t for i in range(3)]
        for x in range(size):
            px, py = x + 0.5, y + 0.5
            d = tile_distance(px, py, c, half, r)
            cover = min(max(0.5 - d, 0), 1)
            ds = tile_distance(px, py - sh_dy, c, half, r)
            shadow = 0.5 * (1 - min(max((ds + sh_blur) / (2 * sh_blur), 0), 1)) * (1 - cover)
            if cover <= 0 and shadow <= 0: continue
            rgb = ground
            if cover > 0:  # the sprite's cells under this pixel, by area
                u0, u1, v0, v1 = (x - ox) / k, (x + 1 - ox) / k, (y - oy) / k, (y + 1 - oy) / k
                acc, cov = [0.0, 0.0, 0.0], 0.0
                for j in range(max(int(math.floor(v0)), 0), min(int(math.ceil(v1)), lines)):
                    hy = min(v1, j + 1) - max(v0, j)
                    for i in range(max(int(math.floor(u0)), 0), min(int(math.ceil(u1)), cols)):
                        p = art[j][i]
                        if not p[3]: continue
                        a = hy * (min(u1, i + 1) - max(u0, i)) * p[3] / 255
                        for n in range(3): acc[n] += p[n] * a
                        cov += a
                if cov:
                    area = (u1 - u0) * (v1 - v0)
                    rgb = [(acc[n] + ground[n] * (area - cov)) / area for n in range(3)]
            a = cover + shadow
            o = (y * size + x) * 4
            out[o:o + 4] = bytes([round(rgb[n] * cover / a) for n in range(3)] + [round(a * 255)])
    return png_encode(size, size, out)


def main():
    if len(sys.argv) < 2: sys.exit(__doc__)
    dst, art = sys.argv[1], sprite()
    if dst.endswith(".png"):
        size = int(sys.argv[2]) if len(sys.argv) > 2 else 1024
        open(dst, "wb").write(render(size, art))
        return
    with tempfile.TemporaryDirectory() as tmp:
        iconset = os.path.join(tmp, "AppIcon.iconset")
        os.mkdir(iconset)
        done = {}
        for size in (16, 32, 128, 256, 512):
            for scale in (1, 2):
                px = size * scale
                if px not in done: done[px] = render(px, art)
                open(os.path.join(iconset, f"icon_{size}x{size}{'@2x' if scale == 2 else ''}.png"), "wb").write(done[px])
        subprocess.run(["iconutil", "-c", "icns", iconset, "-o", dst], check=True)


if __name__ == "__main__":
    main()
