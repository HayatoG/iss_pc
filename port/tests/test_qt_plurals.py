"""Russian plural forms (launcher/plural_ru.py) and their use in the Qt and Tk translators."""
import sys
import unittest

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import plural_ru  # noqa: E402
from qt import logic  # noqa: E402

PT = {'ui_language': 'pt_BR'}
EN = {'ui_language': 'en_US'}
RU = {'ui_language': 'ru_RU'}


class FormTests(unittest.TestCase):
    def test_russian_rules(self):
        cases = {1: 0, 21: 0, 101: 0, 2: 1, 3: 1, 4: 1, 22: 1, 0: 2, 5: 2, 11: 2, 12: 2, 14: 2, 111: 2, 112: 2}
        for n, expected in cases.items():
            self.assertEqual(expected, plural_ru.form(n), n)

    def test_pick_uses_the_three_forms(self):
        text = '{n} сохранение|{n} сохранения|{n} сохранений'
        self.assertEqual('1 сохранение', plural_ru.pick(text.format(n=1), 1))
        self.assertEqual('3 сохранения', plural_ru.pick(text.format(n=3), 3))
        self.assertEqual('5 сохранений', plural_ru.pick(text.format(n=5), 5))
        self.assertEqual('plain', plural_ru.pick('plain', 5))


class TranslatorTests(unittest.TestCase):
    def test_count_in_all_languages(self):
        pt, en, ru = logic.Translator(PT), logic.Translator(EN), logic.Translator(RU)
        self.assertEqual('1 save', pt.count(1))
        self.assertEqual('3 saves', pt.count(3))
        self.assertEqual('1 save', en.count(1))
        self.assertEqual('1 сохранение', ru.count(1))
        self.assertEqual('3 сохранения', ru.count(3))
        self.assertEqual('5 сохранений', ru.count(5))
        self.assertEqual('21 сохранение', ru.count(21))
        self.assertEqual('11 сохранений', ru.count(11))

    def test_profile_changes(self):
        ru = logic.Translator(RU)
        self.assertEqual('Профиль: пользовательский (1 изменение)', ru.plural('exp_profile_custom', 1))
        self.assertEqual('Профиль: пользовательский (2 изменения)', ru.plural('exp_profile_custom', 2))
        self.assertEqual('Профиль: пользовательский (5 изменений)', ru.plural('exp_profile_custom', 5))

    def test_english_keys_without_forms_are_unchanged(self):
        self.assertEqual('Keep the last 1 backups', logic.Translator(EN).plural('backups_keep', 1))
