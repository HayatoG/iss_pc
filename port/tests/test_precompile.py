# SPDX-License-Identifier: GPL-2.0-or-later
"""launcher/precompile.py: when the shader jobs file is made, how it is written, and that no
generator problem stops the game. A fake generator stands in for tools/xpps (no game data)."""
import json
import os
import sys
import tempfile
import textwrap
import threading
import time
import unittest
from unittest import mock
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import precompile  # noqa: E402

FAKE = textwrap.dedent('''
    import json, sys, time
    mode = sys.argv[3]
    out = sys.argv[1]
    print('PROGRESS index 0 2', flush=True)
    print('PROGRESS index 2 2', flush=True)
    if mode == 'slow':
        time.sleep(30)
    if mode == 'fail':
        print('boom: cannot read the packages', file=sys.stderr, flush=True)
        sys.exit(1)
    print('PROGRESS jobs 1 1', flush=True)
    if mode == 'crash_after_progress':
        sys.exit(1)
    open(out + '.tmp', 'wb').write(b'ISSPJOB1' + bytes(8))
    import os
    os.replace(out + '.tmp', out)
    print(json.dumps({'records': 7}), file=sys.stderr, flush=True)
''')


class PrecompileBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.saved_env = {k: os.environ.get(k) for k in ('ISS_USER_DIR', 'TLG_USER_DIR', 'ISS_GPU_USER_DIR', 'TLG_GPU_USER_DIR')}
        for key in self.saved_env:
            os.environ.pop(key, None)
        os.environ['ISS_USER_DIR'] = str(root / 'user')
        self.addCleanup(self.restore_env)
        self.game = root / 'game'
        (self.game / 'art' / 'cache' / 'sub').mkdir(parents=True)
        for name in ('a.xpps', 'sub/b.xpps'):
            (self.game / 'art' / 'cache' / name).write_bytes(b'x' * 100)
        self.fake = root / 'fake_generator.py'
        self.fake.write_text(FAKE)
        self.mode = 'ok'
        self.calls = []
        original = precompile.generator_command

        def command(folder, out, index):
            self.calls.append((folder, out, index))
            return [sys.executable, str(self.fake), str(out), str(index), self.mode]
        precompile.generator_command = command
        self.addCleanup(setattr, precompile, 'generator_command', original)
        self.settings = {'game_dir': str(self.game), 'ui_language': 'en_US', 'precompile_shaders': True}
        self.jobs, self.meta, self.index = precompile.paths(str(self.game))

    def restore_env(self):
        for key, value in self.saved_env.items():
            if value is None:
                os.environ.pop(key, None)
            else:
                os.environ[key] = value

    def run_once(self, settings=None, progress=None):
        lines = []
        outcome = precompile.prepare_jobs(settings or self.settings, log=lines.append, progress=progress)
        return outcome, lines


class ParseProgress(unittest.TestCase):
    def test_lines(self):
        self.assertEqual(('index', 3, 10), precompile.parse_progress('PROGRESS index 3 10\n'))
        self.assertEqual(('jobs', 0, 0), precompile.parse_progress('  PROGRESS jobs 0 0  '))

    def test_not_progress(self):
        for line in ('', 'progress index 1 2', 'PROGRESS index x 2', 'PROGRESS index 1', '{"records": 3}',
                     'PROGRESS index 1 2 3', None):
            self.assertIsNone(precompile.parse_progress(line), line)


class Decision(PrecompileBase):
    def test_off(self):
        self.assertEqual('off', precompile.decide({**self.settings, 'precompile_shaders': False})[0])

    def test_no_game_folder(self):
        self.assertEqual('nogame', precompile.decide({**self.settings, 'game_dir': str(self.game / 'nope')})[0])
        self.assertEqual('nogame', precompile.decide({'precompile_shaders': True, 'game_dir': ''})[0])

    def test_missing_file_generates_then_is_current_then_changes(self):
        self.assertEqual('generate', precompile.decide(self.settings)[0])
        self.assertEqual(('done', 1), (self.run_once()[0], len(self.calls)))
        self.assertEqual('current', precompile.decide(self.settings)[0])
        self.assertEqual('current', self.run_once()[0])
        self.assertEqual(1, len(self.calls))                       # not run again
        (self.game / 'art' / 'cache' / 'a.xpps').write_bytes(b'y' * 101)   # game file changed
        self.assertEqual(('generate', 'game files changed'), precompile.decide(self.settings))
        os.utime(self.game / 'art' / 'cache' / 'a.xpps', ns=(1, 1))
        self.assertEqual('generate', precompile.decide(self.settings)[0])

    def test_new_or_removed_file_changes_fingerprint(self):
        before = precompile.fingerprint(self.game / 'art' / 'cache')
        (self.game / 'art' / 'cache' / 'c.xpps').write_bytes(b'')
        after = precompile.fingerprint(self.game / 'art' / 'cache')
        self.assertNotEqual(before[0], after[0])
        self.assertEqual((2, 3), (before[1], after[1]))

    def test_generator_version_change_regenerates(self):
        self.run_once()
        self.assertEqual('generate', precompile.decide(self.settings, version='other')[0])
        self.assertEqual('generate', precompile.decide(self.settings, version='other')[0])

    def test_missing_json_or_jobs_regenerates(self):
        self.run_once()
        self.meta.unlink()
        self.assertEqual('generate', precompile.decide(self.settings)[0])
        self.run_once()
        self.jobs.unlink()
        self.assertEqual('generate', precompile.decide(self.settings)[0])

    def test_generator_version_follows_the_sources(self):
        folder = Path(self.tmp.name) / 'xpps'
        folder.mkdir()
        (folder / 'a.py').write_text('x = 1')
        first = precompile.generator_version(folder)
        (folder / 'a.py').write_text('x = 2')
        self.assertNotEqual(first, precompile.generator_version(folder))
        self.assertEqual(precompile.generator_version(folder), precompile.generator_version(folder))
        self.assertTrue(precompile.generator_version())            # the real tools/xpps exists

    def test_generator_version_follows_the_witness_table(self):
        folder = Path(self.tmp.name) / 'xpps'
        (folder / 'data').mkdir(parents=True)
        (folder / 'a.py').write_text('x = 1')
        first = precompile.generator_version(folder)
        (folder / 'data' / 'CUSA00223-witnesses.json').write_text('{"format": 1, "pairs": {}}')
        second = precompile.generator_version(folder)
        self.assertNotEqual(first, second)
        (folder / 'data' / 'CUSA00223-witnesses.json').write_text('{"format": 1, "pairs": {"0x1:0x2": []}}')
        self.assertNotEqual(second, precompile.generator_version(folder))


class Writing(PrecompileBase):
    def test_success_writes_jobs_and_json_without_leftovers(self):
        seen = []
        outcome, lines = self.run_once(progress=lambda *a: seen.append(a))
        self.assertEqual('done', outcome)
        self.assertTrue(self.jobs.is_file())
        meta = json.loads(self.meta.read_text())
        self.assertEqual(precompile.META_FORMAT, meta['format'])
        self.assertEqual(precompile.generator_version(), meta['generator'])
        self.assertEqual(precompile.fingerprint(self.game / 'art' / 'cache')[0], meta['fingerprint'])
        self.assertEqual((2, 7, self.jobs.stat().st_size), (meta['xpps_files'], meta['records'], meta['jobs_bytes']))
        self.assertEqual(sorted(p.name for p in self.jobs.parent.iterdir()), sorted([self.jobs.name, self.meta.name]))
        self.assertEqual([('index', 0, 2), ('index', 2, 2), ('jobs', 1, 1)], seen)
        folder, out, index = self.calls[0]
        self.assertEqual(self.game / 'art' / 'cache', folder)
        self.assertEqual(self.jobs.name + '.part', Path(out).name)   # the generator never writes the final name
        self.assertTrue(any('Indexing the game files' in line for line in lines))
        self.assertTrue(any('ready' in line for line in lines))

    def test_portuguese_texts(self):
        _, lines = self.run_once({**self.settings, 'ui_language': 'pt_BR'})
        self.assertTrue(any('Indexando os arquivos do jogo' in line for line in lines), lines)
        self.assertTrue(any('Lista de shaders pronta' in line for line in lines), lines)

    def test_atomic_write_keeps_the_old_file_when_the_write_fails(self):
        path = Path(self.tmp.name) / 'x.bin'
        precompile.atomic_write(path, b'old')

        class Boom:
            pass
        with self.assertRaises(TypeError):
            precompile.atomic_write(path, Boom())
        self.assertEqual(b'old', path.read_bytes())
        self.assertEqual(['x.bin'], [p.name for p in path.parent.iterdir() if p.name.startswith('x.bin')])
        precompile.atomic_write(path, 'new')
        self.assertEqual(b'new', path.read_bytes())

    def test_cancel_leaves_no_partial_file_and_keeps_the_old_one(self):
        self.run_once()                                   # an older, finished file
        old = self.jobs.read_bytes()
        (self.game / 'art' / 'cache' / 'a.xpps').write_bytes(b'z' * 5)
        self.mode = 'slow'
        runner = precompile.Precompiler()
        result = []
        thread = threading.Thread(target=lambda: result.append(
            runner.run(self.settings, log=lambda line: None)))
        thread.start()
        deadline = time.time() + 20
        while runner._process is None and time.time() < deadline:
            time.sleep(0.05)
        time.sleep(0.5)
        runner.cancel()
        thread.join(20)
        self.assertEqual(['cancelled'], result)
        self.assertEqual(old, self.jobs.read_bytes())
        self.assertEqual([self.jobs.name], [p.name for p in self.jobs.parent.iterdir()])  # no .part, no json

    def test_cancel_before_start(self):
        runner = precompile.Precompiler()
        runner.cancel()
        self.assertEqual('cancelled', runner.run(self.settings, log=lambda line: None))
        self.assertFalse(self.jobs.exists())

    def test_option_off_deletes_the_files_and_generates_nothing(self):
        self.run_once()
        self.index.write_bytes(b'idx')
        outcome, _ = self.run_once({**self.settings, 'precompile_shaders': False})
        self.assertEqual('off', outcome)
        self.assertEqual(1, len(self.calls))
        self.assertFalse(self.jobs.exists() or self.meta.exists() or self.index.exists())


class NeverBlocksThePlayer(PrecompileBase):
    def test_generator_error(self):
        self.mode = 'fail'
        outcome, lines = self.run_once()
        self.assertEqual('failed', outcome)
        self.assertFalse(self.jobs.exists() or self.meta.exists())
        self.assertTrue(any('boom: cannot read the packages' in line for line in lines))
        self.assertTrue(any('starts without it' in line for line in lines))

    def test_exit_code_after_progress_is_a_failure_even_with_output(self):
        self.mode = 'crash_after_progress'
        self.assertEqual('failed', self.run_once()[0])
        self.assertFalse(self.jobs.exists())

    def test_generator_cannot_start(self):
        precompile.generator_command = lambda *a: ['/nonexistent/generator']
        outcome, lines = self.run_once()
        self.assertEqual('failed', outcome)
        self.assertTrue(any('skipped' in line for line in lines), lines)

    def test_broken_progress_callback_is_a_failure_not_an_exception(self):
        def bad(*_):
            raise RuntimeError('ui gone')
        self.assertEqual('failed', self.run_once(progress=bad)[0])

    def test_texts_exist_in_both_languages(self):
        for key, pair in precompile.TEXT.items():
            self.assertEqual(2, len(pair), key)
            self.assertTrue(all(pair), key)
        for phase, key in precompile.PHASE_TEXT.items():
            self.assertIn(key, precompile.TEXT)

    def test_default_setting_is_on(self):
        import infamous_run
        self.assertIs(True, infamous_run.DEFAULT_SETTINGS['precompile_shaders'])


try:
    sys.path.insert(0, str(ROOT / 'launcher'))
    from qt.backend import Backend
    from qt.graphics import Graphics
    HAVE_QT = True
except ImportError:
    HAVE_QT = False


@unittest.skipUnless(HAVE_QT, 'PySide6 not installed')
class QtScreens(PrecompileBase):
    def make(self, **settings):
        probes = {'gpu': lambda: ('GPU', '1.4'), 'pad': lambda: None, 'problems': lambda: [], 'cache': lambda: 0,
                  'save': lambda: None, 'news': lambda: [], 'version': lambda: 'v1',
                  'saves': {'running': lambda: False, 'scan': lambda: [], 'open': lambda p: None}}
        import infamous_run
        data = dict(infamous_run.DEFAULT_SETTINGS, game_dir=str(self.game), **{'ui_language': 'en_US', **settings})
        backend = Backend(settings=data, probes=probes)
        return backend, Graphics(backend, {'vram': lambda: 6144, 'clear': lambda: (0, [])})

    def test_option_row_toggles_the_setting(self):
        backend, graphics = self.make()
        row = graphics.model.get('s:precompile_shaders')
        self.assertTrue(row['on'])
        self.assertEqual('Compile shaders on the first start', row['label'])
        graphics.setValue('s:precompile_shaders', '0')
        self.assertFalse(backend.settings['precompile_shaders'])
        self.assertTrue(graphics.model.get('s:precompile_shaders')['changed'])

    def test_option_texts_in_portuguese(self):
        backend, graphics = self.make(ui_language='pt_BR')
        self.assertEqual('Compilar shaders na primeira abertura', graphics.model.get('s:precompile_shaders')['label'])
        strings = json.loads((ROOT / 'launcher' / 'qt' / 'i18n' / 'strings.json').read_text(encoding='utf-8'))
        self.assertTrue(strings['gfx_what_precompile_shaders']['pt'])

    def test_progress_text_and_bar(self):
        backend, _ = self.make()
        self.assertEqual(-1.0, backend.jobsProgressValue)
        backend._on_jobs_progress('index', 50, 200)
        self.assertEqual('Indexing the game files… 25%', backend.playSubtitle)
        self.assertAlmostEqual(0.25, backend.jobsProgressValue)
        backend._on_jobs_progress('jobs', 3, 4)
        self.assertEqual('Preparing the shader list… 75%', backend.jobsText)
        backend._jobs = None
        self.assertEqual(-1.0, backend.jobsProgressValue)

    def test_play_makes_the_list_before_the_game_and_a_failure_still_launches(self):
        backend, _ = self.make()
        order = []
        import infamous_run
        from unittest import mock
        self.mode = 'fail'
        with mock.patch.object(infamous_run, 'prepare', lambda s, log=None: order.append('prepare')), \
                mock.patch.object(infamous_run, 'launch', lambda s: order.append('launch') or (_Dead(), Path(self.tmp.name) / 'run.log')):
            backend.finished.connect(lambda code: order.append(('finished', code)))
            backend._run_game()
        self.assertEqual(['prepare', 'launch'], order[:2])

    def test_cancel_in_the_middle_does_not_launch(self):
        backend, _ = self.make()
        order = []
        import infamous_run
        from unittest import mock
        backend.precompiler.cancel()
        with mock.patch.object(infamous_run, 'prepare', lambda s, log=None: order.append('prepare')), \
                mock.patch.object(infamous_run, 'launch', lambda s: order.append('launch')):
            backend._run_game()
        self.assertEqual(['prepare'], order)
        self.assertFalse(self.jobs.exists())


class TkTexts(unittest.TestCase):
    def test_tk_table_has_the_shader_list_texts(self):
        try:
            import infamous_launcher
        except ImportError as error:
            self.skipTest(f'no tkinter: {error}')
        for key in precompile.TEXT:
            self.assertIn(key, infamous_launcher.TEXT)


class _Dead:
    stdout = iter(())

    def wait(self):
        return 0



class GeneratorCommandTests(unittest.TestCase):
    """The player waits for the generator: it takes every core but one, at normal priority."""
    def test_every_core_but_one(self):
        with mock.patch.object(precompile.os, 'cpu_count', return_value=16):
            command = precompile.generator_command('/game', '/out.bin', '/index.sqlite')
        self.assertEqual('15', command[command.index('--jobs') + 1])
        self.assertNotIn('--nice', command)

    def test_one_core(self):
        for count in (1, None):
            with mock.patch.object(precompile.os, 'cpu_count', return_value=count):
                self.assertEqual(1, precompile.generator_processes())

if __name__ == '__main__':
    unittest.main()
