"""The automatic graphics setup (launcher/hardware.py) and how the launcher applies it."""
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / 'launcher'))
import hardware  # noqa: E402

GB = 2 ** 30


def hw(gpu, ram_gb, platform='win32', vram_gb=8):
    return hardware.detect(platform=platform, adapters=[(gpu, vram_gb * GB)], ram=ram_gb * GB)


class DecideTests(unittest.TestCase):
    def test_native_passes_only_on_nvidia_rtx_windows_with_more_than_16_gb(self):
        self.assertTrue(hardware.decide(hw('NVIDIA GeForce RTX 3060', 32))['native_passes'])
        self.assertTrue(hardware.decide(hw('NVIDIA GeForce RTX 5070 Laptop GPU', 24))['native_passes'])
        self.assertFalse(hardware.decide(hw('NVIDIA GeForce RTX 3060', 16))['native_passes'])   # RAM
        self.assertFalse(hardware.decide(hw('NVIDIA GeForce RTX 3060', 32, 'linux'))['native_passes'])
        self.assertFalse(hardware.decide(hw('NVIDIA GeForce GTX 1660 SUPER', 32))['native_passes'])
        self.assertFalse(hardware.decide(hw('AMD Radeon RX 7800 XT', 32))['native_passes'])
        self.assertFalse(hardware.decide(hw('Intel(R) Arc(TM) B580 Graphics', 32))['native_passes'])
        self.assertFalse(hardware.decide(hardware.detect(platform='win32', adapters=[], ram=None))['native_passes'])

    def test_profile_says_what_and_why(self):
        profile = hardware.decide(hw('NVIDIA GeForce RTX 3060', 16))['profile']
        self.assertIn('native_passes=0', profile)
        self.assertIn('16 GB RAM or less', profile)
        self.assertIn('RTX 3060', profile)


class DetectTests(unittest.TestCase):
    def test_discrete_card_wins_over_the_integrated_one(self):
        found = hardware.detect(platform='win32', ram=16 * GB, adapters=[
            ('AMD Radeon(TM) Graphics', 512 * 2 ** 20), ('NVIDIA GeForce RTX 4060', 8 * GB)])
        self.assertEqual(('NVIDIA', 'NVIDIA GeForce RTX 4060', 8.0, 16.0),
                         (found['vendor'], found['gpu'], found['vram_gb'], found['ram_gb']))

    def test_vendor_from_names(self):
        for name, vendor in (('Radeon RX 580 Series', 'AMD'), ('AMD Radeon RX 6750 XT', 'AMD'),
                             ('NVIDIA GeForce GTX 1060 6GB', 'NVIDIA'), ('Intel(R) UHD Graphics 630', 'Intel'),
                             ('Microsoft Basic Display Adapter', '')):
            self.assertEqual(vendor, hardware.vendor_of(name), name)

    def test_windows_registry_without_vulkaninfo(self):
        class Reg:
            HKEY_LOCAL_MACHINE = 'HKLM'
            values = {'0000': {'DriverDesc': 'NVIDIA GeForce RTX 3070', 'HardwareInformation.qwMemorySize': 8 * GB},
                      '0001': {'DriverDesc': 'AMD Radeon(TM) Graphics', 'HardwareInformation.MemorySize':
                               (512 * 2 ** 20).to_bytes(4, 'little')},
                      'Configuration': {}}

            def OpenKey(self, root, sub):
                return sub

            def EnumKey(self, root, index):
                keys = list(self.values)
                if index >= len(keys):
                    raise OSError
                return keys[index]

            def QueryValueEx(self, key, name):
                if name not in self.values[key]:
                    raise OSError
                return self.values[key][name], 0
        adapters = hardware._windows_adapters(Reg())
        self.assertEqual([('NVIDIA GeForce RTX 3070', 8 * GB), ('AMD Radeon(TM) Graphics', 512 * 2 ** 20)], adapters)

    def test_linux_proc_and_sysfs(self):
        with tempfile.TemporaryDirectory() as root:
            root = Path(root)
            info = root / 'proc/driver/nvidia/gpus/0000:01:00.0/information'
            info.parent.mkdir(parents=True)
            info.write_text('Model: \t\t NVIDIA GeForce RTX 2060\nIRQ: 1\n')
            for card, vendor in (('card0', '0x10de'), ('card1', '0x1002')):
                device = root / 'sys/class/drm' / card / 'device'
                device.mkdir(parents=True)
                (device / 'vendor').write_text(vendor + '\n')
            (root / 'sys/class/drm/card1/device/mem_info_vram_total').write_text(str(4 * GB))
            self.assertEqual([('NVIDIA GeForce RTX 2060', None), ('AMD', 4 * GB)], hardware._linux_adapters(root))

    def test_advanced_mode(self):
        self.assertFalse(hardware.advanced_mode({}, {}))
        self.assertTrue(hardware.advanced_mode({'advanced_mode': True}, {}))
        self.assertTrue(hardware.advanced_mode({}, {'ISS_ADVANCED': '1'}))
        self.assertFalse(hardware.advanced_mode({'advanced_mode': True}, {'ISS_ADVANCED': '0'}))


class LauncherAppliesTheAutomaticSetupTests(unittest.TestCase):
    def setUp(self):
        import infamous_run as run
        self.run = run
        self.folder = tempfile.TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        self.env = mock.patch.dict(os.environ, {'ISS_USER_DIR': self.folder.name})
        self.env.start()
        self.addCleanup(self.env.stop)
        for name in ('ISS_ADVANCED', 'ISS_NATIVE_PASSES', 'TLG_ADVANCED', 'ISS_BP01'):
            os.environ.pop(name, None)
        self.ini = Path(self.folder.name) / 'tlg.ini'
        self.ini.write_text('bp01=0\nfsr1=1\nsensor_aim=150\noverlay_fps=0\nmouse_sensitivity=2.00\n'
                            'display_mode=fullscreen\nadv_barrier_merge=1\n', encoding='utf-8')

    def machine(self, gpu, ram_gb):
        return mock.patch.object(self.run, '_HARDWARE', hw(gpu, ram_gb))

    def states(self, settings):
        return {k: v for k, v in self.run.experimental_states(settings).items() if v is not None}

    def test_stored_graphics_choices_are_ignored_but_overlay_and_controls_stay(self):
        with self.machine('NVIDIA GeForce RTX 3060', 32):
            states = self.states({})
        self.assertEqual('150', states.get('menu:sensor_aim'))
        self.assertEqual('0', states.get('menu:overlay_fps'))
        for gone in ('BP01', 'menu:fsr1', 'TLG_BARRIER_MERGE'):
            self.assertNotIn(gone, states)
        renderer = self.run._item('renderer')
        self.assertEqual('2', self.run.backports_table.effective(renderer, self.run.experimental_states({})))

    def test_native_passes_follow_the_hardware(self):
        native = self.run._item('native_passes')
        with self.machine('NVIDIA GeForce RTX 3060', 32):
            self.assertEqual('1', self.run.backports_table.effective(native, self.run.experimental_states({})))
        with self.machine('AMD Radeon RX 580 Series', 32):
            self.assertEqual('0', self.run.backports_table.effective(native, self.run.experimental_states({})))

    def test_env_carries_the_profile_and_no_player_switch(self):
        with self.machine('AMD Radeon RX 580 Series', 16):
            env = self.run.build_env(dict(self.run.DEFAULT_SETTINGS, write_merge=False, gpu_readbacks='0'))
        self.assertIn('vendor=AMD', env['ISS_AUTO_PROFILE'])
        self.assertNotIn('ISS_BP01', env)
        self.assertNotIn('TLG_ADVANCED', env)
        self.assertEqual('1', env['ISS_WRITE_MERGE'])   # settings.json graphics keys: their defaults
        self.assertEqual('2', env['ISS_READBACKS'])

    def test_advanced_mode_keeps_the_players_choices(self):
        with self.machine('NVIDIA GeForce RTX 3060', 32):
            env = self.run.build_env(dict(self.run.DEFAULT_SETTINGS, advanced_mode=True))
        self.assertEqual('0', env['ISS_BP01'])
        self.assertEqual('1', env['TLG_ADVANCED'])
        self.assertNotIn('ISS_AUTO_PROFILE', env)

    def test_reset_keeps_a_copy_and_only_drops_graphics_keys(self):
        settings = {}
        with mock.patch.object(self.run, 'save_settings'):
            self.assertTrue(self.run.reset_to_auto(settings))
            self.assertFalse(self.run.reset_to_auto(settings))   # once
        text = self.ini.read_text(encoding='utf-8')
        for kept in ('sensor_aim=150', 'overlay_fps=0', 'mouse_sensitivity=2.00', 'display_mode=fullscreen'):
            self.assertIn(kept, text)
        for gone in ('bp01=', 'fsr1=', 'adv_barrier_merge='):
            self.assertNotIn(gone, text)
        self.assertIn('bp01=0', (Path(self.folder.name) / 'tlg.ini.before-auto').read_text(encoding='utf-8'))

    @unittest.skipUnless(os.name == 'nt', 'the AMD workaround is for the Windows driver')
    def test_amd_on_this_ports_renderer_gets_barrier_merging_in_advanced_mode(self):
        self.ini.write_text('renderer=1\n', encoding='utf-8')
        with self.machine('Radeon RX 580 Series', 16), mock.patch.dict(os.environ, {'ISS_ADVANCED': '1'}):
            states = self.run.experimental_states({})
        self.assertEqual('1', states.get('TLG_BARRIER_MERGE'))


if __name__ == '__main__':
    unittest.main()
