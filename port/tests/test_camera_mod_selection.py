"""Camera mod persistence and actual launch environment, without starting a game."""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import infamous_run as run


class CameraModTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.config = Path(self.temp.name) / 'mods.json'
        patch = mock.patch.object(run, 'mods_config', return_value=self.config)
        patch.start()
        self.addCleanup(patch.stop)
        mod_path = mock.patch.object(run, 'mods_dir', return_value=Path(self.temp.name))
        mod_path.start()
        self.addCleanup(mod_path.stop)

    def test_selection_preserves_other_mods_and_defaults_off(self):
        self.assertEqual(set(run.camera_mod_environment().values()), {'1'})
        self.config.write_text(json.dumps({'enabled': ['other/God mode'], 'disabled': []}))
        run.set_mod_enabled(run.CAMERA_MOD_KEY, True)
        self.assertEqual(set(run.camera_mod_environment().values()), {'1.5'})
        self.assertIn('other/God mode', json.loads(self.config.read_text())['enabled'])
        run.set_mod_enabled(run.CAMERA_MOD_KEY, False)
        self.assertEqual(set(run.camera_mod_environment().values()), {'1'})
        self.config.write_text('{broken')
        self.assertFalse(run.camera_mod_enabled())

    def test_catalogue_scoped_to_version(self):
        with mock.patch.object(run.mod_patches, 'game_app_version', return_value='01.07'), \
             mock.patch.object(run.mod_patches, 'external_patches', return_value=[]):
            rows = run.list_mods({'game_dir': self.temp.name})
            self.assertEqual(rows[0][0], run.CAMERA_MOD_KEY)
            self.assertFalse(rows[0][3])
        with mock.patch.object(run.mod_patches, 'game_app_version', return_value='01.06'), \
             mock.patch.object(run.mod_patches, 'external_patches', return_value=[]):
            self.assertEqual(run.list_mods({'game_dir': self.temp.name}), [])

    def test_real_build_env_checkbox_overrides_old_scripts(self):
        settings = dict(run.DEFAULT_SETTINGS, game_dir=self.temp.name)
        with mock.patch.dict(os.environ, {'ISS_USER_DIR': self.temp.name,
                                         'ISS_CAMERA_PAN_SCALE': '4',
                                         'ISS_CAMERA_TILT_SCALE': '4'}), \
             mock.patch.object(run, 'pad_glyphs', return_value=None), \
             mock.patch.object(run, 'window_icon', return_value=None), \
             mock.patch.object(run, 'renderer_v2', return_value=False):
            for enabled, expected in ((False, '1'), (True, '1.5'), (False, '1')):
                run.set_mod_enabled(run.CAMERA_MOD_KEY, enabled)
                env = run.build_env(settings, {'ISS_CAMERA_PAN_SCALE': '3'})
                self.assertEqual(env['ISS_CAMERA_PAN_SCALE'], expected)
                self.assertEqual(env['ISS_CAMERA_TILT_SCALE'], expected)


if __name__ == '__main__':
    unittest.main()
