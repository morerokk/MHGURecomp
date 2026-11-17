"""Check host UI translation coverage without game files or external packages."""
import ast
from collections import Counter
from pathlib import Path
import re
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


class SettingsLocaleTest(unittest.TestCase):
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
        unchanged = {"English", "Español", "中文", "한국어", "Xbox", "Switch", "PlayStation",
                     "FSR", "MSAA", "FXAA", "Discord", "Settings##main", "Settings##dropdown", "Settings##bind",
                     "A", "B", "X", "Y", "L", "R", "ZL", "ZR",
                     "Default: "}  # (a language's own name, the Switch buttons' keybinds, a help line's prefix)
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
