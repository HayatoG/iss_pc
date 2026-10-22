"""Game language that follows the system: infamous_run.system_game_language / resolve_game_language,
used by build_env, the Tk launcher and the Qt launcher. No test depends on the machine's locale."""
import os
import sys
import unittest
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import infamous_run as run  # noqa: E402


class SystemGameLanguageTests(unittest.TestCase):
    def test_locale_mapping(self):
        cases = {'pt_BR.UTF-8': 'pt_BR', 'pt_PT': 'pt_PT', 'pt': 'pt_BR', 'es_MX': 'es_LA', 'es_ES': 'es',
                 'es': 'es_LA', 'fr_FR': 'fr', 'fr_CA': 'fr_CA', 'tr_TR': 'tr', 'ru_RU': 'ru', 'en_US': 'en',
                 'en_GB': 'en_GB', 'de_DE': 'de', 'it_IT': 'it', 'pl_PL': 'pl', 'nb_NO': 'no', 'zh_TW': 'zh_TW',
                 'zh_CN': 'zh_CN', 'ja_JP': 'ja', 'C': 'en', 'POSIX': 'en', 'pt-BR': 'pt_BR', 'ru-RU': 'ru',
                 'Portuguese_Brazil': 'pt_BR', 'Spanish_Spain': 'es', 'Italian_Italy': 'it', 'eo': 'en'}
        for value, expected in cases.items():
            self.assertEqual(expected, run.system_game_language({'LANG': value}), value)

    def test_variable_order_and_empty(self):
        self.assertEqual('ru', run.system_game_language({'LC_ALL': 'ru_RU', 'LC_MESSAGES': 'tr_TR', 'LANG': 'fr_FR'}))
        self.assertEqual('tr', run.system_game_language({'LC_MESSAGES': 'tr_TR', 'LANG': 'fr_FR'}))
        with mock.patch('locale.getlocale', return_value=(None, None)):
            self.assertEqual('en', run.system_game_language({}))
            self.assertEqual('en', run.system_game_language({'LANG': ''}))
        with mock.patch('locale.getlocale', return_value=('es_AR', 'UTF-8')):
            self.assertEqual('es_LA', run.system_game_language({}))
        with mock.patch('locale.getlocale', side_effect=ValueError):
            self.assertEqual('en', run.system_game_language({}))

    def test_default_is_auto(self):
        self.assertEqual('auto', run.DEFAULT_SETTINGS['game_language'])

    def test_resolve(self):
        env = {'LANG': 'tr_TR.UTF-8'}
        self.assertEqual('tr', run.resolve_game_language({'game_language': 'auto'}, env))
        self.assertEqual('tr', run.resolve_game_language({}, env))
        self.assertEqual('pt_BR', run.resolve_game_language({'game_language': 'pt_BR'}, env))   # kept
        self.assertEqual('tr', run.resolve_game_language({'game_language': 'klingon'}, env))    # invalid
        # No game folder: the CUSA00223 set, which has neither German nor French from France
        self.assertEqual('en', run.resolve_game_language({'game_language': 'auto'}, {'LANG': 'de_DE'}))
        self.assertEqual('fr_CA', run.resolve_game_language({'game_language': 'auto'}, {'LANG': 'fr_FR'}))
        self.assertEqual('pl', run.resolve_game_language({'game_language': 'pl'}, env))         # kept

    def test_placeholder_languages_are_skipped(self):
        """Issue #504: a dump from an English console has 3-byte Brazilian files; 'auto' on a pt-BR
        system must not pick them. A missing file keeps the language."""
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as tmp:
            cache = Path(tmp) / 'art' / 'cache'
            cache.mkdir(parents=True)
            (cache / 'lang_brazilian_text.xpps').write_bytes(b'\0\0\0')
            (cache / 'lang_english_text.xpps').write_bytes(b'\0' * 4096)
            settings = {'game_dir': tmp, 'game_language': 'auto'}
            self.assertEqual(['en', 'es_LA', 'fr_CA', 'tr', 'ru'], run.game_languages(tmp))   # latino: no file
            self.assertEqual('en', run.resolve_game_language(settings, {'LANG': 'pt_BR.UTF-8'}))
            self.assertNotIn('pt_BR', run.game_language_choices(settings))
            self.assertIn('pt_BR', run.game_language_choices(dict(settings, game_language='pt_BR')))  # kept
            for name in ('english', 'latino', 'french'):
                (cache / f'lang_{name}_text.xpps').write_bytes(b'\0\0\0')
            self.assertEqual(['tr', 'ru'], run.game_languages(tmp))
        self.assertEqual(list(run._DEFAULT_LANGUAGES), run.game_languages(''))

    def test_every_ps4_language_has_a_choice(self):
        self.assertEqual(list(range(30)), sorted(run.GAME_LANGUAGES.values()))
        with mock.patch.dict(os.environ):
            os.environ.pop('ISS_LANGUAGE', None)
            self.assertEqual('16', run.build_env(dict(run.DEFAULT_SETTINGS, game_language='pl'))['ISS_LANGUAGE'])
            self.assertEqual('3', run.build_env(dict(run.DEFAULT_SETTINGS, game_language='es'))['ISS_LANGUAGE'])

    def test_build_env(self):
        with mock.patch.dict(os.environ, {'LC_ALL': 'es_MX.UTF-8'}):
            os.environ.pop('ISS_LANGUAGE', None)
            settings = dict(run.DEFAULT_SETTINGS)
            self.assertEqual('20', run.build_env(settings)['ISS_LANGUAGE'])
            self.assertEqual('17', run.build_env(dict(settings, game_language='pt_BR'))['ISS_LANGUAGE'])
            os.environ['ISS_LANGUAGE'] = '8'                    # the environment wins
            self.assertEqual('8', run.build_env(settings)['ISS_LANGUAGE'])

    def test_tk_and_qt_use_the_same_function(self):
        tk = (ROOT / 'launcher' / 'infamous_launcher.py').read_text(encoding='utf-8')
        qt = (ROOT / 'launcher' / 'qt' / 'backend.py').read_text(encoding='utf-8')
        self.assertIn('tlg_run.resolve_game_language(self.settings)', tk)
        self.assertIn('tlg_run.resolve_game_language(self.settings)', qt)


if __name__ == '__main__':
    unittest.main()
