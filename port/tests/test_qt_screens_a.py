"""Graphics and Controls screens of the Qt launcher: padsettings.py (key map, mouse, touchpad mode),
qt/gfxlogic.py (presets, VRAM rule and detection) and their controllers (qt/graphics.py,
qt/controls.py). The pure parts need no PySide6; the controller tests skip without it."""
import json
import os
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
from qt import logic  # noqa: E402
from qt import gfxlogic  # noqa: E402
import backports_table  # noqa: E402
import padsettings  # noqa: E402

try:
    from qt.backend import Backend
    from qt.graphics import Graphics
    from qt.controls import Controls
    HAVE_QT = True
except ImportError:
    HAVE_QT = False

QT = ROOT / 'launcher' / 'qt'
STRINGS = json.loads((QT / 'i18n' / 'strings.json').read_text(encoding='utf-8'))
RUNTIME_PAD = ROOT / 'src' / 'runtime_pad.c'
SETTINGS_CPP = ROOT / 'gpu' / 'shim' / 'tlg_settings.cpp'

BTN = {'BTN_CROSS': 'cross', 'BTN_CIRCLE': 'circle', 'BTN_SQUARE': 'square', 'BTN_TRIANGLE': 'triangle',
       'BTN_L1': 'l1', 'BTN_R1': 'r1', 'BTN_L2': 'l2', 'BTN_R2': 'r2', 'BTN_L3': 'l3', 'BTN_R3': 'r3',
       'BTN_OPTIONS': 'options', 'BTN_UP': 'up', 'BTN_DOWN': 'down', 'BTN_LEFT': 'left', 'BTN_RIGHT': 'right',
       'CLICK_LEFT': 'touchpad_left', 'CLICK_RIGHT': 'touchpad_right'}


class KeyboardTableTests(unittest.TestCase):
    """The default keyboard table of the screen is the one in the game's code."""
    def c_key_map(self):
        text = RUNTIME_PAD.read_text(encoding='utf-8')
        body = text[text.index('static ButtonMap key_map='):]
        body = body[:body.index('},')] if False else body[:body.index('\n},')]
        return {BTN[b]: s for s, b in re.findall(r'\{SDL_SCANCODE_(\w+),(BTN_\w+|CLICK_\w+)\}', body)}

    def test_default_keys_match_runtime_pad_c(self):
        mine = {button: scancode for button, scancode, _ in padsettings.KEYBOARD_BUTTONS}
        self.assertEqual(self.c_key_map(), mine)

    def test_fixed_keys_exist_in_runtime_pad_c(self):
        text = RUNTIME_PAD.read_text(encoding='utf-8')
        for scancode in ('W', 'A', 'S', 'D', 'LEFT', 'RIGHT', 'UP', 'DOWN', 'T', 'G', 'Y'):
            self.assertIn(f'SDL_SCANCODE_{scancode}]', text, scancode)

    def test_ps4_button_names_are_the_ones_apply_button_map_reads(self):
        text = RUNTIME_PAD.read_text(encoding='utf-8')
        listed = set(re.findall(r'\{"(\w+)",(?:BTN|CLICK)_\w+\}', text[text.index('ps_names[]'):text.index('typedef struct { int source')]))
        self.assertEqual(listed, set(padsettings.PS4_BUTTONS))

    @unittest.skipUnless('mouse_enabled' in SETTINGS_CPP.read_text(encoding='utf-8'), 'mouse code is on feat/mouse-camera')
    def test_mouse_keys_exist_in_tlg_settings(self):
        text = SETTINGS_CPP.read_text(encoding='utf-8')
        for key, *_ in padsettings.MOUSE_KEYS:
            self.assertIn(f'"{key}"', text)

    @unittest.skipUnless('TLG_MOUSE_PAD_KEY' in RUNTIME_PAD.read_text(encoding='utf-8'), 'mouse code is on feat/mouse-camera')
    def test_touchpad_mode_variables_exist_in_runtime_pad(self):
        text = RUNTIME_PAD.read_text(encoding='utf-8')
        for name in ('TLG_MOUSE_PAD', 'TLG_MOUSE_PAD_MODE', 'TLG_MOUSE_PAD_SENS', 'TLG_MOUSE_PAD_DRAG',
                     'TLG_MOUSE_PAD_KEY', 'TLG_KEY_MAP'):
            self.assertIn(f'"{name}"', text)
        for name in padsettings.MOUSE_SOURCES:
            self.assertIn(f'"{name}"', text)
        self.assertIn(padsettings.PAD_DEFAULT_KEY, text)


class KeyMapTests(unittest.TestCase):
    def test_empty_and_valid(self):
        self.assertEqual(([], [], []), padsettings.parse_key_map(''))
        entries, errors, warnings = padsettings.parse_key_map('cross=Q, R2 = Mouse Left | F ,l1=none')
        self.assertEqual([('cross', ['Q']), ('r2', ['Mouse Left', 'F']), ('l1', None)], entries)
        self.assertEqual([], errors + warnings)

    def test_case_insensitive_like_the_game(self):
        entries, errors, warnings = padsettings.parse_key_map('CROSS=left shift,Touchpad=tab')
        self.assertEqual([], errors + warnings)
        self.assertEqual(2, len(entries))

    def test_format_errors(self):
        _, errors, _ = padsettings.parse_key_map('cross=Q,r1,foo=A,l2=,l3= | ')
        self.assertEqual([('keymap_err_format', 'r1'), ('keymap_err_button', 'foo'),
                          ('keymap_err_empty', 'l2'), ('keymap_err_empty', 'l3')], errors)

    def test_limits_of_the_c_buffers(self):
        _, errors, _ = padsettings.parse_key_map('cross=' + 'Q|' * 9 + 'F')
        self.assertEqual([('keymap_err_many', 'cross')], errors)
        _, errors, _ = padsettings.parse_key_map('cross=' + 'Q' * 1100)
        self.assertEqual('keymap_err_long', errors[0][0])

    def test_unknown_key_name_is_only_a_warning(self):
        entries, errors, warnings = padsettings.parse_key_map('cross=Banana')
        self.assertEqual([], errors)
        self.assertEqual([('keymap_warn_key', 'Banana')], warnings)
        self.assertEqual([('cross', ['Banana'])], entries)

    def test_effective_keys_apply_the_map_and_none(self):
        keys = padsettings.effective_keys('cross=Q,l1=none,touchpad=X')
        self.assertEqual(['Q'], keys['cross'])
        self.assertEqual([], keys['l1'])
        self.assertEqual(['X'], keys['touchpad_left'])
        self.assertEqual(['Left Shift'], keys['circle'])
        self.assertEqual(['E'], padsettings.effective_keys('cross=')['square'])   # a bad entry changes nothing
        self.assertEqual(['Space'], padsettings.effective_keys('cross=')['cross'])


class MouseAndEnvironmentTests(unittest.TestCase):
    def test_mouse_defaults_and_clamping(self):
        self.assertTrue(padsettings.mouse_value({}, 'mouse_enabled'))
        self.assertEqual(1.0, padsettings.mouse_value({}, 'mouse_sensitivity'))
        self.assertEqual(10.0, padsettings.mouse_value({'mouse_sensitivity': '99'}, 'mouse_sensitivity'))
        self.assertEqual(1.0, padsettings.mouse_value({}, 'mouse_sensitivity_y'))
        self.assertEqual(4.0, padsettings.mouse_value({'mouse_sensitivity_y': '9'}, 'mouse_sensitivity_y'))
        self.assertEqual(0.5, padsettings.mouse_value({'mouse_smoothing': 'abc'}, 'mouse_smoothing'))
        self.assertFalse(padsettings.mouse_value({'mouse_enabled': '0'}, 'mouse_enabled'))

    def test_sensitivity_sliders_reach_the_game_range(self):
        # #435/#439: the slider stopped at 5 while the game takes up to 10.
        for key in ('mouse_sensitivity', 'mouse_sensitivity_y'):
            _, _, _, hard, slide = padsettings.MOUSE_SPEC[key]
            self.assertEqual(hard[1], slide[1], key)
        text = SETTINGS_CPP.read_text(encoding='utf-8')
        self.assertIn('Clamp(value.empty() ? 1.0f : f, 0.05f, 10.0f)', text)
        self.assertIn('Clamp(value.empty() ? 1.0f : f, 0.25f, 4.0f)', text)
        self.assertIn('{"TLG_MOUSE_SENS_Y", "mouse_sensitivity_y"}', text)

    def test_update_removes_the_key_at_its_default(self):
        self.assertEqual({'mouse_sensitivity': None}, padsettings.mouse_update('mouse_sensitivity', 1.0))
        self.assertEqual({'mouse_sensitivity': '1.50'}, padsettings.mouse_update('mouse_sensitivity', 1.5))
        self.assertEqual({'mouse_enabled': '0'}, padsettings.mouse_update('mouse_enabled', False))
        self.assertEqual({'mouse_enabled': None}, padsettings.mouse_update('mouse_enabled', True))
        self.assertEqual({'mouse_deadzone': '0.50'}, padsettings.mouse_update('mouse_deadzone', 7))

    def test_nothing_is_exported_by_default(self):
        self.assertEqual({}, padsettings.environment({}))
        self.assertEqual({}, padsettings.environment(dict(padsettings.PAD_DEFAULTS)))
        self.assertEqual({}, padsettings.environment({'mouse_pad_key': padsettings.PAD_DEFAULT_KEY}))

    def test_changes_become_iss_variables(self):
        env = padsettings.environment({'mouse_pad': False, 'mouse_pad_key': 'Mouse X2', 'mouse_pad_mode': 'toggle',
                                       'mouse_pad_sens': 2.0, 'mouse_pad_drag': 0, 'key_map': 'cross=Q'})
        self.assertEqual({'ISS_MOUSE_PAD': '0', 'ISS_MOUSE_PAD_KEY': 'Mouse X2', 'ISS_MOUSE_PAD_MODE': 'toggle',
                          'ISS_MOUSE_PAD_SENS': '2.00', 'ISS_MOUSE_PAD_DRAG': '0', 'ISS_KEY_MAP': 'cross=Q'}, env)

    def test_an_invalid_key_map_is_not_exported(self):
        self.assertEqual({}, padsettings.environment({'key_map': 'cross'}))

    def test_build_env_uses_it_and_the_system_wins(self):
        run = logic.tlg_run
        settings = dict(run.DEFAULT_SETTINGS, mouse_pad_mode='toggle', key_map='cross=Q')
        with mock.patch.dict(os.environ, {'ISS_KEY_MAP': 'cross=E'}):
            env = run.build_env(settings)
        self.assertEqual('toggle', env['ISS_MOUSE_PAD_MODE'])
        self.assertEqual('cross=E', env['ISS_KEY_MAP'])     # set outside the launcher: it wins
        env = run.build_env(dict(run.DEFAULT_SETTINGS))
        self.assertNotIn('ISS_MOUSE_PAD_MODE', env)

    def test_pad_kind(self):
        kinds = [padsettings.pad_kind(n) for n in ('DualSense', 'DualShock 4', 'Xbox', 'Switch Pro', 'Foo', None)]
        self.assertEqual(['playstation', 'playstation', 'xbox', 'switch', 'other', 'none'], kinds)


class VramAndPresetTests(unittest.TestCase):
    def test_budget_follows_the_tk_table(self):
        for gb, mb in ((6, 3500), (8, 5000), (12, 8500), (16, 12000), (24, 19000)):
            self.assertEqual(mb, gfxlogic.suggested_budget_mb(gb), gb)
        self.assertEqual(6800, gfxlogic.suggested_budget_mb(10))     # between the 8 and 12 GB points
        self.assertEqual(2000, gfxlogic.suggested_budget_mb(2))      # never under the Tk minimum
        self.assertEqual(24000, gfxlogic.suggested_budget_mb(48))    # nor over its maximum
        values = [gfxlogic.suggested_budget_mb(g) for g in range(3, 40)]
        self.assertEqual(sorted(values), values)

    def test_budget_warning(self):
        self.assertTrue(gfxlogic.budget_is_too_high(6000, 6144))
        self.assertFalse(gfxlogic.budget_is_too_high(3500, 6144))
        self.assertFalse(gfxlogic.budget_is_too_high(9000, None))

    def test_tk_hint_names_the_same_points(self):
        hint = logic.tk_text()['vram_hint'][0]
        for gb, mb in gfxlogic.BUDGET_POINTS:
            self.assertIn(f'{gb} GB', hint)
            self.assertIn(str(mb), hint)

    def test_nvidia_smi(self):
        self.assertEqual(24564, gfxlogic.parse_nvidia_smi('6144\n24564\n'))
        self.assertIsNone(gfxlogic.parse_nvidia_smi('No devices were found'))

    def test_vulkan_heaps(self):
        text = '''GPU0:
VkPhysicalDeviceProperties:
	deviceName        = NVIDIA GeForce RTX 2060
VkPhysicalDeviceMemoryProperties:
	memoryHeapCount = 2
	memoryHeaps[0]:
		size   = 6442450944 (0x180000000) (6.00 GiB)
		budget = 5000000000 (0x12a05f200) (4.66 GiB)
		flags:
			MEMORY_HEAP_DEVICE_LOCAL_BIT
	memoryHeaps[1]:
		size   = 33554432000 (0x7d0000000) (31.25 GiB)
		flags:
			None
	memoryTypes[0]:
		heapIndex     = 0
GPU1:
VkPhysicalDeviceProperties:
	deviceName        = llvmpipe (LLVM 19.1.7, 256 bits)
VkPhysicalDeviceMemoryProperties:
	memoryHeaps[0]:
		size   = 70000000000 (0x1) (65.19 GiB)
		flags:
			MEMORY_HEAP_DEVICE_LOCAL_BIT
'''
        self.assertEqual(6144, gfxlogic.parse_vulkan_heaps(text, 'NVIDIA GeForce RTX 2060'))
        self.assertEqual(66757, gfxlogic.parse_vulkan_heaps(text))     # no name: the largest device
        self.assertIsNone(gfxlogic.parse_vulkan_heaps('nothing here'))

    def test_detection_order_and_failures(self):
        class Out:
            def __init__(self, stdout):
                self.stdout = stdout
        calls = []

        def run(cmd, **kw):
            calls.append(cmd[0])
            if cmd[0] == 'nvidia-smi':
                raise FileNotFoundError
            return Out('memoryHeaps[0]:\n size = 8589934592\n MEMORY_HEAP_DEVICE_LOCAL_BIT\n')
        self.assertEqual(7000, gfxlogic.detect_vram_mb(run=run, sysfs=lambda: 7000))     # sysfs before vulkaninfo
        self.assertEqual(['nvidia-smi'], calls)
        self.assertEqual(8192, gfxlogic.detect_vram_mb(run=run, sysfs=lambda: None))
        self.assertEqual(['nvidia-smi', 'nvidia-smi', 'vulkaninfo'], calls)

        def fail(cmd, **kw):
            raise FileNotFoundError
        self.assertIsNone(gfxlogic.detect_vram_mb(run=fail, sysfs=lambda: None))

    def test_sysfs(self):
        with tempfile.TemporaryDirectory() as folder:
            for n, size in enumerate((4 * 2 ** 30, 8 * 2 ** 30)):
                (Path(folder) / f'card{n}').mkdir()
                (Path(folder) / f'card{n}' / 'mem').write_text(str(size))
            self.assertEqual(8192, gfxlogic.read_sysfs_vram(f'{folder}/card*/mem'))
            self.assertIsNone(gfxlogic.read_sysfs_vram(f'{folder}/nothing*'))

    def test_presets_only_use_keys_that_exist(self):
        ids = {i.id for i in backports_table.ITEMS}
        self.assertTrue(set(gfxlogic.PRESET_ITEMS) <= ids)
        for preset_id, settings, items in gfxlogic.PRESETS:
            if preset_id in gfxlogic.FULL_PRESETS:      # a full preset also owns keys the plain ones never touch
                self.assertTrue(set(settings) <= set(gfxlogic.SETTING_DEFAULTS))
                self.assertTrue(set(items) <= ids)
                continue
            self.assertTrue(set(settings) <= set(gfxlogic.PRESET_SETTINGS))
            self.assertTrue(set(items) <= set(gfxlogic.PRESET_ITEMS))
            for item_id, value in items.items():       # a value the table accepts (not silently dropped)
                item = gfxlogic.item_by_id(item_id)
                self.assertIn(value, [item.default] + [c[0] for c in item.choices] + ['0', '1'], item_id)
        self.assertEqual(('rec1', 'current', 'amd', 'rx580', 'seco', 'apolar_amd', 'apolar_rtx', 'apolar_gtx',
                          'renderer_v2_8gb', 'renderer_v2_12gb', 'renderer_v2', 'renderer_v2_native', 'performance', 'balanced', 'quality', 'ps4'),
                         tuple(p[0] for p in gfxlogic.PRESETS))

    def test_apolar_profiles_are_complete_and_credit_the_contributor(self):
        expected = {
            'apolar_amd': (True, '30', '1500000', None, 86),
            'apolar_rtx': (True, '30', '600000', '1', 86),
            'apolar_gtx': (False, '24', '400000', '1', 82),
        }
        for preset_id, (vsync, fps, far_clip, linear, item_count) in expected.items():
            source = gfxlogic.FULL_PRESETS[preset_id]
            self.assertEqual('Apolar', source['credit'])
            self.assertEqual('https://github.com/sapoguruoficial-cmd', source['credit_url'])
            self.assertEqual(item_count, len(source['items']))
            settings, items = gfxlogic.preset_targets(preset_id)
            self.assertEqual((vsync, True, '2'),
                             (settings['vsync'], settings['show_fps'], settings['gpu_readbacks']))
            self.assertEqual((fps, far_clip, linear),
                             (items['menu:fps_limit'], items['menu:far_clip'], items['TLG_READBACK_LINEAR']))
            self.assertEqual(preset_id, gfxlogic.matching_preset(settings, items))

    def test_current_preset_is_the_maintainers_setup_and_plain_presets_leave_its_keys_alone(self):
        import infamous_run
        for key, default in gfxlogic.SETTING_DEFAULTS.items():      # kept equal to the launcher's defaults
            self.assertEqual(str(default).lower(), str(infamous_run.DEFAULT_SETTINGS[key]).lower(), key)
        settings, stored = gfxlogic.preset_targets('current')
        self.assertEqual(('2', False, True), (settings['gpu_readbacks'], settings['eager_readback'],
                                              settings['write_merge']))
        self.assertEqual((None, '0', '1', None), tuple(stored[k] for k in ('menu:fsr1', 'menu:rcas', 'BP07', 'BP08')))   # no FSR 1, no sharpening; BP08 on is the default
        for key in ('menu:lod_scale', 'menu:region_lod', 'menu:region_fade', 'menu:near_clip', 'menu:far_clip',
                    'menu:mip_lod_bias'):                      # draw distance and LOD: always native (nothing stored)
            self.assertIsNone(stored[key], key)
        self.assertIsNone(stored['BP06'])      # on is the default
        self.assertIsNone(stored['menu:fps_limit'])
        for preset_id in ('performance', 'balanced', 'quality', 'ps4'):
            plain_settings, plain_items = gfxlogic.preset_targets(preset_id)
            self.assertEqual({'vsync'}, set(plain_settings), preset_id)
            self.assertNotIn('menu:lod_scale', plain_items)
            self.assertNotIn('BP07', plain_items)
        # the plain presets do not reach its keys, so it is only recognised when they all match
        base_settings, base_items = gfxlogic.preset_targets('balanced')
        self.assertEqual('balanced', gfxlogic.matching_preset(base_settings, base_items))
        self.assertEqual('current', gfxlogic.matching_preset(settings, stored))
        self.assertNotEqual('current', gfxlogic.matching_preset(settings, dict(stored, **{'menu:lod_scale': '0.5'})))

    def test_the_amd_preset_is_the_current_one_with_the_safest_choices_for_amd(self):
        _, current = gfxlogic.preset_targets('current')
        settings, amd = gfxlogic.preset_targets('amd')
        _, current_settings = gfxlogic.preset_targets('current')
        self.assertEqual(('2', False, True), (settings['gpu_readbacks'], settings['eager_readback'],
                                              settings['write_merge']))
        self.assertEqual('0', amd['BP06'])                      # suspected on AMD (RX 7600, driver 26.8.1)
        self.assertIsNone(current['BP06'])                      # the default (on): listed, so leaving AMD restores it
        self.assertEqual('0', amd['BP08'])                      # on is the default (2026-10-16): off is stored
        self.assertIsNone(current['BP08'])
        self.assertEqual(('1', '1', '1'), (amd['TLG_BLOCK1D'], amd['TLG_BC_TINY'], amd['TLG_NO_MSAA']))
        self.assertNotIn('TLG_BLOCK1D', current)                # the current preset leaves the AMD workarounds alone
        self.assertIsNone(amd.get('BP09'))                      # BP09 does not fix the RX 580 crash: not part of it
        for key in ('menu:fsr1', 'menu:lod_scale', 'menu:region_lod', 'menu:region_fade', 'menu:near_clip',
                    'menu:far_clip', 'menu:mip_lod_bias', 'BP07'):                # the rest is the currently recommended setup
            self.assertEqual(current[key], amd[key], key)
        s, st = gfxlogic.preset_targets('amd')
        self.assertEqual('amd', gfxlogic.matching_preset(s, st))
        s, st = gfxlogic.preset_targets('current')
        self.assertEqual('current', gfxlogic.matching_preset(s, st))
        # the maintainer's own state (AMD workarounds explicit on a non-AMD PC) still reads as "current"
        self.assertEqual('current', gfxlogic.matching_preset(s, dict(st, TLG_BLOCK1D='1', TLG_BC_TINY='1', TLG_NO_MSAA='1')))

    def test_balanced_is_the_default_and_ps4_is_30fps_without_fsr(self):
        settings, stored = gfxlogic.preset_targets('balanced')
        self.assertEqual({'vsync': True}, settings)
        self.assertTrue(all(v is None for v in stored.values()))
        for preset_id in ('performance', 'balanced', 'quality', 'ps4'):  # background shader compile is never undone by a preset
            _, items = gfxlogic.preset_targets(preset_id)
            self.assertIsNone(items['TLG_ASYNC_SHADERS'], preset_id)
        _, ps4 = gfxlogic.preset_targets('ps4')
        self.assertEqual('30', ps4['menu:fps_limit'])
        self.assertIsNone(ps4['menu:fsr1'])           # off is the default
        self.assertEqual('0', ps4['menu:rcas'])       # sharpening is on by default: off is stored
        self.assertEqual({'vsync': False}, gfxlogic.preset_targets('performance')[0])

    def test_view_presets(self):
        _, rec1 = gfxlogic.preset_targets('rec1')
        _, rec2 = gfxlogic.preset_targets('current')
        _, bare = gfxlogic.preset_targets('seco')
        self.assertEqual(('0.6', '0', '0.8', '1418000', '1.25'),
                         tuple(rec1['menu:' + k] for k in ('lod_scale', 'region_lod', 'region_fade', 'far_clip', 'mip_lod_bias')))
        for key in ('lod_scale', 'region_lod', 'region_fade', 'far_clip'):   # Recommended 2: native (default: not stored)
            self.assertIsNone(rec2['menu:' + key], key)
        for key in ('lod_scale', 'region_lod', 'region_fade', 'far_clip'):   # Bare: the lowest of each range
            item = gfxlogic.item_by_id('menu:' + key)
            self.assertEqual(float(item.lo), float(bare['menu:' + key]), key)
        self.assertEqual(3.0, float(bare['menu:mip_lod_bias']))              # the blurriest textures
        for key, value in rec2.items():                                      # the rest is Recommended 2
            if not key.startswith(('menu:lod', 'menu:region', 'menu:far', 'menu:near', 'menu:mip')):
                self.assertEqual(value, rec1[key], key)
                self.assertEqual(value, bare[key], key)

    def test_every_preset_matches_itself_only(self):
        for preset_id, _, _ in gfxlogic.PRESETS:
            settings, stored = gfxlogic.preset_targets(preset_id)
            self.assertEqual(preset_id, gfxlogic.matching_preset(settings, stored))
        self.assertIsNone(gfxlogic.matching_preset({'vsync': True}, {'menu:fps_limit': '60'}))


class StringsTests(unittest.TestCase):
    def used_keys(self):
        used = set()
        for path in list((QT / 'qml' / 'Launcher').glob('*.qml')) + [QT / 'graphics.py', QT / 'controls.py']:
            used |= set(re.findall(r'(?:tr\(|t_\(|\bt\(|\btf\()["\'](\w+)["\']', path.read_text(encoding='utf-8')))
        return used

    def test_every_key_the_screens_use_exists(self):
        tk = logic.tk_text()
        for key in (k for k in self.used_keys() if not k.endswith('_')):
            self.assertTrue(key in STRINGS or key in tk, key)

    def test_eager_readback_is_off_by_default_and_migrated_once(self):
        import infamous_run
        self.assertFalse(infamous_run.DEFAULT_SETTINGS['eager_readback'])
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'settings.json'
            with mock.patch.object(infamous_run, 'config_path', lambda: path):
                path.write_text(json.dumps({'eager_readback': True}))   # saved before the default changed
                settings = infamous_run.load_settings()
                self.assertFalse(settings['eager_readback'])
                settings['eager_readback'] = True                        # the player turns it back on
                infamous_run.save_settings(settings)
                self.assertTrue(infamous_run.load_settings()['eager_readback'])

    def test_built_keys_exist(self):
        for pid, _, _ in gfxlogic.PRESETS:
            self.assertIn('gfx_preset_' + pid, STRINGS)
            self.assertIn(f'gfx_preset_{pid}_what', STRINGS)
        for key in ('display_mode', 'vsync', 'show_fps', 'gpu_readbacks', 'eager_readback', 'write_merge',
                    'vram_budget_mb'):
            self.assertIn('gfx_what_' + key, STRINGS)
        for kind in ('playstation', 'xbox', 'switch', 'other', 'none'):
            self.assertIn('ctl_kind_' + kind, STRINGS)
        for kind in ('playstation', 'xbox', 'switch', 'other'):
            self.assertIn('ctl_hint_' + kind, STRINGS)
        for key, *_ in padsettings.MOUSE_KEYS:
            if key != 'mouse_enabled':
                self.assertIn('ctl_' + key, STRINGS)
                self.assertIn('ctl_what_' + key, STRINGS)
        for button, _, _ in padsettings.KEYBOARD_BUTTONS:
            self.assertIn('pad_btn_' + button, STRINGS)
        if HAVE_QT:
            from qt.controls import PAD_KEY_LABELS
            for label in PAD_KEY_LABELS.values():
                self.assertIn(label, STRINGS)
        for code in ('keymap_err_format', 'keymap_err_button', 'keymap_err_empty', 'keymap_err_many',
                     'keymap_err_long', 'keymap_warn_key'):
            self.assertIn(code, STRINGS)

    def test_main_loads_both_screens(self):
        main = (QT / 'qml' / 'Launcher' / 'Main.qml').read_text(encoding='utf-8')
        self.assertIn('GraphicsPage', main)
        self.assertIn('ControlsPage', main)


@unittest.skipUnless(HAVE_QT, 'PySide6 not installed')
class ControllerTests(unittest.TestCase):
    """Real files in a temporary data folder: what the screens write is what the game and Tk read."""
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        patcher = mock.patch.dict(os.environ, {'XDG_CONFIG_HOME': str(self.root / 'cfg'),
                                               'ISS_USER_DIR': str(self.root / 'user')})
        patcher.start()
        self.addCleanup(patcher.stop)
        os.environ.pop('TLG_USER_DIR', None)
        probes = {'gpu': lambda: ('NVIDIA GeForce RTX 2060', '1.4'), 'pad': lambda: 'DualSense',
                  'problems': lambda: [], 'cache': lambda: 5 * 2 ** 20, 'save': lambda: None, 'news': lambda: [],
                  'version': lambda: 'v1',
                  'saves': {'running': lambda: self.running, 'scan': lambda: [], 'open': lambda p: None}}
        self.running = False
        settings = dict(logic.tlg_run.DEFAULT_SETTINGS, game_dir='/g', ui_language='en_US')
        self.backend = Backend(settings=settings, probes=probes)
        self.freed = []
        self.graphics = Graphics(self.backend, {'vram': lambda: 6144,
                                                'clear': lambda: self.freed.append(1) or (5 * 2 ** 20, [])})
        self.controls = Controls(self.backend)
        self.ini = self.root / 'user' / 'tlg.ini'

    def ini_text(self):
        return self.ini.read_text() if self.ini.exists() else ''

    def saved_settings(self):
        return json.loads(logic.tlg_run.config_path().read_text())

    # -- Graphics --------------------------------------------------------------------------------------
    def test_rows_cover_the_launcher_video_box_and_the_menu_tabs(self):
        ids = [r['id'] for r in self.graphics.model.rows if r['kind'] == 'item']
        for rid in ('s:display_mode', 's:vsync', 's:show_fps', 's:gpu_readbacks', 's:eager_readback',
                    's:write_merge', 's:vram_budget_mb', 'menu:fsr1', 'menu:rcas', 'menu:rcas_strength',
                    'menu:fps_limit', 'menu:frames_queued', 'TLG_ASYNC_SHADERS', 'TLG_DEFERRED_READBACK',
                    'TLG_STALE_READBACK', 'TLG_BACKGROUND_WARMUP', 'TLG_BARRIER_MERGE', 'menu:overlay_corner'):
            self.assertIn(rid, ids)
        self.assertEqual(len(ids), len(set(ids)))
        for row in self.graphics.model.rows:
            if row['kind'] == 'item':
                self.assertTrue(row['tags'][0]['text'] in ('in game', 'restarts the game'), row['id'])

    def test_live_and_restart_tags_follow_the_table(self):
        row = self.graphics.model.get
        self.assertEqual('in game', row('menu:fps_limit')['tags'][0]['text'])
        self.assertEqual('restarts the game', row('s:vram_budget_mb')['tags'][0]['text'])
        self.assertEqual('restarts the game', row('TLG_STALE_READBACK')['tags'][0]['text'])
        self.assertEqual('in game', row('TLG_ASYNC_SHADERS')['tags'][0]['text'])

    def test_settings_json_rows_write_the_same_keys_as_the_tk_launcher(self):
        self.graphics.setValue('s:display_mode', 'Borderless')
        self.graphics.setValue('s:vsync', '0')
        self.graphics.setValue('s:show_fps', '1')
        self.graphics.setValue('s:gpu_readbacks', '1')
        self.graphics.setValue('s:vram_budget_mb', '99999')
        saved = self.saved_settings()
        self.assertEqual(('Borderless', False, True, '1', 24000),
                         (saved['display_mode'], saved['vsync'], saved['show_fps'], saved['gpu_readbacks'],
                          saved['vram_budget_mb']))
        self.assertTrue(self.graphics.model.get('s:vsync')['changed'])
        self.assertEqual(1, self.graphics.model.get('s:display_mode')['index'])

    def test_menu_rows_go_through_the_fixes_path_into_tlg_ini(self):
        self.graphics.setValue('menu:fps_limit', '60')
        self.graphics.setValue('menu:rcas_strength', '40')
        self.assertIn('fps_limit=60', self.ini_text())
        self.assertIn('rcas_strength=40', self.ini_text())
        self.assertEqual('60', self.backend.fix_states['menu:fps_limit'])
        self.graphics.setValue('menu:fps_limit', '0')           # back to the default: the key goes
        self.assertNotIn('fps_limit', self.ini_text())
        row = self.graphics.model.get('menu:fps_limit')
        self.assertEqual('0', row['options'][row['index']]['value'])

    def test_a_row_changed_on_the_fixes_screen_shows_here(self):
        self.backend.setFix('TLG_STALE_READBACK', '1')
        self.assertTrue(self.graphics.model.get('TLG_STALE_READBACK')['on'])
        self.assertTrue(self.graphics.model.get('TLG_STALE_READBACK')['changed'])

    def test_rx580_toggle_shares_and_persists_the_advanced_setting(self):
        item_id = 'TLG_BARRIER_MERGE'
        with mock.patch.dict(os.environ, {}, clear=False):
            os.environ.pop('ISS_BARRIER_MERGE', None)
            os.environ.pop('TLG_BARRIER_MERGE', None)
            row = self.graphics.model.get(item_id)
            self.assertFalse(row['on'])
            self.assertFalse(row['changed'])

            self.graphics.setValue(item_id, '1')
            self.assertIn('adv_barrier_merge=1', self.ini_text())
            self.assertTrue(self.graphics.model.get(item_id)['on'])
            self.assertEqual('1', logic.tlg_run.build_env(self.backend.settings)['ISS_BARRIER_MERGE'])

            self.backend.setFix(item_id, '0')
            self.assertNotIn('adv_barrier_merge=', self.ini_text())
            self.assertFalse(self.graphics.model.get(item_id)['on'])
            self.assertFalse(self.graphics.model.get(item_id)['changed'])
            self.assertNotIn('ISS_BARRIER_MERGE', logic.tlg_run.build_env(self.backend.settings))

    def test_sharpness_strength_is_disabled_without_rcas(self):
        self.assertFalse(self.graphics.model.get('menu:rcas_strength')['disabled'])
        self.graphics.setValue('menu:rcas', '0')
        self.assertTrue(self.graphics.model.get('menu:rcas_strength')['disabled'])

    def test_presets_apply_and_are_recognised(self):
        self.assertEqual('Current preset: Balanced', self.graphics.presetText)
        self.graphics.applyPreset('ps4')
        self.assertIn('fps_limit=30', self.ini_text())
        self.assertIn('rcas=0', self.ini_text())
        self.assertEqual('Current preset: True to PS4', self.graphics.presetText)
        self.assertEqual(['ps4'], [p['id'] for p in self.graphics.presets if p['active']])
        self.graphics.applyPreset('performance')               # undoes what the PS4 one set
        self.assertNotIn('fps_limit', self.ini_text())
        self.assertIn('frames_queued=2', self.ini_text())
        self.assertFalse(self.saved_settings()['vsync'])
        self.graphics.applyPreset('balanced')
        self.assertTrue(self.saved_settings()['vsync'])
        self.assertNotIn('frames_queued', self.ini_text())
        self.graphics.setValue('menu:fps_limit', '120')
        self.assertIn('custom', self.graphics.presetText)

    def test_applying_the_current_preset_stores_every_key_and_balanced_undoes_only_its_own(self):
        self.graphics.applyPreset('current')
        saved = self.saved_settings()
        self.assertEqual(('2', False, True), (saved['gpu_readbacks'], saved['eager_readback'], saved['write_merge']))
        ini = self.ini_text()
        for line in ('rcas=0', 'bp07=1'):
            self.assertIn(line, ini)
        self.assertNotIn('bp08', ini)                 # on is its default: nothing stored
        self.assertNotIn('fsr1=1', ini)
        for key in ('lod_scale', 'region_lod', 'region_fade', 'near_clip', 'far_clip', 'mip_lod_bias', 'lod_bands'):
            self.assertNotIn(key + '=', ini)
        self.assertIn('current', self.graphics.presetText.lower())
        self.graphics.applyPreset('balanced')
        ini = self.ini_text()
        self.assertNotIn('rcas=0', ini)               # balanced: sharpening back to its default (on)
        self.assertIn('bp07=1', ini)

    def test_applying_a_full_preset_sends_a_changed_draw_distance_back_to_native(self):
        for preset in ('current', 'amd'):
            self.graphics.setValue('menu:lod_scale', '0.6')
            self.graphics.setValue('menu:far_clip', '1418000')
            self.assertIn('lod_scale=0.6', self.ini_text())
            self.graphics.applyPreset(preset)
            ini = self.ini_text()
            for key in ('lod_scale', 'far_clip'):
                self.assertNotIn(key + '=', ini, preset)

    def test_applying_the_amd_preset_and_going_back_to_the_current_one(self):
        self.graphics.applyPreset('amd')
        ini = self.ini_text()
        for line in ('bp06=0', 'block1d=1', 'bc_tiny=1', 'no_msaa=1', 'rcas=0', 'bp07=1'):
            self.assertIn(line, ini)
        self.assertIn('bp08=0', ini)                   # the AMD preset turns it off
        self.assertIn('amd', self.graphics.presetText.lower())
        self.graphics.applyPreset('current')
        ini = self.ini_text()
        self.assertNotIn('bp06', ini)                  # back to the default (on)
        self.assertNotIn('bp08', ini)                  # back to the default (on): nothing stored
        self.assertIn('block1d=1', ini)                # the current preset leaves the AMD workarounds as they are

    def test_presets_leave_other_choices_alone(self):
        self.graphics.setValue('s:display_mode', 'Fullscreen')
        self.graphics.setValue('s:vram_budget_mb', '4000')
        self.graphics.setValue('menu:overlay_corner', '3')
        self.backend.setFix('BP06', '0')
        self.graphics.applyPreset('ps4')
        saved = self.saved_settings()
        self.assertEqual(('Fullscreen', 4000), (saved['display_mode'], saved['vram_budget_mb']))
        self.assertIn('overlay_corner=3', self.ini_text())
        self.assertIn('bp06=0', self.ini_text())

    def test_vram_line_and_suggestion(self):
        self.backend._gpu = ('NVIDIA GeForce RTX 2060', '1.4')
        self.graphics.set_vram(6144)
        self.assertEqual('Your GPU (NVIDIA GeForce RTX 2060) has 6 GB of VRAM. Suggested budget: 3500 MB.',
                         self.graphics.vramLine)
        self.assertEqual(3500, self.graphics.vramSuggested)
        self.assertFalse(self.graphics.canSuggest)            # 3500 is already the default
        self.graphics.setValue('s:vram_budget_mb', '6000')
        self.assertTrue(self.graphics.canSuggest)
        self.assertIn('6000 MB', self.graphics.vramWarn)
        row = self.graphics.model.get('s:vram_budget_mb')
        self.assertEqual('Use 3500 MB', row['action']['label'])
        self.graphics.runAction('s:vram_budget_mb', 'suggest')
        self.assertEqual(3500, self.saved_settings()['vram_budget_mb'])
        self.assertEqual('', self.graphics.vramWarn)

    def test_unknown_vram_falls_back_to_the_tk_table(self):
        self.graphics.set_vram(None)
        self.assertIn('could not read', self.graphics.vramLine)
        self.assertIn('6 GB card: 3500', self.graphics.vramLine)
        self.assertFalse(self.graphics.canSuggest)

    def test_portuguese_texts(self):
        self.backend.settings['ui_language'] = 'pt_BR'
        self.backend.setLang('pt_BR')
        self.assertEqual('Predefinição atual: Equilibrado', self.graphics.presetText)
        self.assertEqual('no jogo', self.graphics.model.get('menu:fps_limit')['tags'][0]['text'])
        self.assertIn('Sem limite', self.graphics.model.get('menu:fps_limit')['options'][-1]['label'])

    def test_clear_cache_asks_first_and_never_while_playing(self):
        self.assertTrue(self.graphics.canClear)
        self.graphics.askClear()
        self.assertTrue(self.graphics.confirming)
        self.assertIn('5.0 MB', self.graphics.confirmText)
        self.graphics.cancelClear()
        self.assertEqual([], self.freed)
        self.graphics.askClear()
        self.graphics.clearCache()
        self.assertEqual([1], self.freed)
        self.assertEqual('good', self.graphics.messageTone)
        self.assertIn('5.0 MB freed', self.graphics.message)
        self.running = True
        self.assertFalse(self.graphics.canClear)
        self.graphics.askClear()
        self.assertFalse(self.graphics.confirming)
        self.graphics.clearCache()
        self.assertEqual([1], self.freed)
        self.assertEqual('bad', self.graphics.messageTone)

    # -- Controls --------------------------------------------------------------------------------------------
    def test_controller_card(self):
        self.assertEqual(('DualSense', 'PlayStation controller', 'playstation'),
                         (self.controls.padName, self.controls.padKindText, self.controls.padKind))
        self.backend.probes['pad'] = lambda: None
        self.backend.refresh()
        self.assertFalse(self.controls.padFound)
        self.assertEqual('No controller detected', self.controls.padName)
        self.assertEqual('', self.controls.padHint)

    def test_deadzone_is_the_launcher_setting_and_sensor_speed_the_menu_key(self):
        self.controls.setValue('c:stick_deadzone', '55')
        self.assertEqual(40, self.saved_settings()['stick_deadzone'])
        self.assertEqual('0.40', logic.tlg_run.build_env(self.backend.settings)['ISS_STICK_DEADZONE'])
        self.controls.setValue('menu:sensor_aim', '150')
        self.assertIn('sensor_aim=150', self.ini_text())
        self.assertEqual(150, self.controls.pad_model.get('menu:sensor_aim')['value'])

    def test_rumble_switch_is_the_tlg_ini_key_the_menu_shares(self):
        self.assertTrue(self.controls.pad_model.get('i:rumble')['on'])
        self.controls.setValue('i:rumble', '0')
        self.assertIn('rumble=0', self.ini_text())
        self.assertFalse(self.controls.pad_model.get('i:rumble')['on'])
        self.controls.setValue('i:rumble', '1')
        self.assertNotIn('rumble=', self.ini_text())     # the default again
        self.assertTrue(self.controls.pad_model.get('i:rumble')['on'])

    def test_mouse_keys_go_to_tlg_ini_and_defaults_remove_them(self):
        self.controls.setValue('m:mouse_sensitivity', '1.5')
        self.controls.setValue('m:mouse_invert_y', '1')
        self.assertIn('mouse_sensitivity=1.50', self.ini_text())
        self.assertIn('mouse_invert_y=1', self.ini_text())
        self.assertEqual(1.5, self.controls.mouse_model.get('m:mouse_sensitivity')['value'])
        self.assertTrue(self.controls.mouse_model.get('m:mouse_invert_y')['on'])
        self.controls.setValue('m:mouse_sensitivity', '1.0')
        self.controls.setValue('m:mouse_invert_y', '0')
        self.assertNotIn('mouse_', self.ini_text())

    def test_the_other_keys_of_tlg_ini_survive(self):
        self.ini.parent.mkdir(parents=True)
        self.ini.write_text('# tlg settings\nbp01=0\nfps_limit=60\n')
        self.controls.setValue('m:mouse_accel', '0.5')
        self.assertEqual({'bp01', 'fps_limit', 'mouse_accel'},
                         {l.split('=')[0] for l in self.ini_text().splitlines() if '=' in l})

    def test_turning_the_mouse_off_writes_mouse_enabled_0_and_dims_the_rows(self):
        self.assertTrue(self.controls.mouseEnabled)
        self.assertEqual('Turn the mouse off', self.controls.mouseButtonText)
        self.controls.toggleMouse()
        self.assertIn('mouse_enabled=0', self.ini_text())
        self.assertFalse(self.controls.mouseEnabled)
        self.assertEqual('Turn the mouse on', self.controls.mouseButtonText)
        self.assertTrue(all(r['disabled'] for r in self.controls.mouse_model.rows))
        self.assertTrue(all(r['disabled'] for r in self.controls.touch_model.rows))
        self.controls.toggleMouse()
        self.assertNotIn('mouse_enabled', self.ini_text())
        self.assertFalse(any(r['disabled'] for r in self.controls.mouse_model.rows))

    def test_touchpad_mode_goes_to_the_environment(self):
        self.controls.setValue('p:mouse_pad_mode', 'toggle')
        self.controls.setValue('p:mouse_pad_key', 'Mouse X2')
        self.controls.setValue('p:mouse_pad_sens', '2.5')
        self.controls.setValue('p:mouse_pad_drag', '0')
        saved = self.saved_settings()
        self.assertEqual(('toggle', 'Mouse X2', 2.5, 0),
                         (saved['mouse_pad_mode'], saved['mouse_pad_key'], saved['mouse_pad_sens'], saved['mouse_pad_drag']))
        env = logic.tlg_run.build_env(self.backend.settings)
        self.assertEqual(('toggle', 'Mouse X2', '2.50', '0'),
                         (env['ISS_MOUSE_PAD_MODE'], env['ISS_MOUSE_PAD_KEY'], env['ISS_MOUSE_PAD_SENS'],
                          env['ISS_MOUSE_PAD_DRAG']))
        self.controls.setValue('p:mouse_pad', '0')
        self.assertEqual('0', logic.tlg_run.build_env(self.backend.settings)['ISS_MOUSE_PAD'])
        row = self.controls.touch_model.get('p:mouse_pad_drag')
        self.assertEqual('never', row['valueText'])

    def test_a_hand_edited_pad_key_still_shows(self):
        self.backend.settings['mouse_pad_key'] = 'Right Alt'
        self.controls.rebuild()
        row = self.controls.touch_model.get('p:mouse_pad_key')
        self.assertEqual('Custom: Right Alt', row['options'][row['index']]['label'])

    def test_keyboard_table_marks_remapped_buttons(self):
        rows = {r['name']: r for r in self.controls.keyRows}
        self.assertEqual('Space', rows['Cross (X)']['keys'])
        self.assertFalse(any(r['changed'] for r in rows.values()))
        self.assertEqual(22, len(rows))      # 2 sticks, 17 remappable buttons, 3 sensor keys
        self.controls.setDraft('cross=Q,l1=none')
        self.assertTrue(self.controls.draftValid)
        self.controls.applyKeyMap()
        rows = {r['name']: r for r in self.controls.keyRows}
        self.assertEqual(('Q', True), (rows['Cross (X)']['keys'], rows['Cross (X)']['changed']))
        self.assertEqual('no key', rows['L1']['keys'])
        self.assertEqual('cross=Q,l1=none', self.saved_settings()['key_map'])
        self.assertEqual('cross=Q,l1=none', logic.tlg_run.build_env(self.backend.settings)['ISS_KEY_MAP'])

    def test_invalid_map_cannot_be_saved(self):
        self.controls.setDraft('cross=Q,foo=A')
        self.assertFalse(self.controls.draftValid)
        self.assertEqual('bad', self.controls.draftMessages[0]['tone'])
        self.controls.applyKeyMap()
        self.assertNotIn('key_map', self.backend.settings)
        self.controls.setDraft('cross=Banana')
        self.assertTrue(self.controls.draftValid)
        self.assertEqual(['good', 'warn'], [m['tone'] for m in self.controls.draftMessages])

    def test_clear_goes_back_to_the_default(self):
        self.controls.setDraft('cross=Q')
        self.controls.applyKeyMap()
        self.controls.clearKeyMap()
        self.assertNotIn('key_map', self.saved_settings())
        self.assertEqual('', self.controls.draftText)
        self.assertEqual({}, {k: v for k, v in logic.tlg_run.build_env(self.backend.settings).items() if k == 'ISS_KEY_MAP'})

    def test_portuguese_keyboard_labels(self):
        self.backend.settings['ui_language'] = 'pt_BR'
        self.backend.setLang('pt_BR')
        names = [r['name'] for r in self.controls.keyRows]
        self.assertIn('Bola (Circle)', names)
        self.assertIn('Analógico esquerdo', names)


if __name__ == '__main__':
    unittest.main()
