# SPDX-License-Identifier: GPL-2.0-or-later
"""launcher/infamous_run.py build_env with renderer v2: gpu_v2/defaults.env, the variables set
outside the launcher (bench.ps1 variants, shells) and the player's switches, in that order of
precedence; ISS_TUNING merged per key; ISS_X=__unset__."""
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import backports_table as t  # noqa: E402
import infamous_run as run  # noqa: E402

DEFAULTS = ROOT / 'gpu_v2' / 'defaults.env'


def defaults():
    out = {}
    for line in DEFAULTS.read_text(encoding='utf-8').splitlines():
        key, sep, value = line.strip().partition('=')
        if sep and key and not key.startswith('#'):
            out[key] = value
    return out


class MergeTuningTest(unittest.TestCase):
    def test_per_key(self):
        base = 'a=1,scout=1,b=2'
        self.assertEqual('a=1,scout=1,b=2,x=1', run.merge_tuning(base, 'x=1'))
        self.assertEqual('a=1,scout=0,b=2', run.merge_tuning(base, 'scout=0'))
        self.assertEqual('a=1,scout=1,b=2', run.merge_tuning(base, ''))
        self.assertEqual('x=1', run.merge_tuning('', 'x=1'))

    def test_relaxed_site_is_added(self):
        merged = run.merge_tuning('relaxed_site=0x100,scout=1', 'relaxed_site=0x200,relaxed_site=0x100')
        self.assertEqual('relaxed_site=0x100,scout=1,relaxed_site=0x200', merged)

    def test_idempotent(self):
        base = defaults()['ISS_TUNING']
        for given in ('x=1', 'scout=0,relaxed_site=0x100', base):
            once = run.merge_tuning(base, given)
            self.assertEqual(once, run.merge_tuning(base, once), given)


class BuildEnvV2Test(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.saved = dict(os.environ)
        for name in list(os.environ):
            if name.startswith(('ISS_', 'TLG_')):
                del os.environ[name]
        os.environ['ISS_USER_DIR'] = self.folder.name
        self.patches = [mock.patch.object(run, 'pad_glyphs', return_value=None),
                        mock.patch.object(run, 'window_icon', return_value=None),
                        mock.patch.object(run, 'renderer_v2', return_value=True)]
        for p in self.patches:
            p.start()
        self.settings = dict(run.DEFAULT_SETTINGS, game_dir=self.folder.name)

    def tearDown(self):
        for p in self.patches:
            p.stop()
        os.environ.clear()
        os.environ.update(self.saved)
        self.folder.cleanup()

    def env(self, **given):
        os.environ.update(given)
        try:
            return run.build_env(self.settings)
        finally:
            for name in given:
                os.environ.pop(name, None)

    def test_defaults_tuning_without_a_variant(self):
        self.assertEqual(defaults()['ISS_TUNING'], self.env()['ISS_TUNING'])

    def test_variant_tuning_keeps_the_defaults(self):
        # bench.ps1's own example variant (ISS_TUNING = 'x=1') must not drop scout=1 & co.
        items = self.env(ISS_TUNING='x=1')['ISS_TUNING'].split(',')
        self.assertIn('scout=1', items)
        self.assertIn('x=1', items)
        for item in defaults()['ISS_TUNING'].split(','):
            self.assertIn(item, items)
        self.assertIn('scout=0', self.env(ISS_TUNING='scout=0')['ISS_TUNING'].split(','))
        self.assertNotIn('scout=1', self.env(ISS_TUNING='scout=0')['ISS_TUNING'].split(','))

    def test_unset_sentinel(self):
        self.assertIn('ISS_CP_CPU', defaults())
        env = self.env(ISS_CP_CPU=run.UNSET)
        self.assertNotIn('ISS_CP_CPU', env)
        self.assertNotIn('TLG_CP_CPU', env)
        self.assertNotIn('ISS_TUNING', self.env(ISS_TUNING=run.UNSET))
        self.assertNotIn(run.UNSET, self.env(ISS_NOT_A_DEFAULT=run.UNSET).values())

    def test_defaults_win_over_the_draw_pipe_switch(self):
        # the player_drawpipe_on case: an old tlg.ini adv_draw_pipe=1 leaves defaults.env's 0
        self.assertEqual('0', defaults()['ISS_DRAW_PIPE'])
        self.assertEqual('0', self.env()['ISS_DRAW_PIPE'])
        (Path(self.folder.name) / 'tlg.ini').write_text('adv_draw_pipe=1\n', encoding='utf-8')
        self.assertEqual('0', self.env()['ISS_DRAW_PIPE'])
        self.assertEqual('1', self.env(ISS_DRAW_PIPE='1')['ISS_DRAW_PIPE'])   # the environment still wins
        item = next(i for i in t.ITEMS if i.id == 'TLG_DRAW_PIPE')
        self.assertTrue(item.name[0].endswith(t.NO_EFFECT_V2_NOTE[0]))
        with mock.patch.object(run, 'renderer_v2', return_value=False):   # this port's renderer: passed
            self.assertEqual('1', run.build_env(self.settings)['ISS_DRAW_PIPE'])

    def test_other_switches_win_over_the_defaults(self):
        # defaults.env comes first: a switch the player changed overrides a key it sets
        fake = dict(defaults(), ISS_PREP_WORKERS='8')
        (Path(self.folder.name) / 'tlg.ini').write_text('adv_prep_workers=4\n', encoding='utf-8')
        with mock.patch.object(run, 'renderer_v2_env', return_value=fake):
            self.assertEqual('4', self.env()['ISS_PREP_WORKERS'])

    def test_dead_keys_are_gone(self):
        for key in ('ISS_TLS_PATCH', 'ISS_GPU', 'ISS_MAPPING_CACHE', 'ISS_NATIVE_GEOMETRY'):
            self.assertNotIn(key, defaults())


def v2_only(text):
    """text without the #else branches of '#ifdef TLG_RENDERER_V2' (this port's renderer only)."""
    out, stack = [], []   # stack: per open #if, True while its lines are dropped
    for line in text.splitlines():
        s = line.strip()
        if s.startswith(('#if', '#ifdef', '#ifndef')):
            stack.append([s == '#ifdef TLG_RENDERER_V2', False])
        elif s.startswith(('#else', '#elif')) and stack:
            stack[-1][1] = stack[-1][0]
        elif s.startswith('#endif') and stack:
            stack.pop()
        elif not any(drop for _, drop in stack):
            out.append(line)
    return '\n'.join(out)


class NoEffectV2Test(unittest.TestCase):
    def test_rows_marked_have_no_reader_in_v2(self):
        paths = [p for folder in ('gpu_v2', 'src') for p in (ROOT / folder).rglob('*')
                 if p.suffix in ('.c', '.cpp', '.h', '.hpp', '.inl') and p.is_file()]
        code = '\n'.join(p.read_text(encoding='utf-8', errors='replace') for p in paths)
        # port/gpu/shim: this port's interface, built into renderer v2 too (gpu_v2/CMakeLists.txt
        # OURS_SOURCES), with its renderer-v1-only parts under '#ifdef TLG_RENDERER_V2 ... #else'.
        shim = '\n'.join(v2_only(p.read_text(encoding='utf-8', errors='replace'))
                         for p in sorted((ROOT / 'gpu' / 'shim').glob('*.cpp')))
        # tlg_settings.cpp copies some TLG_ variables into settings fields (the overlay shows them):
        # such a variable only has an effect when renderer v2 or the runtime uses the field.
        fields = dict(re.findall(r'\{"(TLG_\w+)",\s*"(\w+)"\}', shim))
        self.assertIn('TLG_ASYNC_SHADERS', fields)
        shim = re.sub(r'\{"TLG_\w+",\s*"\w+"\}', '', shim)
        text = code + '\n' + shim
        items = {i.id: i for i in t.ITEMS}
        for rid in sorted(t.NO_EFFECT_V2):
            suffix = items[rid].env.split('_', 1)[1]
            self.assertIsNone(re.search(rf'(ISS|TLG)_{suffix}\b|"{suffix}"', text), rid)
            if items[rid].env in fields:
                self.assertIsNone(re.search(rf'\b{fields[items[rid].env]}\b', code), rid)
            self.assertTrue(items[rid].name[0].endswith(t.NO_EFFECT_V2_NOTE[0]), rid)
        for name in ('READBACKS', 'EAGER_READBACK', 'WRITE_MERGE'):   # the launcher's own settings
            self.assertIsNone(re.search(rf'"(ISS|TLG)_{name}"', text), name)


if __name__ == '__main__':
    unittest.main()
