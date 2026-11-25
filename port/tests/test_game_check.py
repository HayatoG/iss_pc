"""Game folder check (old libc.prx, other eboot) and friendly errors, without game binaries."""
import contextlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import infamous_run as run  # noqa: E402
import link_modules  # noqa: E402
import test_link_libc  # noqa: E402  (its fixture, not its tests)
from test_probe import package  # noqa: E402

TLS_SYMBOL = dict(name='', type=3, binding=0, section=1, value=0, size=0, identity=None)
NAMED_TLS = dict(name='KmfpnwO2lkA#C#A', type=6, binding=1, section=0, value=0, size=0, identity=None)


def tls_module(relocs, has_tls=True):
    return dict(ph=[dict(type=7)] if has_tls else [], symbols=[TLS_SYMBOL, NAMED_TLS], relocs=relocs)


class ExternalTlsTests(unittest.TestCase):
    def test_own_tls_block_is_linkable(self):
        self.assertEqual(link_modules.external_tls_relocations(tls_module([(64, 16, 0, 0), (72, 8, 0, 0)])), 0)

    def test_other_modules_tls_symbol_is_counted(self):
        relocs = [(64, 16, 0, 0), (72, 16, 1, 0), (80, 16, 0, 8)]
        self.assertEqual(link_modules.external_tls_relocations(tls_module(relocs)), 2)

    def test_module_without_tls_segment_cannot_link_dtpmod(self):
        self.assertEqual(link_modules.external_tls_relocations(tls_module([(64, 16, 0, 0)], has_tls=False)), 1)

    def test_link_raises_external_tls_error(self):
        main, libc = test_link_libc.LinkTests.fixture(None, relocs=[(64, 16, 1, 0)])
        main['identity'] = lambda name: ('fixture', ('libc', 1), ('libc', 1))
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            (out / 'boot.bin').write_bytes(package(b'\xc3', names=['fixture#q#q'], capabilities=1))
            with mock.patch.object(link_modules, 'module', side_effect=[main, libc]):
                with self.assertRaisesRegex(link_modules.ExternalTlsError,
                                            r'libc\.prx: unsupported external TLS relocation'):
                    link_modules.link(Path('fixture-game'), out, ['libc.prx'])


class CheckGameTests(unittest.TestCase):
    def folder(self, tmp):
        game = Path(tmp)
        (game / 'sce_module').mkdir()
        for name in ('eboot.bin', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx'):
            (game / name).write_bytes(b'x')
        for name in run.ART_REQUIRED:
            path = game / 'art' / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'x')
        return game

    def check(self, game, layout=run.EBOOT_107, external=0):
        run._inspected.clear()
        with mock.patch.object(run, 'eboot_layout', side_effect=lambda path: layout), \
                mock.patch.object(run, 'ART_MIN_FILES', len(run.ART_REQUIRED)), \
                mock.patch.object(run, 'external_tls_relocations',
                                  side_effect=lambda path: external if path.name == 'libc.prx' else 0), \
                mock.patch.object(run, 'PROBE', game / 'eboot.bin'):
            return run.check_game(game)

    def test_update_107_is_ready(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertEqual(self.check(self.folder(tmp)), [])

    def test_base_libc_blocks_play(self):
        with tempfile.TemporaryDirectory() as tmp:
            problems = self.check(self.folder(tmp), external=4)
        self.assertEqual(problems, [('old_module', 'sce_module/libc.prx')])
        self.assertTrue(run.blocking(problems))

    def test_other_eboot_is_only_a_warning(self):
        with tempfile.TemporaryDirectory() as tmp:
            problems = self.check(self.folder(tmp), layout=(0x939050, 135_507_407))
        self.assertEqual(problems, [('other_eboot', 'entry=0x939050, image=135,507,407 bytes')])
        self.assertEqual(run.blocking(problems), [])

    def test_unreadable_module_is_reported(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = self.folder(tmp)
            run._inspected.clear()
            with mock.patch.object(run, 'eboot_layout', side_effect=lambda path: run.EBOOT_107), \
                    mock.patch.object(run, 'ART_MIN_FILES', len(run.ART_REQUIRED)), \
                    mock.patch.object(run, 'PROBE', game / 'eboot.bin'):
                problems = run.check_game(game)  # 1-byte modules: the real parser rejects them
        self.assertEqual([kind for kind, _ in problems], ['unreadable', 'unreadable'])

    def test_messages_are_bilingual(self):
        for key in run.TEXT:
            en, pt = run.message(key, False, detail='X'), run.message(key, True, detail='X')
            self.assertIn('X', en)
            self.assertIn('X', pt)
            self.assertNotEqual(en, pt)
            self.assertEqual(run.message(key, detail='X'), f'{pt}\n{en}')


class FriendlyErrorTests(unittest.TestCase):
    def test_link_error_becomes_game_error(self):
        output = ('Traceback (most recent call last):\n  ...\n'
                  'ValueError: libc.prx: unsupported external TLS relocation\n'
                  'link_modules.py: libc.prx: unsupported external TLS relocation\n')
        error = run.step_error('link_modules.py', output)
        self.assertIsInstance(error, run.GameError)
        self.assertEqual((error.key, error.detail), ('old_module', 'sce_module/libc.prx'))
        self.assertIn('update 1.07', str(error))

    def test_other_errors_keep_the_last_line(self):
        error = run.step_error('prepare.py', 'Traceback\n  ...\nValueError: invalid memory image\n\n')
        self.assertNotIsInstance(error, run.GameError)
        self.assertEqual(str(error), 'prepare.py: ValueError: invalid memory image')

    def test_script_exception_is_reported_not_raised(self):
        with tempfile.TemporaryDirectory() as tmp:
            script = Path(tmp) / 'broken.py'
            script.write_text("raise ValueError('libc.prx: unsupported external TLS relocation')\n")
            stderr = io.StringIO()
            argv, path = sys.argv, list(sys.path)
            try:
                with contextlib.redirect_stderr(stderr):
                    code = run.run_script([str(script)])
            finally:
                sys.argv, sys.path[:] = argv, path
        self.assertEqual(code, 1)
        self.assertEqual(stderr.getvalue().splitlines()[-1],
                         'broken.py: libc.prx: unsupported external TLS relocation')

    def test_prepare_raises_game_error_for_old_libc(self):
        failed = mock.Mock(returncode=1, stdout='', stderr='link_modules.py: libc.prx: unsupported external TLS relocation\n')
        ok = mock.Mock(returncode=0, stdout='Prepared\n', stderr='')
        logged = []
        with mock.patch.object(run.subprocess, 'run', side_effect=[ok, failed]):
            with self.assertRaises(run.GameError) as caught:
                run.prepare({'game_dir': 'game'}, log=logged.append)
        self.assertEqual(caught.exception.key, 'old_module')
        self.assertIn('link_modules.py: libc.prx: unsupported external TLS relocation', logged)


if __name__ == '__main__':
    unittest.main()
