#!/usr/bin/env python3
"""Make extras/mhgurecomp-cjk.otf, the Chinese glyphs of the settings menu's translation (runtime/settings_locale.h),
from the full Noto Sans SC.

The full font is 8.3 MB, and every player downloads the release's copy with each update. The subset keeps the 2,500
characters in common use (ImGui's GetGlyphRangesChineseSimplifiedCommon, read from third_party/cimgui), every
character the Chinese table uses (tests/test_settings_locale.py fails when a line uses one the font has not: run this
again), and CJK punctuation and full-width forms. Noto Sans SC is under the SIL Open Font License 1.1, which allows a
subset (THIRD_PARTY_NOTICES); it has no Reserved Font Name.

    pip install fonttools
    curl -LO https://raw.githubusercontent.com/notofonts/noto-cjk/main/Sans/SubsetOTF/SC/NotoSansSC-Regular.otf
    python tools/subset_cjk_font.py NotoSansSC-Regular.otf

(the release's: Noto Sans SC 2.004, SHA-256 faa6c9df652116dde789d351359f3d7e5d2285a2b2a1f04a2d7244df706d5ea9)
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "extras/mhgurecomp-cjk.otf"
RANGES = [(0x2000, 0x206F), (0x3000, 0x303F), (0xFF00, 0xFFEF), (0xFFFD, 0xFFFD)]


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


def table_chars():
    """Every non-ASCII character of the Chinese table (settings_text_zh)."""
    text = (ROOT / "runtime/settings_locale.h").read_text(encoding="utf-8")
    start = text.index("settings_text_zh(")
    table = text[start:text.index("\n    };", start)]
    return {ord(c) for c in table if ord(c) > 0x7F}


def main():
    from fontTools import subset
    from fontTools.ttLib import TTFont
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    font = TTFont(sys.argv[1])
    have = set(font.getBestCmap())
    wanted = imgui_common() | table_chars()
    for lo, hi in RANGES:
        wanted |= set(range(lo, hi + 1))
    missing = sorted(c for c in table_chars() if c not in have)
    if missing:
        sys.exit("not in %s: %s" % (sys.argv[1], "".join(map(chr, missing))))
    opts = subset.Options()
    opts.layout_features = ["*"]
    opts.name_IDs = ["*"]
    opts.name_languages = ["*"]
    opts.notdef_outline = True
    opts.drop_tables += ["DSIG"]  # (a signature that no longer matches the subset)
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=sorted(wanted & have))
    sub.subset(font)
    font.save(OUT)
    print("%s: %d characters, %d bytes" % (OUT.relative_to(ROOT), len(wanted & have), OUT.stat().st_size))


if __name__ == "__main__":
    main()
