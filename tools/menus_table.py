#!/usr/bin/env python3
"""Turn learned menu geometries (menus_learned.txt, runtime/hle/menuptr.c) into the table the recomp ships.

    python tools/menus_table.py <menus_learned.txt>... [-o runtime/hle/menuptr_table.h]

The files are read in order as the game reads one (later lines win, a known bar only gets the steps it lacked, row
places apply to the geometry with their bar), the table's own lines first, so a playthrough's file adds to and corrects
what is shipped. A call keeps at most four geometries; a geometry with no bar signature (an older line: a fixed place) is
left out when its call has one with a signature. The pause menu is built in (menuptr.c) and left out. The result is
one line per geometry and one per row place, sorted, as a C string the game parses before the player's own file.
"""
import argparse
import os
import sys

BUILTIN = {0x86F8EC, 0x87D824, 0x880798, 0x5E29A0, 0x87F8A0, 0x8874CC, 0x931D1C, 0x9839B8, 0x984314, 0x981F1C, 0x993D04, 0x967514, 0x5890A8}  # (menuptr.c builtin[]: the pause menu, the front end's)
MAX_VARIANTS = 4
ROWS = 16


def near(a, b, tol=0.5):
    return abs(a - b) < tol


class Geom:
    def __init__(self, lr, x, y, w, h, dx, dy, z=0.0, kw=0.0, kh=0.0, ox=0.0, oy=0.0, cdx=0.0, cdy=0.0, cols=0.0, count=0.0):
        self.lr, self.x, self.y, self.w, self.h, self.dx, self.dy = lr, x, y, w, h, dx, dy
        self.z, self.kw, self.kh, self.ox, self.oy, self.cdx, self.cdy = z, kw, kh, ox, oy, cdx, cdy
        self.cols = int(cols)
        self.count = int(count)  # the cells it was learned with: only for a menu spaced by its count (0: any)
        self.off = {}  # row -> (x, y) from the first row's bar

    def same_kind(self, o):  # (menuptr.c same_kind)
        if (not self.kw) != (not o.kw):
            return False
        if not self.kw:
            return True
        return (near(self.z, o.z) and abs(self.kw - o.kw) < 0.01 and abs(self.kh - o.kh) < 0.01 and near(self.w, o.w)
                and near(self.h, o.h))

    def same_sig(self, z, kw, kh):  # (menuptr.c geom_kind_sig)
        return self.kw and near(self.z, z) and abs(self.kw - kw) < 0.01 and abs(self.kh - kh) < 0.01


def steps_differ(a, b):  # (menuptr.c steps_differ)
    return (a.dx or a.dy) and (b.dx or b.dy) and (not near(a.dx, b.dx) or not near(a.dy, b.dy))


def put(table, g):  # (menuptr.c geom_put)
    variants = table.setdefault(g.lr, [])
    o = next((o for o in variants if o.count and o.count == g.count and o.same_kind(g)), None)
    if o is None:
        o = next((o for o in variants if o.same_kind(g)), None)
    if o is not None and o.kw and g.count and o.count != g.count and steps_differ(o, g):
        o = None  # (another count with another step: a geometry of its own)
    if o is not None:
        if not o.kw:
            variants[variants.index(o)] = g
            return
        if not o.dx and not o.dy and (g.dx or g.dy):
            o.dx, o.dy = g.dx, g.dy
        if not o.cdx and not o.cdy and (g.cdx or g.cdy):
            o.cdx, o.cdy = g.cdx, g.cdy
        if not o.cols and g.cols:
            o.cols = g.cols
        o.x, o.y = g.x, g.y
        return
    if not any(v.same_kind(g) for v in variants):
        g.count = 0  # (the count is kept only where it splits a menu)
    if len(variants) >= MAX_VARIANTS:
        variants[0] = g
    else:
        variants.append(g)


def read(table, lines):  # (menuptr.c geoms_load)
    for line in lines:
        t = line.split()
        if not t or t[0].startswith(("#", "//")):
            continue
        try:
            lr = int(t[0], 16)
            if len(t) == 8 and t[1] == "off":
                row = int(t[2])
                x, y, z, kw, kh = map(float, t[3:8])
                for o in table.get(lr, []):
                    if o.same_sig(z, kw, kh):
                        if 0 <= row < ROWS:
                            o.off.setdefault(0, (0.0, 0.0))
                            o.off[row] = (x, y)
                        break
                continue
            v = list(map(float, t[1:]))
        except ValueError:
            continue
        if len(v) not in (6, 11, 13, 14, 15) or lr in BUILTIN:
            continue
        v += [0.0] * (15 - len(v))
        put(table, Geom(lr, *v))


def fmt(v):
    s = ("%.2f" % v).rstrip("0").rstrip(".")
    return "0" if s in ("", "-0") else s


def lines_of(table):
    out = []
    for lr in sorted(table):
        variants = table[lr]
        if any(g.kw for g in variants):
            variants = [g for g in variants if g.kw]
        for g in sorted(variants, key=lambda g: g.count != 0):  # (any count first: a count's own splits from it on loading)
            out.append("%08x %s %d%s" % (lr, " ".join(fmt(v) for v in (g.x, g.y, g.w, g.h, g.dx, g.dy, g.z, g.kw, g.kh, g.ox, g.oy,
                                                                        g.cdx, g.cdy)), g.cols, " %d" % g.count if g.count else ""))
            for row in sorted(g.off):
                if row:
                    out.append("%08x off %d %s" % (lr, row, " ".join(fmt(v) for v in (*g.off[row], g.z, g.kw, g.kh))))
    return out


def shipped(path):
    """The lines of an existing table header (between its quotes)."""
    if not os.path.exists(path):
        return []
    out = []
    for line in open(path, encoding="utf-8"):
        line = line.strip()
        if line.startswith('"') and line.endswith('\\n"'):
            out.append(line[1:-3])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("files", nargs="+")
    ap.add_argument("-o", "--out", default=os.path.join(os.path.dirname(__file__), "..", "runtime", "hle", "menuptr_table.h"))
    a = ap.parse_args()
    table = {}
    read(table, shipped(a.out))
    for f in a.files:
        read(table, open(f, encoding="utf-8").read().splitlines())
    lines = lines_of(table)
    with open(a.out, "w", encoding="utf-8", newline="\n") as o:
        o.write("// Menu geometries the recomp ships (runtime/hle/menuptr.c), learned in playthroughs: made by tools/menus_table.py\n")
        o.write("// from menus_learned.txt files; edit those, not this. Lines as in menus_learned.txt.\n")
        o.write("#pragma once\n\nstatic const char menuptr_shipped[] =\n")
        for line in lines:
            o.write('    "%s\\n"\n' % line)
        o.write('    "";\n')
    print("%d menus, %d lines -> %s" % (len(table), len(lines), a.out), file=sys.stderr)


if __name__ == "__main__":
    main()
