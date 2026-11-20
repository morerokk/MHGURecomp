"""Check host UI translation coverage without game files or external packages."""
import ast
from collections import Counter
from pathlib import Path
import re
import struct
import unittest

ROOT = Path(__file__).resolve().parents[1]
LITERAL = r'"(?:\\.|[^"\\])*"'
PAIR = re.compile(r'\{\s*' + LITERAL + r'\s*,\s*' + LITERAL + r'\s*\}')


def literals(text):
    return [ast.literal_eval(value) for value in re.findall(LITERAL, text)]


def catalog_table(text, marker):
    """The (key, text) pairs of one C table: from its declaration to the closing brace of its initialiser."""
    start = text.index(marker)
    end = text.index('\n    };', start)
    return [tuple(literals(value)) for value in PAIR.findall(text[start:end])]


def font_chars(path):
    """The characters an OpenType font maps (its cmap's Unicode subtables, formats 4 and 12)."""
    data = path.read_bytes()
    u16 = lambda o: struct.unpack_from('>H', data, o)[0]
    u32 = lambda o: struct.unpack_from('>I', data, o)[0]
    tables = {data[12 + 16 * i:16 + 16 * i]: u32(20 + 16 * i) for i in range(u16(4))}
    cmap, chars = tables[b'cmap'], set()
    for i in range(u16(cmap + 2)):
        sub = cmap + u32(cmap + 8 + 8 * i)
        if u16(sub) == 4:
            n = u16(sub + 6) // 2
            ends, starts = sub + 14, sub + 16 + 2 * n
            deltas, offsets = starts + 2 * n, starts + 4 * n
            for s in range(n):
                for c in range(u16(starts + 2 * s), u16(ends + 2 * s) + 1):
                    if c == 0xFFFF:
                        continue
                    ro = u16(offsets + 2 * s)
                    glyph = c + u16(deltas + 2 * s) if not ro else u16(offsets + 2 * s + ro + 2 * (c - u16(starts + 2 * s)))
                    if glyph & 0xFFFF:
                        chars.add(c)
        elif u16(sub) == 12:
            for g in range(u32(sub + 12)):
                chars.update(range(u32(sub + 16 + 12 * g), u32(sub + 20 + 12 * g) + 1))
    return chars


class SettingsLocaleTest(unittest.TestCase):
    def test_fonts_have_the_tables(self):
        """The release's CJK font (a subset: tools/subset_cjk_font.py) has every character the Chinese and Korean lines
        use, the Language list's own names, and the symbols cheats and mods name buttons with."""
        catalog = (ROOT / "runtime/settings_locale.h").read_text(encoding="utf-8")
        font = font_chars(ROOT / "extras/mhgurecomp-cjk.otf")
        for table, name in (("settings_text_zh", "中文"), ("settings_text_ko", "한국어")):
            used = {c for _, text in catalog_table(catalog, table) for c in text if ord(c) > 0x7F} | set(name)
            missing = {c for c in used if ord(c) not in font}
            self.assertGreater(len(used), 400)
            self.assertFalse(missing, f"Not in extras/mhgurecomp-cjk.otf (run tools/subset_cjk_font.py): "
                                      f"{''.join(sorted(missing))}")
        symbols = "↑↓←→↖↗↘↙△○□▲▼◀▶●■◆①②③★☆♪「」ぁあアカ"
        self.assertFalse([c for c in symbols if ord(c) not in font])

    def test_catalog_and_visible_labels(self):
        catalog = (ROOT / "runtime/settings_locale.h").read_text(encoding="utf-8")
        pairs = catalog_table(catalog, 'messages[] = {')
        keys = [en for en, _ in pairs]
        self.assertTrue(pairs)
        self.assertFalse([key for key, count in Counter(keys).items() if count > 1])
        self.assertTrue(all(en.strip() and es.strip() for en, es in pairs))
        self.assertNotIn(chr(0xfffd), catalog)
        translations = dict(pairs)
        self.assertEqual(translations["Misc"], "Varios")
        self.assertEqual(translations["Hunter"], "Cazador")
        self.assertEqual(translations["Reset this tab"], "Restablecer esta pestaña")

        # Simplified Chinese (settings_text_zh): a line for every key the menu has, no key twice, nothing empty
        zh = catalog_table(catalog, 'settings_text_zh')
        zh_keys = [en for en, _ in zh]
        self.assertTrue(zh)
        self.assertFalse([key for key, count in Counter(zh_keys).items() if count > 1])
        self.assertTrue(all(en.strip() and text.strip() for en, text in zh))
        self.assertFalse(set(keys) - set(zh_keys), f"No Chinese for: {sorted(set(keys) - set(zh_keys))}")
        zh_translations = dict(zh)
        self.assertEqual(zh_translations["Misc"], "杂项")
        self.assertEqual(zh_translations["Default"], "默认")
        self.assertEqual(zh_translations["Reset this tab"], "重置本页")

        # Korean (settings_text_ko): the same, and its format strings take the same arguments as the English ones
        ko = catalog_table(catalog, 'settings_text_ko')
        ko_keys = [en for en, _ in ko]
        self.assertTrue(ko)
        self.assertFalse([key for key, count in Counter(ko_keys).items() if count > 1])
        self.assertTrue(all(en.strip() and text.strip() for en, text in ko))
        self.assertFalse(set(keys) - set(ko_keys), f"No Korean for: {sorted(set(keys) - set(ko_keys))}")
        spec = re.compile(r'%[-+ #0]*\d*(?:\.\d+)?[a-zA-Z%]')
        for table in (zh, ko):
            self.assertFalse([en for en, text in table if spec.findall(en) != spec.findall(text)])
        ko_translations = dict(ko)
        self.assertEqual(ko_translations["Misc"], "기타")
        self.assertEqual(ko_translations["Default"], "기본값")
        self.assertEqual(ko_translations["Hub"], "집회소")

        source = (ROOT / "runtime/settings_panel.cpp").read_text(encoding="utf-8")
        source = re.sub(r'//[^\n]*|/\*.*?\*/', '', source, flags=re.S)
        # INI keys looked up in the settings table (ini("Mode"), lo("FPS")...) are not shown
        source = re.sub(r'\b(?:ini|cold|lo|hi)\(' + LITERAL + r'\)', '', source)
        # nor is a C linkage specification (extern "C" ...)
        source = re.sub(r'\bextern\s+"C"', '', source)
        unchanged = {"English", "Español", "中文", "한국어", "Chinese", "Korean", "Xbox", "Switch", "PlayStation",
                     "FSR", "MSAA", "FXAA", "Discord", "Settings##main", "Settings##dropdown", "Settings##bind",
                     "A", "B", "X", "Y", "L", "R", "ZL", "ZR",
                     "Default: ",
                     "MHGU Player"}  # (a language's own name, the Switch buttons' keybinds, a help line's prefix, the room name others see)
        visible = {text for text in literals(source) if text and
                   (text[0].isupper() or " " in text) and any(c.isalpha() for c in text)}
        self.assertFalse(visible - set(keys) - unchanged,
                         f"Untranslated UI labels: {sorted(visible - set(keys) - unchanged)}")

        # the Game actions screen's labels (runtime/actions.c: an action's and a merged row's)
        actions = (ROOT / "runtime/actions.c").read_text(encoding="utf-8")
        labels = [ast.literal_eval(m.group(1)) for m in re.finditer(
            r'\{"\w+",\s*ACT_VIEW_\w+,[^"]*?(' + LITERAL + r')', actions)]
        labels += [ast.literal_eval(m.group(1)) for m in re.finditer(
            r'\{"\w+",\s*(' + LITERAL + r'),\s*\d,\s*ACT_', actions)]
        self.assertGreater(len(labels), 80)
        self.assertFalse(set(labels) - set(keys), f"Untranslated action labels: {sorted(set(labels) - set(keys))}")
        for group in ("Hunt and town", "Menus"):
            self.assertIn(group, keys)

        # notices on screen (runtime/net/ldn.c: UI_TEXT("..."))
        notices = literals(" ".join(re.findall(r'UI_TEXT\((' + LITERAL + r')\)',
                                               (ROOT / "runtime/net/ldn.c").read_text(encoding="utf-8"))))
        self.assertTrue(notices)
        self.assertFalse(set(notices) - set(keys), f"Untranslated notices: {sorted(set(notices) - set(keys))}")

        errors = (ROOT / "runtime/config_store.c").read_text(encoding="utf-8")
        messages = [literals(value)[-1] for value in re.findall(
            r'fail\([^;]*?' + LITERAL + r'\)', errors)]
        # the settings table's messages (runtime/config.c g_settings)
        table = (ROOT / "runtime/config.c").read_text(encoding="utf-8")
        messages += [literals(value)[0] for value in re.findall(r'#define ERR_\w+ +(' + LITERAL + ')', table)]
        self.assertTrue(messages)
        self.assertFalse(set(messages) - set(keys))


if __name__ == "__main__":
    unittest.main()
