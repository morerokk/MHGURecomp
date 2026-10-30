"""Check host UI translation coverage without game files or external packages."""
import ast
from collections import Counter
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[1]
LITERAL = r'"(?:\\.|[^"\\])*"'


def literals(text):
    return [ast.literal_eval(value) for value in re.findall(LITERAL, text)]


class SettingsLocaleTest(unittest.TestCase):
    def test_catalog_and_visible_labels(self):
        catalog = (ROOT / "runtime/settings_locale.h").read_text(encoding="utf-8")
        raw_entries = [literals(value) for value in re.findall(r'\{([^{}]+)\}', catalog)]
        entries = [e for e in raw_entries if len(e) == 7]
        keys = [e[0] for e in entries]
        self.assertTrue(entries)
        self.assertFalse([key for key, count in Counter(keys).items() if count > 1])
        self.assertTrue(all(all(text.strip() for text in e) for e in entries))
        self.assertNotIn(chr(0xfffd), catalog)
        translations = {e[0]: e[2] for e in entries}
        self.assertEqual(translations["Miscellaneous"], "Misceláneos")
        self.assertEqual(translations["Hunter"], "Cazador")
        self.assertEqual(translations["Reset this tab"], "Restablecer esta pestaña")

        source = (ROOT / "runtime/settings_panel.cpp").read_text(encoding="utf-8")
        source = re.sub(r'//[^\n]*|/\*.*?\*/', '', source, flags=re.S)
        # INI keys looked up in the settings table (cold("Mode"), lo("FPS")...) are not shown
        source = re.sub(r'\b(?:cold|lo|hi)\(' + LITERAL + r'\)', '', source)
        unchanged = {"English", "Français", "Español", "Deutsch", "Italiano",
                     "Traditional_Chinese", "Simplified_Chinese",
                     "Xbox", "Switch", "PlayStation",
                     "FSR", "FSR 3.1", "FSR 4", "MSAA", "FXAA", "Settings##main"}
        visible = {text for text in literals(source) if text and
                   (text[0].isupper() or " " in text) and any(c.isalpha() for c in text)}
        self.assertFalse(visible - set(keys) - unchanged,
                         f"Untranslated UI labels: {sorted(visible - set(keys) - unchanged)}")

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
