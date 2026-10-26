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

    def test_player_switch_wins_over_the_defaults(self):
        self.assertEqual('0', defaults()['ISS_DRAW_PIPE'])
        self.assertEqual('0', self.env()['ISS_DRAW_PIPE'])
        (Path(self.folder.name) / 'tlg.ini').write_text('adv_draw_pipe=1\n', encoding='utf-8')
        self.assertEqual('1', self.env()['ISS_DRAW_PIPE'])
        self.assertEqual('0', self.env(ISS_DRAW_PIPE='0')['ISS_DRAW_PIPE'])   # the environment still wins

    def test_dead_keys_are_gone(self):
        for key in ('ISS_TLS_PATCH', 'ISS_GPU', 'ISS_MAPPING_CACHE', 'ISS_NATIVE_GEOMETRY'):
            self.assertNotIn(key, defaults())


class NoEffectV2Test(unittest.TestCase):
    def test_rows_marked_have_no_reader_in_v2(self):
        chunks = []
        for folder in ('gpu_v2', 'src'):
            for path in (ROOT / folder).rglob('*'):
                if path.suffix in ('.c', '.cpp', '.h', '.hpp') and path.is_file():
                    chunks.append(path.read_text(encoding='utf-8', errors='replace'))
        text = '\n'.join(chunks)
        items = {i.id: i for i in t.ITEMS}
        for rid in sorted(t.NO_EFFECT_V2):
            suffix = items[rid].env.split('_', 1)[1]
            self.assertIsNone(re.search(rf'(ISS|TLG)_{suffix}\b|"{suffix}"', text), rid)
            self.assertTrue(items[rid].name[0].endswith(t.NO_EFFECT_V2_NOTE[0]), rid)
        for name in ('READBACKS', 'EAGER_READBACK', 'WRITE_MERGE'):   # the launcher's own settings
            self.assertIsNone(re.search(rf'"(ISS|TLG)_{name}"', text), name)


if __name__ == '__main__':
    unittest.main()
