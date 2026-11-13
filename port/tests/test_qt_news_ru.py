"""Russian news and release notes (launcher/qt/logic.py, releases.py)."""
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
from qt import logic, releases  # noqa: E402

RU = {'ui_language': 'ru_RU'}
README = ("# x\n\n## What's new\n\n- **English thing:** detail\n\n## Что нового\n\n"
          '- **Новый лаунчер (#51):** подробности.\n- Второй пункт без жирного.\n'
          '- **Третья [со ссылкой](http://x)** дальше\n- **Четвёртый:** не входит\n\n'
          "## Status\n- **вне:** x\n\n## Novidades\n- **Coisa:** x\n")


class RussianNewsTests(unittest.TestCase):
    def test_russian_section_is_parsed(self):
        self.assertEqual(['Новый лаунчер', 'Второй пункт без жирного', 'Третья со ссылкой'],
                         logic.parse_news(README, False, russian=True))

    def test_portuguese_and_english_sections_still_work(self):
        self.assertEqual(['English thing'], logic.parse_news(README, False))
        self.assertEqual(['Coisa'], logic.parse_news(README, True))

    def test_news_uses_the_parsed_russian_section(self):
        with tempfile.TemporaryDirectory() as folder:
            readme = Path(folder) / 'README.md'
            readme.write_text(README, encoding='utf-8')
            t = logic.Translator(RU)
            self.assertEqual(['Новый лаунчер', 'Второй пункт без жирного', 'Третья со ссылкой'],
                             logic.news(t, False, readme=readme, russian=True))

    def test_fallback_only_when_the_section_is_missing(self):
        with tempfile.TemporaryDirectory() as folder:
            readme = Path(folder) / 'README.md'
            readme.write_text("## What's new\n\n- **English thing:** detail\n", encoding='utf-8')
            t = logic.Translator(RU)
            self.assertEqual(['Исправления света, огня и дыма', 'Окно экспериментальных исправлений',
                              'Меньше утечек памяти'], logic.news(t, False, readme=readme, russian=True))

    def test_fallback_when_the_readme_is_missing(self):
        t = logic.Translator(RU)
        self.assertEqual(3, len(logic.news(t, False, readme='/no/such/README.md', russian=True)))


class RussianReleaseNotesTests(unittest.TestCase):
    def test_russian_blocks_are_extracted(self):
        body = '<!-- notes:ru-RU -->\n- Исправлено (#7)\n<!-- /notes -->'
        self.assertEqual('• Исправлено (#7)', releases.extract_notes(body, False, russian=True))

    def test_russian_section_titles(self):
        for title in ('## Что нового', '## Новости'):
            body = f'{title}\n\n- Исправлено\n\n## Другое\n\n- не входит\n'
            self.assertEqual('• Исправлено', releases.extract_notes(body, False, russian=True))

    def test_english_notes_when_there_is_no_russian_block(self):
        body = "## What's new\n\n- English only\n\n## Novidades\n\n- Só português\n"
        self.assertEqual('• English only', releases.extract_notes(body, False, russian=True))

    def test_russian_prefers_its_block_over_the_other_languages(self):
        body = ('<!-- notes:en-US -->\n- English\n<!-- /notes -->\n'
                '<!-- notes:ru-RU -->\n- Русский\n<!-- /notes -->\n'
                '<!-- notes:pt-BR -->\n- Português\n<!-- /notes -->')
        self.assertEqual('• Русский', releases.extract_notes(body, False, russian=True))


if __name__ == '__main__':
    unittest.main()
