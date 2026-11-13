"""Guide image selection by interface language (launcher/qt/logic.py guide_path)."""
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
from qt import logic  # noqa: E402


class GuidePathTests(unittest.TestCase):
    def guide(self, folder, kind, lang):
        return logic.guide_path(kind, lang, folder)

    def test_russian_prefers_russian(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            (base / 'inFAMOUS-controle-xbox-ru-RU.png').write_bytes(b'ru')
            (base / 'inFAMOUS-controle-xbox-en-US.png').write_bytes(b'en')
            self.assertEqual(base / 'inFAMOUS-controle-xbox-ru-RU.png', self.guide(base, 'xbox', 'ru'))

    def test_russian_falls_back_to_english(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            (base / 'inFAMOUS-controle-switch-en-US.png').write_bytes(b'en')
            self.assertEqual(base / 'inFAMOUS-controle-switch-en-US.png', self.guide(base, 'switch', 'ru'))

    def test_russian_without_any_image_is_none(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertIsNone(self.guide(Path(folder), '8bitdo', 'ru'))

    def test_portuguese_uses_brazilian(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            (base / 'inFAMOUS-controle-xbox-pt-BR.png').write_bytes(b'pt')
            (base / 'inFAMOUS-controle-xbox-en-US.png').write_bytes(b'en')
            self.assertEqual(base / 'inFAMOUS-controle-xbox-pt-BR.png', self.guide(base, 'xbox', 'pt'))

    def test_english_uses_english(self):
        with tempfile.TemporaryDirectory() as folder:
            base = Path(folder)
            (base / 'inFAMOUS-controle-8bitdo-pt-BR.png').write_bytes(b'pt')
            (base / 'inFAMOUS-controle-8bitdo-en-US.png').write_bytes(b'en')
            self.assertEqual(base / 'inFAMOUS-controle-8bitdo-en-US.png', self.guide(base, '8bitdo', 'en'))

    def test_every_shipped_guide_has_all_three_languages(self):
        for kind, _ in logic.GUIDE_KINDS:
            for lang in ('ru', 'pt', 'en'):
                self.assertTrue(self.guide(logic.GUIDE_DIR, kind, lang), (kind, lang))


if __name__ == '__main__':
    unittest.main()
