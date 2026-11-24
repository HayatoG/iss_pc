"""The game folder's own metadata, with synthetic files only (no game data in the repository):
game languages from sce_sys/playgo-chunk.dat, the param.sfo check and the update's param.sfo in
scripts/make_game_view.py."""
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import infamous_run as run  # noqa: E402

ALL = (1 << 64) - 1
GAME_VIEW = ROOT.parent / 'scripts' / 'make_game_view.py'


def lang_mask(*ids):
    return sum(1 << (63 - i) for i in ids)


def playgo(masks, big_endian=False):
    """A playgo-chunk.dat with one chunk attribute (32 bytes, language mask at 16) per mask."""
    table = 256
    header = bytearray(table)
    header[:4] = b'plgo'
    struct.pack_into('<H', header, 10, len(masks))
    struct.pack_into('<II', header, 192, table, len(masks) * 32)
    attrs = b''.join(b'\0' * 16 + struct.pack('>Q' if big_endian else '<Q', m) + b'\0' * 8 for m in masks)
    return bytes(header) + attrs


def param_sfo(app_ver):
    """A minimal param.sfo with one APP_VER string entry."""
    key, value = b'APP_VER\0', app_ver.encode() + b'\0' * (8 - len(app_ver))
    keys = 20 + 16
    values = keys + len(key)
    header = struct.pack('<4sIIII', b'\0PSF', 0x101, keys, values, 1)
    return header + struct.pack('<HHIII', 0, 0x204, len(app_ver) + 1, 8, 0) + key + value


def write(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


class GameLanguageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.game = Path(self.tmp.name)
        run._inspected.clear()

    def tearDown(self):
        self.tmp.cleanup()

    def chunks(self, masks, big_endian=False):
        write(self.game / 'sce_sys' / 'playgo-chunk.dat', playgo(masks, big_endian))
        run._inspected.clear()

    def test_language_chunks_decide(self):
        self.chunks([ALL, lang_mask(1), lang_mask(2), lang_mask(3, 20), lang_mask(17), 0])
        self.assertEqual({1, 2, 3, 17, 20}, run.playgo_languages(self.game / 'sce_sys' / 'playgo-chunk.dat'))
        self.assertEqual(['en', 'pt_BR', 'es_LA', 'es', 'fr'], run.game_languages(self.game))
        self.chunks([ALL, lang_mask(18, 8, 19)])
        self.assertEqual(['en_GB', 'tr', 'ru'], run.game_languages(self.game))

    def test_european_dump_offers_its_languages(self):
        # #194, #202, #162: Polish, Italian and Spanish (Spain) were never offered
        self.chunks([ALL, lang_mask(18), lang_mask(2), lang_mask(4), lang_mask(5), lang_mask(3), lang_mask(16),
                     lang_mask(7), lang_mask(8)])
        self.assertEqual(['en_GB', 'pt_PT', 'es', 'fr', 'de', 'it', 'pl', 'ru'], run.game_languages(self.game))
        settings = {'game_dir': str(self.game), 'game_language': 'auto'}
        self.assertEqual('pl', run.resolve_game_language(settings, {'LANG': 'pl_PL.UTF-8'}))
        self.assertEqual('es', run.resolve_game_language(settings, {'LANG': 'es_MX.UTF-8'}))      # closest variant
        self.assertEqual('pt_PT', run.resolve_game_language(settings, {'LANG': 'pt_BR.UTF-8'}))
        self.assertEqual('en_GB', run.resolve_game_language(settings, {'LANG': 'en_US.UTF-8'}))
        self.assertEqual('en_GB', run.resolve_game_language(settings, {'LANG': 'ja_JP.UTF-8'}))
        with mock.patch.dict(os.environ, {'LC_ALL': 'it_IT.UTF-8'}):
            os.environ.pop('ISS_LANGUAGE', None)
            self.assertEqual('5', run.build_env(dict(run.DEFAULT_SETTINGS, game_dir=str(self.game)))['ISS_LANGUAGE'])

    def test_big_endian_masks(self):
        self.chunks([lang_mask(1), lang_mask(8)], big_endian=True)
        self.assertEqual(['en', 'ru'], run.game_languages(self.game))

    def test_nothing_known_offers_every_language(self):
        every = list(run._DEFAULT_LANGUAGES)
        self.assertEqual(every, run.game_languages(self.game))          # no playgo-chunk.dat (FTP dump)
        self.assertEqual(every, run.game_languages(''))
        self.chunks([ALL, 0])                                            # no language chunk
        self.assertEqual(every, run.game_languages(self.game))
        self.chunks([lang_mask(30, 40)])                                 # only ids the PS4 has no language for
        self.assertEqual(every, run.game_languages(self.game))
        self.chunks([lang_mask(0, 9)])
        self.assertEqual(['ja', 'ko'], run.game_languages(self.game))
        write(self.game / 'sce_sys' / 'playgo-chunk.dat', b'junk' * 100)
        run._inspected.clear()
        self.assertEqual(every, run.game_languages(self.game))
        data = bytearray(playgo([lang_mask(1)]))
        struct.pack_into('<H', data, 10, 50)                             # table past the end of the file
        write(self.game / 'sce_sys' / 'playgo-chunk.dat', bytes(data))
        run._inspected.clear()
        self.assertEqual(every, run.game_languages(self.game))

    def test_choices_keep_the_saved_language(self):
        self.chunks([lang_mask(1), lang_mask(17)])
        settings = {'game_dir': str(self.game), 'game_language': 'auto'}
        self.assertEqual(['auto', 'en', 'pt_BR'], run.game_language_choices(settings))
        self.assertEqual(['auto', 'en', 'pt_BR', 'ru'], run.game_language_choices(dict(settings, game_language='ru')))
        self.assertEqual(['auto'] + list(run._DEFAULT_LANGUAGES), run.game_language_choices({'game_language': 'tr'}))
        self.assertEqual(['auto'] + list(run._DEFAULT_LANGUAGES) + ['pl'], run.game_language_choices({'game_language': 'pl'}))

    def test_auto_falls_back_to_a_language_the_game_has(self):
        self.chunks([lang_mask(1), lang_mask(17)])
        settings = {'game_dir': str(self.game), 'game_language': 'auto'}
        self.assertEqual('pt_BR', run.resolve_game_language(settings, {'LANG': 'pt_BR.UTF-8'}))
        self.assertEqual('en', run.resolve_game_language(settings, {'LANG': 'ru_RU.UTF-8'}))
        self.assertEqual('ru', run.resolve_game_language(dict(settings, game_language='ru'), {'LANG': 'en_US'}))
        self.chunks([lang_mask(8)])
        self.assertEqual('ru', run.resolve_game_language(settings, {'LANG': 'tr_TR'}))
        self.assertEqual('tr', run.resolve_game_language({'game_language': 'auto'}, {'LANG': 'tr_TR'}))

    def test_build_env_uses_the_game_languages(self):
        self.chunks([lang_mask(1), lang_mask(20)])
        with mock.patch.dict(os.environ, {'LC_ALL': 'tr_TR.UTF-8'}):
            os.environ.pop('ISS_LANGUAGE', None)
            self.assertEqual('1', run.build_env(dict(run.DEFAULT_SETTINGS, game_dir=str(self.game)))['ISS_LANGUAGE'])

    def test_both_launchers_offer_the_same_choices(self):
        tk = (ROOT / 'launcher' / 'infamous_launcher.py').read_text(encoding='utf-8')
        qt = (ROOT / 'launcher' / 'qt' / 'backend.py').read_text(encoding='utf-8')
        self.assertIn('tlg_run.game_language_choices(self.settings)', tk)
        self.assertIn('tlg_run.game_language_choices(self.settings)', qt)


class ParamSfoCheckTests(unittest.TestCase):
    def check(self, game, layout=run.EBOOT_107):
        run._inspected.clear()
        with mock.patch.object(run, 'eboot_layout', side_effect=lambda path: layout), \
                mock.patch.object(run, 'ART_MIN_FILES', len(run.ART_REQUIRED)), \
                mock.patch.object(run, 'external_tls_relocations', side_effect=lambda path: 0), \
                mock.patch.object(run, 'PROBE', game / 'eboot.bin'):
            return run.check_game(game)

    def folder(self, tmp, app_ver=None):
        game = Path(tmp)
        for name in ('eboot.bin', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx'):
            write(game / name, b'x')
        for name in run.ART_REQUIRED:
            write(game / 'art' / name, b'x')
        if app_ver:
            write(game / 'sce_sys' / 'param.sfo', param_sfo(app_ver))
        return game

    def test_base_param_sfo_is_a_warning(self):
        with tempfile.TemporaryDirectory() as tmp:
            problems = self.check(self.folder(tmp, '01.00'))
        self.assertEqual([('old_param', 'sce_sys/param.sfo (APP_VER 01.00)')], problems)
        self.assertEqual([], run.blocking(problems))
        self.assertIn('appmeta', run.message('old_param', False, detail='x'))

    def test_update_or_missing_param_sfo_is_fine(self):
        for app_ver in ('01.07', None):
            with tempfile.TemporaryDirectory() as tmp:
                self.assertEqual([], self.check(self.folder(tmp, app_ver)), app_ver)

    def test_other_eboot_does_not_also_warn_about_param_sfo(self):
        with tempfile.TemporaryDirectory() as tmp:
            problems = self.check(self.folder(tmp, '01.00'), layout=(0x933850, 85_115_231))
        self.assertEqual(['other_eboot'], [key for key, _ in problems])


@unittest.skipIf(os.name == 'nt', 'the game view is made of symbolic links')
class GameViewTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.base, self.patch, self.out = root / 'dump', root / 'dump' / 'patch0', root / 'game'
        write(self.base / 'eboot.bin', b'base')
        write(self.base / 'sce_sys' / 'param.sfo', param_sfo('01.00'))
        write(self.base / 'sce_sys' / 'icon0.png', b'icon')
        write(self.patch / 'eboot.bin', b'update')
        write(self.patch / 'sce_sys' / 'changeinfo' / 'changeinfo.xml', b'<x/>')

    def tearDown(self):
        self.tmp.cleanup()

    def view(self, *extra):
        result = subprocess.run([sys.executable, str(GAME_VIEW), str(self.base), str(self.patch), str(self.out),
                                 *map(str, extra)], capture_output=True, text=True)
        self.assertEqual(0, result.returncode, result.stderr)
        return result.stdout

    def param(self):
        return (self.out / 'sce_sys' / 'param.sfo').resolve()

    def test_without_the_update_param_sfo_the_base_one_stays(self):
        self.assertIn('APP_VER 01.00', self.view())
        self.assertEqual((self.base / 'sce_sys' / 'param.sfo').resolve(), self.param())
        self.assertEqual((self.patch / 'eboot.bin').resolve(), (self.out / 'eboot.bin').resolve())
        self.assertTrue((self.out / 'sce_sys' / 'icon0.png').is_symlink())
        self.assertFalse((self.out / 'patch0').exists())

    def test_patch_meta_param_sfo_wins(self):
        meta = write(self.base / 'patch-meta' / 'param.sfo', param_sfo('01.07'))
        self.assertIn('APP_VER 01.07', self.view())
        self.assertEqual(meta.resolve(), self.param())
        self.assertFalse((self.out / 'patch-meta').exists())
        self.view()                                                       # re-running keeps it
        self.assertEqual(meta.resolve(), self.param())

    def test_update_sce_sys_param_sfo_wins(self):
        own = write(self.patch / 'sce_sys' / 'param.sfo', param_sfo('01.07'))
        self.view()
        self.assertEqual(own.resolve(), self.param())

    def test_given_param_sfo_wins_and_an_older_one_does_not(self):
        given = write(Path(self.tmp.name) / 'appmeta' / 'param.sfo', param_sfo('01.07'))
        self.view(given)
        self.assertEqual(given.resolve(), self.param())
        older = write(self.base / 'patch-meta' / 'param.sfo', param_sfo('00.99'))
        self.view()
        self.assertNotEqual(older.resolve(), self.param())

    def test_unreadable_given_param_sfo_is_refused(self):
        bad = write(Path(self.tmp.name) / 'bad.sfo', b'nope')
        result = subprocess.run([sys.executable, str(GAME_VIEW), str(self.base), str(self.patch), str(self.out),
                                 str(bad)], capture_output=True, text=True)
        self.assertNotEqual(0, result.returncode)
        self.assertFalse(self.out.exists())


if __name__ == '__main__':
    unittest.main()
