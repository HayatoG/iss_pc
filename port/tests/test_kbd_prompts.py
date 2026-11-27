"""Tests for the dynamic Keyboard & Mouse button prompt mod (generator + launcher wiring)."""
import json
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
sys.path.insert(0, str(ROOT / 'launcher'))

import kbd_prompts  # noqa: E402
import infamous_run as run  # noqa: E402


def _make_fake_lang_xpps(path, fill_rgba=(10, 20, 30, 255)):
    """Creates a minimal valid KCAP lang_english_text.xpps with the 512x512 icon SBitmap."""
    header = bytearray(0x600)
    header[0:4] = b'KCAP'
    # Place SBitmap T# dwords at 0x4f0 and OID at 0x510 (matching real lang_*_text.xpps layout).
    tex_rel = 0x400  # tex_off = 0x208 + 0x400 = 0x608
    struct.pack_into('<3I', header, 0x4F0, tex_rel, kbd_prompts.EXPECTED_DW1, kbd_prompts.EXPECTED_DW2)
    header[0x510:0x518] = kbd_prompts.BITMAP_OID
    pad = bytearray(0x608 - len(header))
    rgba = bytes(fill_rgba) * (kbd_prompts.ATLAS_W * kbd_prompts.ATLAS_H)
    tiled = kbd_prompts.tile_atlas(rgba)
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(bytes(header) + bytes(pad) + tiled)
    return 0x608, rgba, tiled


class KbdPromptsGeneratorTests(unittest.TestCase):
    def test_effective_bindings_default_and_custom(self):
        b_mouse = kbd_prompts.effective_bindings('', mouse_enabled=True)
        self.assertEqual(b_mouse['cross'], 'Space')
        self.assertEqual(b_mouse['r2'], 'Mouse Left')
        self.assertEqual(b_mouse['l2'], 'Mouse Right')
        self.assertEqual(b_mouse['r3'], 'Mouse Middle')
        self.assertEqual(b_mouse['right_stick'], '__MOUSE_MOVE__')

        b_nomouse = kbd_prompts.effective_bindings('', mouse_enabled=False)
        self.assertEqual(b_nomouse['r2'], 'F')
        self.assertEqual(b_nomouse['l2'], 'R')
        self.assertEqual(b_nomouse['r3'], 'C')
        self.assertEqual(b_nomouse['right_stick'], '__ARROWS__')

        b_custom = kbd_prompts.effective_bindings(
            'cross=F|Space, r2=G, l2=Mouse X1, touchpad=Backspace, l3=none',
            mouse_enabled=True,
        )
        self.assertEqual(b_custom['cross'], 'F')
        self.assertEqual(b_custom['r2'], 'G')
        self.assertEqual(b_custom['l2'], 'Mouse X1')
        self.assertEqual(b_custom['touchpad_left'], 'Backspace')
        self.assertIsNone(b_custom['l3'])

    def test_key_display_spec_covers_all_categories(self):
        self.assertEqual(kbd_prompts.key_display_spec('Space'), ('text', 'SPACE'))
        self.assertEqual(kbd_prompts.key_display_spec('Left Shift'), ('text', 'SHIFT'))
        self.assertEqual(kbd_prompts.key_display_spec('Right Shift'), ('text', 'RSHF'))
        self.assertEqual(kbd_prompts.key_display_spec('Left Ctrl'), ('text', 'CTRL'))
        self.assertEqual(kbd_prompts.key_display_spec('Return'), ('text', 'ENTER'))
        self.assertEqual(kbd_prompts.key_display_spec('Keypad 7'), ('text', 'KP7'))
        self.assertEqual(kbd_prompts.key_display_spec('F12'), ('text', 'F12'))
        self.assertEqual(kbd_prompts.key_display_spec('Up'), ('arrow_up', ''))
        self.assertEqual(kbd_prompts.key_display_spec('Mouse Left'), ('mouse_left', ''))
        self.assertEqual(kbd_prompts.key_display_spec('Wheel Down'), ('wheel_down', ''))
        self.assertEqual(kbd_prompts.key_display_spec(None), ('none', '-'))
        # Unknown long key name still produces a compact non-empty label.
        kind, lbl = kbd_prompts.key_display_spec('SuperLongCustomKey')
        self.assertEqual(kind, 'text')
        self.assertTrue(1 <= len(lbl) <= 5)

    def test_tile_untile_roundtrip_and_preserves_non_button_pixels(self):
        with tempfile.TemporaryDirectory() as tmp:
            game = Path(tmp) / 'game'
            lang = game / 'art' / 'cache' / 'lang_english_text.xpps'
            tex_off, orig_rgba, orig_tiled = _make_fake_lang_xpps(lang, fill_rgba=(11, 22, 33, 255))

            found_path, found_off = kbd_prompts.find_lang_atlas_source(game)
            self.assertEqual(found_path, lang)
            self.assertEqual(found_off, tex_off)
            self.assertEqual(kbd_prompts.tile_atlas(kbd_prompts.untile_atlas(orig_tiled)), orig_tiled)

            out_bin = Path(tmp) / 'out' / 'kbd_prompts_atlas.bin'
            msg1 = kbd_prompts.generate(game, out_bin, key_map='cross=F,r2=Mouse X2', mouse_enabled=True)
            self.assertIn('generated', msg1)
            self.assertEqual(out_bin.stat().st_size, kbd_prompts.ATLAS_BYTES)

            # Second call with identical inputs hits cache.
            msg2 = kbd_prompts.generate(game, out_bin, key_map='cross=F,r2=Mouse X2', mouse_enabled=True)
            self.assertIn('up to date', msg2)

            # Changing key_map regenerates with different bytes.
            bytes_before = out_bin.read_bytes()
            msg3 = kbd_prompts.generate(game, out_bin, key_map='cross=G,r2=Mouse X2', mouse_enabled=True)
            self.assertIn('generated', msg3)
            bytes_after = out_bin.read_bytes()
            self.assertNotEqual(bytes_before, bytes_after)

            # Non-button region (e.g., logo area at x=20, y=20) stays bit-for-bit untouched.
            patched_rgba = kbd_prompts.untile_atlas(bytes_after)
            logo_off = (20 * kbd_prompts.ATLAS_W + 20) * 4
            self.assertEqual(bytes(patched_rgba[logo_off:logo_off + 4]), bytes((11, 22, 33, 255)))
            # Button region (Cross at x=102..152, y=263..313) was replaced with pure-white alpha glyphs.
            cross_px = [
                tuple(patched_rgba[(y * kbd_prompts.ATLAS_W + x) * 4:(y * kbd_prompts.ATLAS_W + x) * 4 + 4])
                for y in range(263, 313) for x in range(102, 152)
            ]
            self.assertTrue(any(p[3] == 255 and p[:3] == (255, 255, 255) for p in cross_px))
            self.assertTrue(any(p[3] == 0 for p in cross_px))


class KbdPromptsLauncherTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.game = self.root / 'game'
        _make_fake_lang_xpps(self.game / 'art' / 'cache' / 'lang_english_text.xpps')
        self.config = self.root / 'mods.json'
        self.atlas = self.root / 'out' / 'kbd_prompts_atlas.bin'
        for attr, val in (
            ('mods_config', mock.Mock(return_value=self.config)),
            ('mods_dir', mock.Mock(return_value=self.root / 'mods')),
            ('KBD_PROMPTS_ATLAS', self.atlas),
        ):
            p = mock.patch.object(run, attr, val)
            p.start()
            self.addCleanup(p.stop)

    def test_mod_defaults_off_and_controls_env(self):
        settings = dict(run.DEFAULT_SETTINGS, game_dir=str(self.game), key_map='cross=V')
        with mock.patch.object(run.mod_patches, 'game_app_version', return_value='01.07'), \
             mock.patch.object(run.mod_patches, 'external_patches', return_value=[]):
            rows = run.list_mods(settings)
            kbd_rows = [r for r in rows if r[0] == run.KBD_PROMPTS_MOD_KEY]
            self.assertEqual(len(kbd_rows), 1)
            self.assertFalse(kbd_rows[0][3])

        with mock.patch.dict(os.environ, {'ISS_USER_DIR': str(self.root),
                                         'ISS_KBD_PROMPTS_ATLAS': '/stale/path.bin'}), \
             mock.patch.object(run, 'pad_glyphs', return_value=None), \
             mock.patch.object(run, 'window_icon', return_value=None), \
             mock.patch.object(run, 'renderer_v2', return_value=False):
            # Off: stale env var is stripped.
            env_off = run.build_env(settings)
            self.assertNotIn('ISS_KBD_PROMPTS_ATLAS', env_off)
            self.assertFalse(self.atlas.exists())

            # On: atlas generated with custom key_map and exported.
            run.set_mod_enabled(run.KBD_PROMPTS_MOD_KEY, True)
            env_on = run.build_env(settings)
            self.assertEqual(env_on.get('ISS_KBD_PROMPTS_ATLAS'), str(self.atlas))
            self.assertTrue(self.atlas.is_file())
            meta = json.loads(self.atlas.with_suffix('.bin.json').read_text())
            self.assertEqual(meta['bindings']['cross'], 'V')

            # Back off: env var removed cleanly.
            run.set_mod_enabled(run.KBD_PROMPTS_MOD_KEY, False)
            env_off2 = run.build_env(settings)
            self.assertNotIn('ISS_KBD_PROMPTS_ATLAS', env_off2)


if __name__ == '__main__':
    unittest.main()
