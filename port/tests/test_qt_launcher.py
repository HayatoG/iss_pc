"""The Qt launcher's logic (launcher/qt/logic.py, backend.py): translations, chips, news, saves,
fix groups. The pure-Python parts need no PySide6; the Backend tests skip without it."""
import os
import json
import re
import struct
import sys
import tempfile
import unittest
from unittest import mock
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
from qt import logic  # noqa: E402

QT = ROOT / 'launcher' / 'qt'
STRINGS = json.loads((QT / 'i18n' / 'strings.json').read_text(encoding='utf-8'))
PT = {'ui_language': 'pt_BR'}
EN = {'ui_language': 'en_US'}


def placeholders(text):
    return set(re.findall(r'{(\w+)}', text))


def make_sfo(entries):
    """A minimal PSF (param.sfo / param.bin) with UTF-8 string entries."""
    keys, values, index = b'', b'', b''
    for key, value in entries.items():
        data = value.encode() + b'\0'
        index += struct.pack('<HHIII', len(keys), 0x0204, len(data), len(data), len(values))
        keys += key.encode() + b'\0'
        values += data
    start = 20 + len(index)
    return struct.pack('<4sIIII', b'\0PSF', 0x101, start, start + len(keys), len(entries)) + index + keys + values


class TranslationTests(unittest.TestCase):
    def test_every_key_has_both_languages(self):
        for key, entry in STRINGS.items():
            self.assertEqual({'en', 'pt'}, set(entry), key)
            self.assertTrue(entry['en'].strip() and entry['pt'].strip(), key)

    def test_placeholders_match_between_languages(self):
        for key, entry in STRINGS.items():
            self.assertEqual(placeholders(entry['en']), placeholders(entry['pt']), key)

    def test_no_key_hides_a_tk_key_with_other_placeholders(self):
        tk = logic.tk_text()
        for key in set(STRINGS) & set(tk):
            self.assertEqual(placeholders(tk[key][0]), placeholders(STRINGS[key]['en']), key)

    def test_keys_used_by_the_interface_exist(self):
        tk = logic.tk_text()
        used = set()
        for path in list(QT.rglob('*.qml')) + [QT / 'backend.py', QT / 'logic.py']:
            text = path.read_text(encoding='utf-8')
            used |= set(re.findall(r'(?:tr\(|t_\(|\bt\()["\'](\w+)["\']', text))
        used |= {'nav_' + n for n in ('play', 'graphics', 'controls', 'fixes', 'saves', 'guides', 'support', 'updates', 'advanced')}
        for key in (k for k in used if not k.endswith('_')):
            self.assertTrue(key in STRINGS or key in tk, key)

    def test_translator_picks_language_and_falls_back_to_tk_text(self):
        pt, en = logic.Translator(PT), logic.Translator(EN)
        self.assertEqual('Jogar', pt('nav_play'))
        self.assertEqual('Play', en('nav_play'))
        self.assertEqual(logic.tk_text()['exp_live'][1], pt('exp_live'))   # reused from the Tk launcher
        self.assertEqual('Cache de shaders: 1 MB', pt('chip_cache', size='1 MB'))
        self.assertEqual('no_such_key', en('no_such_key'))


class ChipTests(unittest.TestCase):
    def chips(self, settings=PT, **kw):
        kw.setdefault('states', {})
        kw.setdefault('problems', [])
        kw.setdefault('cache_bytes', 0)
        settings = dict(settings, game_dir=kw.pop('game_dir', '/some/game'))
        return logic.compute_chips(settings, logic.Translator(settings), **kw)

    def test_everything_ready(self):
        chips, ready = self.chips(gpu=('NVIDIA GeForce RTX 2060', '1.3'), pad='DualSense',
                                  cache_bytes=218 * 2 ** 20)
        self.assertTrue(ready)
        texts = [c['text'] for c in chips]
        self.assertIn('NVIDIA GeForce RTX 2060 · Vulkan 1.3', texts)
        self.assertIn('Pasta do jogo OK · update 1.07', texts)
        self.assertIn('Perfil: Recomendado', texts)
        self.assertIn('Controle: DualSense', texts)
        self.assertTrue(any(t.startswith('Cache de shaders: 218') for t in texts))
        self.assertTrue(all(c['level'] == 'ok' for c in chips))

    def test_missing_game_folder_blocks(self):
        chips, ready = self.chips(game_dir='', gpu=None)
        self.assertFalse(ready)
        self.assertEqual('bad', chips[1]['level'])
        self.assertEqual('warn', chips[0]['level'])

    def test_blocking_problem_names_the_file(self):
        chips, ready = self.chips(problems=[('missing', 'eboot.bin')], gpu='checking')
        self.assertFalse(ready)
        self.assertIn('eboot.bin', chips[1]['text'])

    def test_unbuilt_port_is_its_own_chip_not_a_game_problem(self):
        chips, ready = self.chips(problems=[('not_built', '/x/tlg-probe')])
        self.assertFalse(ready)
        texts = [c['text'] for c in chips]
        self.assertIn('Pasta do jogo OK · update 1.07', texts)
        self.assertIn('Port não compilado (rode o build.sh)', texts)

    def test_profile_counts_changes_like_the_tk_window(self):
        chips, _ = self.chips(states={'BP06': '1', 'BP01': '0'})
        self.assertIn('Perfil: personalizado (2 mudanças)', [c['text'] for c in chips])

    def test_english(self):
        chips, _ = self.chips(EN, gpu=('GPU', ''), pad=None)
        texts = [c['text'] for c in chips]
        self.assertIn('Game folder OK · update 1.07', texts)
        self.assertIn('No controller detected', texts)


class ProbeParsingTests(unittest.TestCase):
    SUMMARY = """Devices:
========
GPU0:
\tapiVersion         = 1.4.318
\tdriverVersion      = 595.1.0.0
\tdeviceType         = PHYSICAL_DEVICE_TYPE_CPU
\tdeviceName         = llvmpipe (LLVM 19.1.7, 256 bits)
GPU1:
\tapiVersion         = 1.3.289
\tdeviceType         = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU
\tdeviceName         = NVIDIA GeForce RTX 2060
"""

    def test_prefers_the_discrete_gpu(self):
        self.assertEqual(('NVIDIA GeForce RTX 2060', '1.3'), logic.parse_vulkaninfo(self.SUMMARY))

    def test_no_devices(self):
        self.assertIsNone(logic.parse_vulkaninfo('nothing useful'))

    def test_controller_names(self):
        text = 'N: Name="Sony Interactive Entertainment DualSense Wireless Controller"\nN: Name="DualSense Touchpad"\n'
        self.assertEqual('DualSense', logic.detect_pad(text))
        self.assertEqual('Xbox', logic.detect_pad('N: Name="Microsoft Xbox Series pad"\n'))
        for name, label in (('XInput Controller #1', 'Xbox'), ('Controller (Xbox One For Windows)', 'Xbox'),
                            ('8BitDo Ultimate 2 Wireless Controller', '8BitDo'),
                            ('Nintendo Switch Pro Controller', 'Switch Pro'), ('Wireless Controller', 'DualShock 4')):
            self.assertEqual(label, logic.detect_pad(f'N: Name="{name}"\n'), name)
        self.assertIsNone(logic.detect_pad('N: Name="Logitech USB Keyboard"\nN: Name="Power Button"\n'))

    def test_guide_for_detected_pads(self):
        for name, kind in (('Xbox', 'xbox'), ('8BitDo', '8bitdo'), ('Switch Pro', 'switch'), ('DualSense', 'xbox')):
            self.assertEqual(kind, logic.guide_kind(name))

    def test_controller_from_the_probe(self):
        from unittest import mock
        class R:
            stdout = 'Gamepad: type=standard south=a east=b west=x north=y name=XInput Controller #1\n'
        with mock.patch.object(logic.subprocess, 'run', return_value=R()):
            self.assertEqual('Xbox', logic.detect_pad())


class NewsTests(unittest.TestCase):
    README = ('# x\n\n## Novidades\n\n- **Primeira coisa (#51):** detalhe longo.\n- Segunda sem negrito.\n'
              '- **Terceira [com link](http://x)** mais\n- **Quarta:** não entra\n\n## Status\n- **fora:** x\n'
              "\n## What's new\n\n- **First thing:** detail\n")

    def test_takes_three_bold_leads(self):
        self.assertEqual(['Primeira coisa', 'Segunda sem negrito.'.rstrip('.'), 'Terceira com link'],
                         logic.parse_news(self.README, True))

    def test_english_section(self):
        self.assertEqual(['First thing'], logic.parse_news(self.README, False))

    def test_fallback_when_readme_is_missing(self):
        t = logic.Translator(PT)
        self.assertEqual(3, len(logic.news(t, True, readme='/no/such/README.md')))

    def test_the_real_readme_has_news(self):
        self.assertEqual(3, len(logic.parse_news((ROOT.parent / 'README.md').read_text(encoding='utf-8'), True)))


class SaveTests(unittest.TestCase):
    def test_title_from_param_bin(self):
        with tempfile.TemporaryDirectory() as folder:
            slot = Path(folder) / '1' / 'CUSA00223' / 'sce_sys'
            slot.mkdir(parents=True)
            (slot / 'param.bin').write_bytes(make_sfo({'SUBTITLE': 'Welcome to Seattle', 'DETAIL': '8%'}))
            self.assertEqual('Welcome to Seattle — 8%', logic.last_save_title(folder))

    def test_hidden_without_a_save_or_with_a_damaged_one(self):
        with tempfile.TemporaryDirectory() as folder:
            self.assertIsNone(logic.last_save_title(folder))
            self.assertIsNone(logic.last_save_title(Path(folder) / 'nope'))
            (Path(folder) / 'param.bin').write_bytes(b'garbage')
            self.assertIsNone(logic.last_save_title(folder))


class FixGroupTests(unittest.TestCase):
    def test_every_fix_is_in_a_known_group(self):
        for item in logic.fix_items():
            self.assertIn(logic.group_of(item), logic.GROUP_ORDER, item.id)
            self.assertIn(item.id, logic._GROUP_OF, item.id)   # a new switch must be placed on purpose

    def test_all_twelve_backports_and_the_ab_switches_are_listed(self):
        ids = {i.id for i in logic.fix_items()}
        self.assertTrue({f'BP{n:02d}' for n in range(1, 13)} <= ids)
        self.assertTrue({i.id for i in logic.backports_table.DIAGNOSTICS} <= ids)

    def test_risky_and_broken_items_are_experimental(self):
        for item in logic.fix_items():
            if item.risky or item.broken:
                self.assertEqual('exp', logic.group_of(item), item.id)


try:
    from qt.backend import Backend
    HAVE_QT = True
except ImportError:
    HAVE_QT = False


@unittest.skipUnless(HAVE_QT, 'PySide6 not installed')
class BackendTests(unittest.TestCase):
    def make(self, **settings):
        saved = []
        probes = {'gpu': lambda: ('GPU', '1.3'), 'pad': lambda: None, 'problems': lambda: [],
                  'cache': lambda: 0, 'save': lambda: 'Welcome to Seattle — 8%', 'news': lambda: ['a', 'b', 'c'],
                  'version': lambda: 'v1', 'save_fixes': lambda s, st: saved.append(dict(st))}
        base = dict(logic.tlg_run.DEFAULT_SETTINGS, game_dir='/g', ui_language='pt_BR', **settings)
        return Backend(settings=base, probes=probes), saved

    def test_play_subtitle_continues_the_save(self):
        backend, _ = self.make()
        self.assertTrue(backend.canPlay)
        self.assertEqual('Continuar · Welcome to Seattle — 8%', backend.playSubtitle)

    def test_set_fix_saves_and_profile_follows(self):
        backend, saved = self.make()
        self.assertTrue(backend.profileIsRecommended)
        backend.setFix('BP06', '1')
        self.assertEqual('1', saved[-1]['BP06'])
        self.assertFalse(backend.profileIsRecommended)
        backend.setFix('BP06', '0')            # back to its default: nothing stored
        self.assertIsNone(saved[-1]['BP06'])
        self.assertTrue(backend.profileIsRecommended)

    def test_previous_release_turns_every_default_on_backport_off(self):
        backend, saved = self.make()
        backend.resetFixes('previous')
        for item in logic.backports_table.BACKPORTS:
            self.assertEqual('0' if item.default == '1' else None, saved[-1][item.id], item.id)
        backend.resetFixes('defaults')
        self.assertTrue(backend.profileIsRecommended)

    def test_occlusion_selector_defaults_to_3_and_saves_through_the_fix_path(self):
        backend, saved = self.make()
        self.assertEqual(['0', '1', '2', '3'], [o['value'] for o in backend.occlusionOptions])
        self.assertEqual(3, backend.occlusionIndex)
        backend.setOcclusion('1')
        self.assertEqual(1, backend.occlusionIndex)
        self.assertEqual('1', saved[-1]['TLG_OCCLUSION_MODE'])
        backend.setOcclusion('3')
        self.assertEqual(3, backend.occlusionIndex)

    def test_game_language_list_is_auto_then_game_languages_in_order(self):
        backend, _ = self.make()
        self.assertEqual(['auto'] + list(logic.tlg_run.GAME_LANGUAGES), [o['value'] for o in backend.gameLanguageOptions])
        self.assertEqual(7, len(backend.gameLanguageOptions))
        self.assertEqual('Automático (idioma do sistema)', backend.gameLanguageOptions[0]['label'])
        self.assertEqual(['English', 'Português (Brasil)', 'Español (Latinoamérica)', 'Français (Canada)',
                          'Türkçe', 'Русский'], [o['label'] for o in backend.gameLanguageOptions[1:]])
        tk = (ROOT / 'launcher' / 'infamous_launcher.py').read_text(encoding='utf-8')
        for key, name in logic.GAME_LANGUAGE_NAMES.items():
            self.assertIn(f"'{key}': '{name}'", tk)

    def test_game_language_default_is_auto_and_shows_what_applies(self):
        self.assertEqual('auto', logic.tlg_run.DEFAULT_SETTINGS['game_language'])
        backend, _ = self.make(game_language='auto')
        self.assertEqual(0, backend.gameLanguageIndex)
        with mock.patch.dict('os.environ', {'LC_ALL': 'fr_FR.UTF-8'}):
            self.assertEqual('Automático: Français (Canada)', backend.gameLanguageNow)
        backend, _ = self.make(game_language='ru')
        self.assertEqual('', backend.gameLanguageNow)

    def test_game_language_saves_the_key_and_auto(self):
        backend, _ = self.make()
        with mock.patch.object(logic.tlg_run, 'save_settings') as save:
            backend.setGameLanguage('ru')
            self.assertEqual('ru', save.call_args[0][0]['game_language'])
            self.assertEqual(6, backend.gameLanguageIndex)
            backend.setGameLanguage('xx')                        # unknown keys are ignored
            self.assertEqual(1, save.call_count)
            backend.setGameLanguage('auto')
            self.assertEqual('auto', save.call_args[0][0]['game_language'])
            self.assertEqual(0, backend.gameLanguageIndex)

    def test_build_env_passes_the_chosen_game_language(self):
        for key, number in (('en', '1'), ('pt_BR', '17'), ('es_LA', '20'), ('fr_CA', '22'), ('tr', '19'), ('ru', '8')):
            backend, _ = self.make()
            with mock.patch.object(logic.tlg_run, 'save_settings'), mock.patch.dict('os.environ'):
                os.environ.pop('ISS_LANGUAGE', None)
                backend.setGameLanguage(key)
                self.assertEqual(number, logic.tlg_run.build_env(backend.settings)['ISS_LANGUAGE'], key)

    def test_controller_guides_follow_pad_language_and_tolerate_missing_files(self):
        self.assertEqual('8bitdo', logic.guide_kind('8BitDo Pro 2'))
        self.assertEqual('switch', logic.guide_kind('Switch Pro'))
        self.assertEqual('xbox', logic.guide_kind(None))
        with tempfile.TemporaryDirectory() as folder:
            backend, _ = self.make()
            backend.probes['guide_dir'] = Path(folder)
            self.assertEqual('', backend.guideSource)                      # no images: no crash
            (Path(folder) / 'inFAMOUS-controle-xbox-pt-BR.png').write_bytes(b'png')
            backend.setGuideKind('xbox')
            self.assertTrue(backend.guideSource.endswith('xbox-pt-BR.png'))
            backend.setLang('en_US')
            self.assertEqual('', backend.guideSource)                      # only pt-BR exists in the temp folder
            out = Path(folder) / 'copy'
            backend.setLang('pt_BR')
            backend.saveGuide(str(out))
            self.assertEqual(b'png', (Path(folder) / 'copy.png').read_bytes())
        shipped = [logic.guide_path(k, pt) for k, _ in logic.GUIDE_KINDS for pt in (True, False)]
        self.assertTrue(all(shipped))

    def test_language_switch_changes_texts_and_saves_setting(self):
        backend, _ = self.make()
        backend.probes['save_fixes'] = lambda s, st: None
        with tempfile.TemporaryDirectory() as folder, \
                mock.patch.dict('os.environ', {'XDG_CONFIG_HOME': folder}):   # not the player's settings.json
            backend.setLang('en_US')
        self.assertEqual('en_US', backend.lang)
        self.assertEqual('Continue · Welcome to Seattle — 8%', backend.playSubtitle)

    def test_fix_rows_have_groups_headers_and_controls(self):
        backend, _ = self.make()
        rows = backend.fixes.rows
        headers = [r['title'] for r in rows if r['kind'] == 'header']
        self.assertEqual(['Luzes e efeitos', 'Desempenho', 'Texturas', 'Experimental'], headers)
        bp06 = next(r for r in rows if r.get('id') == 'BP06')
        self.assertEqual('switch', bp06['control'])
        self.assertFalse(bp06['on'])
        auto = next(r for r in rows if r.get('id') == 'TLG_BLOCK1D')
        self.assertEqual('combo', auto['control'])


if __name__ == '__main__':
    unittest.main()
