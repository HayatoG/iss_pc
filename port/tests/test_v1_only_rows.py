"""Renderer-1-only workarounds (BLOCK1D, BC_TINY, NO_MSAA, IMG_TRACE, BC1D_LOG): the texts say so and
the Qt rows are disabled while the effective renderer is v2. Settings are isolated by tests/paths.py."""
import os
import sys
import unittest

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from qt import logic  # noqa: E402

IDS = ('TLG_BLOCK1D', 'TLG_BC_TINY', 'TLG_NO_MSAA', 'TLG_IMG_TRACE', 'TLG_BC1D_LOG')

try:
    from qt.backend import Backend
    HAVE_QT = True
except ImportError:
    HAVE_QT = False


class TextTests(unittest.TestCase):
    def test_texts_say_renderer_1_only_in_both_languages(self):
        items = {i.id: i for i in logic.backports_table.ITEMS}
        for rid in IDS:
            en, pt = items[rid].what
            self.assertIn('Renderer 1 only', en, rid)
            self.assertIn('Só no renderer 1', pt, rid)

    def test_ids_match_the_logic_constant(self):
        self.assertEqual(set(IDS), set(logic.V1_ONLY))


    def test_renderer_is_always_2_while_v1_is_disabled(self):
        item = next(i for i in logic.backports_table.ITEMS if i.id == 'TLG_RENDERER')
        if logic.backports_table.V1_DISABLED:
            for state in ({}, {'TLG_RENDERER': '1'}, {'TLG_RENDERER': '2'}):
                self.assertEqual(logic.backports_table.effective(item, state), '2')


@unittest.skipUnless(HAVE_QT, 'PySide6 not installed')
class RowTests(unittest.TestCase):
    def rows(self, renderer):
        probes = {'gpu': lambda: ('GPU', '1.3'), 'pad': lambda: None, 'problems': lambda: [],
                  'cache': lambda: 0, 'save': lambda: '', 'news': lambda: [], 'version': lambda: 'v1',
                  'save_fixes': lambda s, st: None}
        settings = dict(logic.tlg_run.DEFAULT_SETTINGS, game_dir='/g', ui_language='en_US')
        backend = Backend(settings=settings, probes=probes)
        backend.fix_states['TLG_RENDERER'] = renderer
        backend.fixes.refresh_all()
        backend.advanced.refresh_all()
        found = {}
        for model in (backend.fixes, backend.advanced):
            found.update({r['id']: r for r in model.rows if r['kind'] == 'item'})
        return found

    def test_disabled_with_renderer_2_enabled_with_1(self):
        # With renderer 1 off in this build (V1_DISABLED) the effective renderer is always 2.
        for renderer, off in (('2', True), ('1', logic.backports_table.V1_DISABLED)):
            found = self.rows(renderer)
            for rid in IDS:
                self.assertIn(rid, found)
                self.assertEqual(off, found[rid]['disabled'], (renderer, rid))

    def test_other_rows_stay_enabled(self):
        self.assertFalse(self.rows('2')['BP06']['disabled'])


if __name__ == '__main__':
    unittest.main()
