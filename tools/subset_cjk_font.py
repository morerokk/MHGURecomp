#!/usr/bin/env python3
"""Make extras/mhgurecomp-cjk.otf, the glyphs the menus' font (ImGui's ProggyVector: ASCII and Latin-1) lacks, shipped
beside the program and merged into the menus' font from the start (runtime/overlay.cpp), from Noto Sans CJK SC.

It is for the menu's own translations (runtime/settings_locale.h: Chinese, Korean) and for the text it shows but does
not write: cheat names (SELECT+↑), mod names, Forge plugins' windows. The full font is 16 MB, and every player
downloads the release's copy with each update; the subset (1.7 MB) keeps:
- the 2,500 Chinese characters in common use (ImGui's GetGlyphRangesChineseSimplifiedCommon, read from
  third_party/cimgui) and every character of the Chinese table;
- the 2,350 Hangul syllables of KS X 1001 (the ones EUC-KR has), Hangul compatibility jamo and every character of the
  Korean table (Noto Sans CJK's Hangul is Noto Sans KR's);
- hiragana and katakana, CJK punctuation and full-width forms;
- the symbols controllers and the game are described with: arrows (the D-pad), geometric shapes (△○□ ▲▼◀▶), ①②,
  ★♪, box drawing, letterlike and number forms, math operators and technical symbols.
Uncommon hanzi (Traditional ones among them) and kanji are left out: Big5's level 1 alone would make it 4.4 MB.
tests/test_settings_locale.py fails when a line uses a character the font has not: run this again.

Noto Sans CJK is under the SIL Open Font License 1.1 (THIRD_PARTY_NOTICES), with Reserved Font Name 'Source': the
subset keeps the font's own name (Noto Sans CJK SC), which does not use it.

    pip install fonttools
    curl -LO https://github.com/notofonts/noto-cjk/raw/main/Sans/OTF/SimplifiedChinese/NotoSansCJKsc-Regular.otf
    python tools/subset_cjk_font.py NotoSansCJKsc-Regular.otf

(the release's: version 2.004, SHA-256 2c76254f6fc379fddfce0a7e84fb5385bb135d3e399294f6eeb6680d0365b74b)
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = "extras/mhgurecomp-cjk.otf"
BLOCKS = [
    (0x2000, 0x206F),  # General Punctuation
    (0x2100, 0x214F),  # Letterlike Symbols
    (0x2150, 0x218F),  # Number Forms
    (0x2190, 0x21FF),  # Arrows
    (0x2200, 0x22FF),  # Mathematical Operators
    (0x2300, 0x23FF),  # Miscellaneous Technical
    (0x2460, 0x24FF),  # Enclosed Alphanumerics
    (0x2500, 0x257F),  # Box Drawing
    (0x25A0, 0x25FF),  # Geometric Shapes
    (0x2600, 0x26FF),  # Miscellaneous Symbols
    (0x3000, 0x303F),  # CJK Symbols and Punctuation
    (0x3040, 0x309F),  # Hiragana
    (0x30A0, 0x30FF),  # Katakana
    (0xFF00, 0xFFEF),  # Halfwidth and Fullwidth Forms
    (0xFFFD, 0xFFFD),  # the replacement character
]


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


def make(source):
    from fontTools import subset
    from fontTools.ttLib import TTFont
    font = TTFont(source)
    have = set(font.getBestCmap())
    used = table_chars("settings_text_zh") | table_chars("settings_text_ko")
    wanted = used | imgui_common() | ks_x_1001()
    for lo, hi in BLOCKS:
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
    path = ROOT / OUT
    font.save(path)
    print("%s: %d characters, %d bytes" % (OUT, len(wanted & have), path.stat().st_size))


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source", help="NotoSansCJKsc-Regular.otf")
    make(parser.parse_args().source)


if __name__ == "__main__":
    main()
