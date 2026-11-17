#!/usr/bin/env python3
"""Make the settings menu's CJK fonts (runtime/settings_locale.h's Chinese and Korean lines), shipped beside the
program and merged into the menu's font from the start (runtime/overlay.cpp), from the full Noto Sans SC and KR.

The full fonts are 8.3 and 4.6 MB, and every player downloads the release's copies with each update. The subsets keep:
- extras/mhgurecomp-cjk.otf (Noto Sans SC): the 2,500 characters in common use (ImGui's
  GetGlyphRangesChineseSimplifiedCommon, read from third_party/cimgui) and every character of the Chinese table;
- extras/mhgurecomp-kr.otf (Noto Sans KR): the 2,350 Hangul syllables of KS X 1001 (the ones EUC-KR has), Hangul
  compatibility jamo and every character of the Korean table;
and CJK punctuation and full-width forms in both. tests/test_settings_locale.py fails when a line uses a character its
font has not: run this again.

Noto Sans SC and KR are under the SIL Open Font License 1.1 (THIRD_PARTY_NOTICES), with Reserved Font Name 'Source': a
subset keeps the fonts' own names (Noto Sans SC / KR), which do not use it.

    pip install fonttools
    curl -LO https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf
    curl -LO https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/SubsetOTF/KR/NotoSansKR-Regular.otf
    python tools/subset_cjk_font.py --sc NotoSansSC-Regular.otf --kr NotoSansKR-Regular.otf

(either one alone remakes that font only; the release's: version 2.004, SHA-256
 SC faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9,
 KR 69975a0ac8472717870aefeab0a4d52739308d90856b9955313b2ad5e0148d68)
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PUNCTUATION = [(0x2000, 0x206F), (0x3000, 0x303F), (0xFF00, 0xFFEF), (0xFFFD, 0xFFFD)]


def imgui_common():
    """ImGui's 2,500 regularly used Simplified Chinese characters (accumulative offsets from U+4E00)."""
    src = (ROOT / "third_party/cimgui/imgui/imgui_draw.cpp").read_text(encoding="utf-8")
    body = src[src.index("ImFontAtlas::GetGlyphRangesChineseSimplifiedCommon()"):]
    body = body[body.index("accumulative_offsets_from_0x4E00[]"):]
    body = body[body.index("{") + 1:body.index("}")]
    chars, c = set(), 0x4E00
    for offset in map(int, re.findall(r"\d+", body)):
        c += offset
        chars.add(c)
    return chars


def ks_x_1001():
    """The 2,350 Hangul syllables of KS X 1001 (two bytes in EUC-KR; Python spells the others with jamo)."""
    return {c for c in range(0xAC00, 0xD7A4) if len(chr(c).encode("euc_kr")) == 2} | set(range(0x3130, 0x3190))


def table_chars(function):
    """Every non-ASCII character of one table of runtime/settings_locale.h (settings_text_zh, settings_text_ko)."""
    text = (ROOT / "runtime/settings_locale.h").read_text(encoding="utf-8")
    start = text.index(function + "(")
    table = text[start:text.index("\n    };", start)]
    return {ord(c) for c in table if ord(c) > 0x7F}


FONTS = {  # option: (output, the table's function, the characters beyond the table)
    "sc": ("extras/mhgurecomp-cjk.otf", "settings_text_zh", imgui_common),
    "kr": ("extras/mhgurecomp-kr.otf", "settings_text_ko", ks_x_1001),
}


def make(source, out, table, more):
    from fontTools import subset
    from fontTools.ttLib import TTFont
    font = TTFont(source)
    have = set(font.getBestCmap())
    used = table_chars(table)
    wanted = used | more()
    for lo, hi in PUNCTUATION:
        wanted |= set(range(lo, hi + 1))
    missing = sorted(c for c in used if c not in have)
    if missing:
        sys.exit("not in %s: %s" % (source, "".join(map(chr, missing))))
    opts = subset.Options()
    opts.layout_features = ["*"]
    opts.name_IDs = ["*"]
    opts.name_languages = ["*"]
    opts.notdef_outline = True
    opts.drop_tables += ["DSIG"]  # (a signature that no longer matches the subset)
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=sorted(wanted & have))
    sub.subset(font)
    path = ROOT / out
    font.save(path)
    print("%s: %d characters, %d bytes" % (out, len(wanted & have), path.stat().st_size))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sc", help="NotoSansSC-Regular.otf: remake extras/mhgurecomp-cjk.otf")
    parser.add_argument("--kr", help="NotoSansKR-Regular.otf: remake extras/mhgurecomp-kr.otf")
    args = parser.parse_args()
    if not args.sc and not args.kr:
        parser.error("give --sc and/or --kr")
    for option, (out, table, more) in FONTS.items():
        if getattr(args, option):
            make(getattr(args, option), out, table, more)


if __name__ == "__main__":
    main()
