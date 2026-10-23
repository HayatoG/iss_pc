"""launcher/backports_table.py stays in step with the C++ sources it mirrors."""
import re
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import backports_table as t  # noqa: E402
import infamous_run  # noqa: E402,F401

HEADER = ROOT / 'gpu' / 'shadps4' / 'common' / 'backports.h'
TOGGLES = ROOT / 'gpu' / 'shadps4' / 'shader_recompiler' / 'recompiler_toggles.h'
SOURCE_DIRS = ('gpu', 'gpu_v2', 'src')


def header_entries():
    text = HEADER.read_text(encoding='utf-8')
    entries = {}
    for m in re.finditer(r'\{"(BP\d+)",\s*"(TLG_BP\d+)",\s*(true|false),\s*(true|false),', text):
        entries[m.group(1)] = (m.group(2), m.group(3) == 'true', m.group(4) == 'true')
    live_block = re.search(r'kLive\{\{(.*?)\}\};', text, re.S).group(1)
    live = {m.group(2): m.group(1) == 'true'
            for m in re.finditer(r'(true|false),\s*//\s*(BP\d+)', live_block)}
    return entries, live


def repo_text():
    chunks = []
    for folder in SOURCE_DIRS:
        for path in (ROOT / folder).rglob('*'):
            if path.suffix in ('.c', '.cpp', '.h', '.hpp', '.py', '.sh') and path.is_file():
                try:
                    chunks.append(path.read_text(encoding='utf-8', errors='replace'))
                except OSError:
                    pass
    return '\n'.join(chunks)


class BackportsTableTest(unittest.TestCase):
    def test_bp_rows_match_header(self):
        entries, live = header_entries()
        self.assertEqual(len(entries), 12)
        self.assertEqual(sorted(entries), sorted(i.id for i in t.BACKPORTS), 'ids differ from backports.h')
        for item in t.BACKPORTS:
            env, default, shader = entries[item.id]
            self.assertEqual(item.env, env, item.id)
            self.assertEqual(item.default == '1', default, f'{item.id} default')
            self.assertEqual(item.shader, shader, f'{item.id} shader')
            self.assertEqual(item.live, live[item.id], f'{item.id} live')

    def test_every_bp_has_texts_and_source(self):
        for item in t.BACKPORTS:
            self.assertTrue(all(item.name) and all(item.what) and item.source, item.id)
        risky = {i.id for i in t.BACKPORTS if i.risky}
        self.assertEqual(risky, {'BP06', 'BP07', 'BP08', 'BP09'})   # experimental, whatever their default

    def test_diagnostic_names_exist_in_sources(self):
        text = repo_text()
        for item in t.DIAGNOSTICS:
            self.assertIn(item.env, text, item.env)

    def test_recompiler_toggle_defaults(self):
        text = TOGGLES.read_text(encoding='utf-8')
        for item in t.DIAGNOSTICS:
            m = re.search(rf'EnvFlag\("{item.env}",\s*(.*?)\);', text)
            if not m:
                continue
            fallback = m.group(1)
            expected = '0' if 'return false' in fallback else '1'
            self.assertEqual(item.default, expected, item.env)  # AlwaysOn / InfamousDefault: on in this port
            self.assertTrue(item.shader, item.env)  # all of them are part of the shader cache key

    def test_menu_keys_exist_in_settings_code(self):
        text = (ROOT / 'gpu' / 'shim' / 'tlg_settings.cpp').read_text(encoding='utf-8')
        for item in t.MENU:
            self.assertIn(f'"{item.key}"', text, item.key)

    def test_menu_keys_are_menu_widgets(self):
        # Every key offered as a menu option is a widget of the in-game menu (the seven overlay
        # item switches are the "What to show" checkboxes, stored as overlay_<item>).
        text = (ROOT / 'gpu' / 'shim' / 'tlg_overlay.cpp').read_text(encoding='utf-8')
        items = {'overlay_fps', 'overlay_frametime', 'overlay_upscaler', 'overlay_gpu', 'overlay_cpu',
                 'overlay_ram', 'overlay_vram'}
        for item in t.MENU:
            if item.key not in items:
                self.assertRegex(text, rf'\bs\.{item.key}\b', item.key)

    def test_unique_keys(self):
        keys = [i.key for i in t.ITEMS]
        self.assertEqual(len(keys), len(set(keys)))
        self.assertEqual(len({i.id for i in t.ITEMS}), len(t.ITEMS))


class StatesTest(unittest.TestCase):
    def test_untouched_changes_nothing(self):
        states = t.read_states({})
        self.assertTrue(all(v is None for v in states.values()))
        self.assertEqual(t.environment(states), {})
        self.assertTrue(all(v is None for v in t.ini_updates(states).values()))

    def test_rx580_diagnostic_controls_round_trip_and_external_env_wins(self):
        states = t.read_states({'adv_explicit_range': '0', 'adv_barrier_split': '2', 'adv_barrier_merge': '1'})
        expected = {'ISS_EXPLICIT_RANGE': '0', 'ISS_BARRIER_SPLIT': '2', 'ISS_BARRIER_MERGE': '1'}
        self.assertEqual(t.environment(states, {}), expected)
        for key, value in expected.items():
            self.assertNotIn(key, t.environment(states, {key: value}))
            self.assertNotIn(key, t.environment(states, {key.replace('ISS_', 'TLG_'): value}))
        defaults = t.read_states({'adv_explicit_range': '1', 'adv_barrier_split': '0', 'adv_barrier_merge': '0'})
        self.assertEqual(t.environment(defaults, {}), {})
        split = next(i for i in t.ITEMS if i.id == 'TLG_BARRIER_SPLIT')
        self.assertIsNone(t.normalize(split, '3'))
        self.assertIsNone(t.normalize(split, '-1'))

    def test_value_equal_to_default_is_dropped(self):
        states = t.read_states({'bp01': '1', 'bp06': '1', 'bp12': '0', 'overlay_corner': '1', 'rcas_strength': '60'})
        self.assertTrue(all(v is None for v in states.values()))

    def test_changes_become_env_and_ini(self):
        states = t.read_states({'bp01': '0', 'bp06': '0', 'overlay_corner': '3', 'rcas_strength': '999',
                                'wqm': '0', 'pipeline_binaries': '1'})
        self.assertEqual(states['BP01'], '0')
        self.assertEqual(states['BP06'], '0')
        self.assertIsNone(states['menu:rcas_strength'])  # out of range: ignored
        self.assertEqual(t.environment(states), {'ISS_BP01': '0', 'ISS_BP06': '0', 'ISS_WQM': '0',
                                                 'ISS_PIPELINE_BINARIES': '1'})
        self.assertEqual(t.environment(states, {'TLG_BP01': '1', 'ISS_WQM': '1'}),
                         {'ISS_BP06': '0', 'ISS_PIPELINE_BINARIES': '1'})
        ini = t.ini_updates(states)
        self.assertEqual((ini['bp01'], ini['overlay_corner'], ini['bp02']), ('0', '3', None))

    def test_previous_release_turns_default_on_off(self):
        states = t.previous_release(t.read_states({}))
        env = t.environment(states)
        self.assertEqual(sorted(env), [f'ISS_BP{n:02d}' for n in (1, 2, 3, 4, 5, 6, 8, 10, 11)])
        self.assertEqual(set(env.values()), {'0'})  # BP07, BP09 and BP12 are already off: no variable
        self.assertEqual(t.environment(t.defaults(states)), {})

    def test_write_ini_sets_and_removes_keeping_other_lines(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / 'tlg.ini'
            path.write_text('# c\nvsync=1\nbp01=0\nbp02=0\n', encoding='utf-8')
            t.write_ini(path, {'bp01': None, 'bp03': '0', 'bp02': '0'})
            self.assertEqual(path.read_text(encoding='utf-8'), '# c\nvsync=1\nbp02=0\nbp03=0\n')


class RecommendedTest(unittest.TestCase):
    def test_recommended_is_the_table_defaults(self):
        states = t.read_states({})
        self.assertEqual(t.changes(states), 0)
        self.assertEqual(t.environment(states), {})
        rec = {i.id for i in t.ITEMS if t.is_recommended(i)}
        self.assertTrue({f'BP{n:02d}' for n in (1, 2, 3, 4, 5, 6, 8, 10, 11)} <= rec)   # BP08 on since 2026-10-16
        self.assertFalse(rec & {f'BP{n:02d}' for n in (7, 9, 12)})
        self.assertNotIn('TLG_PIPELINE_BINARIES', rec)

    def test_profile_counts_changes_but_not_menu_preferences(self):
        states = t.read_states({'bp01': '0', 'bp06': '0', 'overlay_corner': '3'})
        self.assertEqual(t.changes(states), 2)
        self.assertEqual(t.changes(t.previous_release(t.read_states({}))), 9)   # BP08 on since 2026-10-16
        self.assertEqual(t.changes(t.defaults(states)), 0)

    def test_first_run_notice_once(self):
        settings = {}
        self.assertTrue(t.notice_pending(settings))
        t.mark_notice_seen(settings)
        self.assertFalse(t.notice_pending(settings))
        self.assertFalse(t.notice_pending(dict(settings)))  # as saved and loaded again


class LaunchEnvTest(unittest.TestCase):
    def test_build_env_carries_only_the_changes(self):
        import os
        import infamous_run as run
        with tempfile.TemporaryDirectory() as folder:
            old_env = dict(os.environ)
            try:
                for name in list(os.environ):
                    if name.startswith(('ISS_BP', 'TLG_BP')):
                        del os.environ[name]
                os.environ['ISS_USER_DIR'] = folder
                os.environ['ISS_ADVANCED'] = '1'   # the player's choices apply only in the advanced mode
                settings = dict(run.DEFAULT_SETTINGS)
                env = run.build_env(settings)
                self.assertFalse([k for k in env if k.startswith('ISS_BP')])
                (Path(folder) / 'tlg.ini').write_text('bp01=0\nbp06=0\n', encoding='utf-8')
                env = run.build_env(settings)
                self.assertEqual((env['ISS_BP01'], env['ISS_BP06']), ('0', '0'))
                self.assertFalse([k for k in env if k.startswith('ISS_BP') and k not in ('ISS_BP01', 'ISS_BP06')])
            finally:
                os.environ.clear()
                os.environ.update(old_env)


if __name__ == '__main__':
    unittest.main()


class AdvancedTableTest(unittest.TestCase):
    def test_names_exist_in_sources(self):
        text = repo_text()
        for item in t.ADVANCED:
            self.assertIn(f'"{item.env}"', text, item.env)

    def test_rows_are_consistent(self):
        import advanced_table
        areas = {a[0] for a in advanced_table.AREAS}
        for item in t.ADVANCED:
            self.assertEqual('adv', item.group)
            self.assertIn(item.tab, areas, item.id)
            self.assertTrue(item.key.startswith('adv_') and item.pass_env and not item.shader, item.id)
            self.assertTrue(all(item.name) and all(item.what), item.id)
            if item.vtype == 'int':
                self.assertLess(item.lo, item.hi, item.id)
                self.assertTrue(item.default is None or item.lo <= int(item.default) <= item.hi, item.id)
            if item.vtype == 'choice':
                self.assertIn(item.default, [c[0] for c in item.choices], item.id)

    def test_changes_reach_the_game_and_the_ini(self):
        states = {i.id: None for i in t.ITEMS}
        threads = next(i for i in t.ADVANCED if i.env == 'TLG_PREP_WORKERS')
        states[threads.id] = t.normalize(threads, '4')
        self.assertEqual('4', states[threads.id])
        self.assertEqual({'ISS_PREP_WORKERS': '4'}, t.environment(states, {}))
        self.assertEqual('4', t.ini_updates(states)['adv_prep_workers'])
        self.assertEqual(1, t.advanced_changes(states))
        self.assertEqual(0, t.changes(states))                      # the profile counts fixes only
        self.assertIsNone(t.normalize(threads, '99'))               # out of range
        self.assertEqual({}, t.environment(states, {'ISS_PREP_WORKERS': '2'}))   # the environment wins

    def test_defaults_clear_only_the_advanced_group(self):
        states = {i.id: None for i in t.ITEMS}
        states['TLG_FULL_BARRIERS'] = '1'
        states['BP01'] = '0'
        cleared = t.defaults(states, 'adv')
        self.assertIsNone(cleared['TLG_FULL_BARRIERS'])
        self.assertEqual('0', cleared['BP01'])
