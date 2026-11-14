"""Support and Updates screens of the Qt launcher (launcher/qt/report.py, releases.py, support.py,
updates.py): path redaction, log summary, issue URL, version comparison, bilingual release notes,
known-issue matching, the one HTTPS request (stubbed: no test uses the network), controllers."""
import io
import json
import os
import re
import socket
import sys
import tempfile
import unittest
import urllib.error
import zipfile
from pathlib import Path
from unittest import mock
from urllib.parse import parse_qs, urlparse

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
from qt import logic, releases, report  # noqa: E402

try:
    from qt.backend import Backend
    HAVE_QT = True
except ImportError:
    HAVE_QT = False

STRINGS = json.loads((ROOT / 'launcher' / 'qt' / 'i18n' / 'strings.json').read_text(encoding='utf-8'))
T = logic.Translator({'ui_language': 'en_US'}, STRINGS, {})


class RedactionTests(unittest.TestCase):
    def test_windows_paths(self):
        text = r'Saves in C:\Users\maria\Documents\inFAMOUS-PC\user and d:/users/Maria/x and \\?\C:\Users\joao\AppData'
        out, n = report.redact(text)
        self.assertEqual(n, 3)
        self.assertIn(r'C:\Users\<user>\Documents\inFAMOUS-PC\user', out)
        self.assertIn('d:/users/<user>/x', out)
        self.assertIn(r'C:\Users\<user>\AppData', out)
        self.assertEqual(report.leftover_home_paths(out), [])

    def test_name_with_a_space_and_json_escaped_backslashes(self):
        out, _ = report.redact(r'path=C:\Users\Ana Silva\Desktop\iss and "C:\\Users\\bob\\Games\\x"')
        self.assertIn(r'C:\Users\<user>\Desktop\iss', out)
        self.assertIn(r'C:\\Users\\<user>\\Games', out)
        self.assertNotIn('Ana', out)
        self.assertNotIn('bob', out)
        self.assertEqual(report.leftover_home_paths(out), [])

    def test_a_sentence_after_a_path_is_not_swallowed(self):
        out, _ = report.redact(r'opened C:\Users\ana and then crashed')
        self.assertEqual(out, r'opened C:\Users\<user> and then crashed')

    def test_linux_and_other_roots(self):
        text = ('/home/maria/.local/share/infamous-pc/user\n/var/home/maria/x\n/Users/maria/Library\n'
                '/run/media/maria/DISCO/infamous\n/media/maria/usb\nhome: /home/joao.silva/y')
        out, n = report.redact(text)
        self.assertEqual(n, 6)
        for name in ('maria', 'joao'):
            self.assertNotIn(name, out)
        self.assertIn('/home/<user>/.local/share/infamous-pc/user', out)
        self.assertIn('/run/media/<user>/DISCO/infamous', out)
        self.assertEqual(report.leftover_home_paths(out), [])

    def test_the_rest_of_the_text_is_kept(self):
        text = 'Frame stats: 60.0 FPS\nGPU: NVIDIA GeForce RTX 2060\n/usr/lib/x86_64-linux-gnu/libvulkan.so\nC:\\Windows\\System32\\x.dll\n'
        self.assertEqual(report.redact(text), (text, 0))

    def test_public_and_default_profiles_are_not_personal(self):
        text = r'C:\Users\Public\Documents and C:\Users\Default\x'
        self.assertEqual(report.redact(text)[0], text)

    def test_known_names_are_replaced_anywhere(self):
        out, n = report.redact('hello maria, host=PC-DA-MARIA user maria_x Maria.', names={'maria'})
        self.assertEqual(out, 'hello <user>, host=PC-DA-<user> user maria_x <user>.')
        self.assertEqual(n, 3)
        self.assertEqual(report.leftover_home_paths(out, {'maria'}), [])
        # a generic name must not wreck the text
        self.assertEqual(report.redact('user/savedata', names={'user'})[0], 'user/savedata')

    def test_leftover_detector_sees_a_missed_path(self):
        self.assertEqual(report.leftover_home_paths('/home/ana/x'), ['home/ana'])
        self.assertEqual(report.leftover_home_paths('/home/<user>/x'), [])
        self.assertEqual(report.leftover_home_paths('nothing of an here', {'an'}), [])   # too short: ignored
        self.assertEqual(report.leftover_home_paths('hi maria', {'maria'}), ['maria'])

    def test_own_names_uses_this_machine(self):
        with mock.patch('getpass.getuser', return_value='zeca'), mock.patch.dict(os.environ, {'USERNAME': 'ZecaWin'}):
            names = report.own_names()
        self.assertIn('zeca', names)
        self.assertIn('ZecaWin', names)
        self.assertNotIn('root', report.own_names({'root'}))


LOG = '''Build: 3f2a9c1d8e41 (built Oct 13 2026 09:12:44); game CUSA00223 01.07; threads inherit
Config: window=Windowed vsync=1
  BP01=1 BP02=1
  user=/home/maria/inFAMOUS-PC/user
GPU: using NVIDIA GeForce RTX 2060
Frame stats: 60.0 FPS, worst frame 16.7 ms (vblank 60 Hz); 0 shader/pipeline compiles
Frame stats: 30.0 FPS, worst frame 950.0 ms (vblank 60 Hz); 4 compiles
Fault: access violation at 0x1234 (thread 7)
Fault: access violation at 0x1234 (thread 7)
FREEZE: no frame for 5 s; threads:
   thread 1 ...
STOP: watchdog timeout; thread stacks:
Fault profile (5 s): [W 0x1 x2]
'''


class LogSummaryTests(unittest.TestCase):
    def test_config_block_events_and_fps(self):
        s = report.summarize_log(LOG)
        self.assertTrue(s['build'].startswith('Build: 3f2a9c1d8e41'))
        self.assertEqual(s['config'], ['Config: window=Windowed vsync=1', 'BP01=1 BP02=1', 'user=/home/maria/inFAMOUS-PC/user'])
        self.assertAlmostEqual(s['fps_avg'], 45.0)
        self.assertEqual((s['fps_min'], s['fps_max'], s['worst_ms'], s['fps_samples']), (30.0, 60.0, 950.0, 2))
        kinds = [(k, c) for k, _, c in s['events']]
        self.assertEqual(kinds, [('Fault', 2), ('FREEZE', 1), ('STOP', 1)])   # repeated line counted once
        self.assertEqual(s['event_counts'], {'Fault': 2, 'FREEZE': 1, 'STOP': 1})   # "Fault profile" is not a Fault:

    def test_runtime_failure_explains_the_stop(self):
        # 2026-10-26 report: only the STOP reached the summary; the cause was the line above it
        s = report.summarize_log('Runtime: application heap API registered\n'
                                 'Runtime: cannot create 6080 MiB of guest memory (error 1450); is the page file large enough?\n'
                                 'STOP: guest raised debug exception code=0xa0020013 argument=0x0\n'
                                 'Runtime: guest threads created=0, exited=0, joined=0\n')
        self.assertEqual([k for k, _, _ in s['events']], ['Runtime', 'STOP'])
        short = report.short_report([], report.summary_lines(s, T), T)
        self.assertIn('Runtime: cannot create 6080 MiB of guest memory (error 1450)', short)

    def test_no_config_block_falls_back_to_the_header_lines(self):
        s = report.summarize_log('Settings: tlg.ini\nWindow: Windowed\nGPU 0: X (DiscreteGpu)\nnoise\n')
        self.assertEqual(s['config'], [])
        self.assertEqual(s['header'], ['Settings: tlg.ini', 'Window: Windowed', 'GPU 0: X (DiscreteGpu)'])
        self.assertIsNone(s['fps_avg'])
        lines = report.summary_lines(s, T)
        self.assertTrue(any('no "Frame stats"' in l for l in lines))
        self.assertTrue(any('No Fault:' in l for l in lines))

    def test_summary_lines_text(self):
        lines = report.summary_lines(report.summarize_log(LOG), T)
        self.assertIn('FPS: average 45.0 (lowest 30.0, highest 60.0; 2 samples)', lines)
        self.assertIn('Fault: access violation at 0x1234 (thread 7)  (x2)', lines)

    def test_list_and_read_logs(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            for n, name in enumerate(('run-1.log', 'run-2.log', 'other.txt')):
                (d / name).write_text(LOG)
                os.utime(d / name, (1000 + n, 1000 + n))
            self.assertEqual([p.name for p in report.list_logs(d)], ['run-2.log', 'run-1.log'])
            self.assertEqual(report.list_logs(d / 'missing'), [])
            big = d / 'run-3.log'
            big.write_bytes(b'Build: abcdef1234\n' + b'x' * 5000 + b'\nEND\n')
            text, cut = report.read_log(big, limit=1000)
            self.assertTrue(cut)
            self.assertTrue(text.startswith('Build:') and text.rstrip().endswith('END'))


class IssueUrlTests(unittest.TestCase):
    def query(self, url):
        parsed = urlparse(url)
        self.assertEqual(f'{parsed.scheme}://{parsed.netloc}{parsed.path}', 'https://github.com/HayatoG/iss_pc/issues/new')
        return {k: v[0] for k, v in parse_qs(parsed.query).items()}

    def test_short_body_goes_in_full(self):
        body = '### Hardware\n- **GPU:** RTX 2060 & friends\nacentuação ç\n'
        url, short = report.issue_url('Problem: RTX 2060', body, 'short')
        self.assertFalse(short)
        q = self.query(url)
        self.assertEqual(q['body'], body)
        self.assertEqual(q['title'], 'Problem: RTX 2060')
        self.assertNotIn('\n', url)

    def test_long_body_leaves_only_the_summary(self):
        long_body = 'x' * 6001
        url, short = report.issue_url('t', long_body, 'SUMMARY only')
        self.assertTrue(short)
        self.assertEqual(self.query(url)['body'], 'SUMMARY only')
        url, short = report.issue_url('t', 'y' * 6000, 's')
        self.assertFalse(short)

    def test_body_that_encodes_too_long_is_shortened_too(self):
        body = 'ç\n' * 2900          # 5800 characters, a lot more once encoded
        url, short = report.issue_url('t', body, 'resumo')
        self.assertTrue(short)
        self.assertLessEqual(len(url), report.URL_LIMIT)

    def test_a_huge_summary_is_cut_to_fit(self):
        url, short = report.issue_url('t', 'a' * 9000, 'ç' * 9000)
        self.assertTrue(short)
        self.assertLessEqual(len(url), report.URL_LIMIT)

    def test_report_texts_and_zip(self):
        info = {'version': 'v2026.10.15-61e8942', 'game': {'title_id': 'CUSA00223', 'app_ver': '01.07'}, 'os': 'Windows 11',
                'cpu': {'name': 'CPU X', 'features': {'SSE4A': False, 'AVX2': True, 'F16C': None}}, 'ram': 16 * 2 ** 30,
                'gpu': {'name': 'AMD Radeon RX 580 Series', 'driver': 'AMD 24.9', 'vulkan': '1.3', 'vendor': 'AMD',
                        'type': 'discrete_gpu', 'vram_gb': 8.0}}
        rows = report.hardware_rows(info, T)
        text = dict(rows)
        self.assertEqual(text['Game'], 'CUSA00223 01.07')
        self.assertEqual(text['CPU'], 'CPU X (SSE4A no, AVX2 yes, F16C ?)')
        self.assertEqual(text['RAM'], '16.0 GiB')
        self.assertEqual(text['GPU'], 'AMD Radeon RX 580 Series (8.0 GiB VRAM)')
        known = report.known_for(report.load_known_issues(), info['gpu'], 'windows')
        for k in known:
            k['title'] = k['title']['en']
        summary = report.summary_lines(report.summarize_log(LOG), T)
        md = report.markdown_report(rows, summary, 'run-1.log', known, 'It closes.', T)
        self.assertTrue(md.startswith('It closes.'))
        self.assertIn('### Hardware and version', md)
        self.assertIn('- **GPU:** AMD Radeon RX 580', md)
        self.assertIn('```\nBuild: 3f2a9c1d8e41', md)
        self.assertIn('#49, #65, #67', md)
        self.assertIn('#### ' if False else '### Known problems for this GPU', md)
        short = report.short_report(rows, summary, T)
        self.assertIn('attach the .zip', short)
        self.assertLess(len(short), len(md))
        with tempfile.TemporaryDirectory() as d:
            target = Path(d) / 'r.zip'
            report.write_zip(target, md, 'LOG TEXT', 'run-1.log')
            with zipfile.ZipFile(target) as z:
                self.assertEqual(sorted(z.namelist()), ['report.md', 'run-1.redacted.log'])
                self.assertEqual(z.read('run-1.redacted.log').decode(), 'LOG TEXT')


class KnownIssuesTests(unittest.TestCase):
    def setUp(self):
        self.issues = report.load_known_issues()

    def ids(self, gpu, os_key):
        return [i['id'] for i in report.known_for(self.issues, gpu, os_key) if i['matched']]

    def gpu(self, name, vendor, vram=8.0, type_='discrete_gpu'):
        return {'name': name, 'vendor': vendor, 'vram_gb': vram, 'type': type_, 'driver': 'x'}

    def test_the_four_starting_entries_exist_with_their_issue_numbers(self):
        by_id = {i['id']: i for i in self.issues}
        self.assertEqual(by_id['polaris-windows']['issues'], [49, 65, 67])
        self.assertEqual(by_id['vram-4gb']['issues'], [72, 75])
        self.assertEqual(by_id['maxwell-quadro']['issues'], [70])
        self.assertIn('ISS_BP09=1', by_id['polaris-windows']['what']['en'])
        for i in self.issues:
            self.assertTrue(i['title']['en'] and i['title']['pt'] and i['what']['en'] and i['what']['pt'])

    def test_polaris_only_on_windows(self):
        for name in ('AMD Radeon RX 580 Series', 'AMD Radeon(TM) RX 570', 'Radeon RX 480 Graphics', 'AMD Radeon RX 550'):
            self.assertIn('polaris-windows', self.ids(self.gpu(name, 'AMD'), 'windows'), name)
            self.assertNotIn('polaris-windows', self.ids(self.gpu(name, 'AMD'), 'linux'), name)
        for name in ('AMD Radeon RX 5500 XT', 'AMD Radeon RX 5700 XT', 'AMD Radeon RX 6600', 'AMD Radeon RX 7800 XT'):
            self.assertNotIn('polaris-windows', self.ids(self.gpu(name, 'AMD'), 'windows'), name)

    def test_ampere_on_windows(self):
        for name in ('NVIDIA GeForce RTX 3060', 'NVIDIA GeForce RTX 3080 Ti', 'NVIDIA GeForce RTX 3050 Laptop GPU'):
            self.assertIn('ampere-windows', self.ids(self.gpu(name, 'NVIDIA'), 'windows'), name)
        self.assertNotIn('ampere-windows', self.ids(self.gpu('NVIDIA GeForce RTX 3060', 'NVIDIA'), 'linux'))
        self.assertNotIn('ampere-windows', self.ids(self.gpu('NVIDIA GeForce RTX 2060', 'NVIDIA'), 'windows'))
        self.assertNotIn('ampere-windows', self.ids(self.gpu('NVIDIA GeForce RTX 4060', 'NVIDIA'), 'windows'))

    def test_four_gb_vram(self):
        self.assertIn('vram-4gb', self.ids(self.gpu('NVIDIA GeForce GTX 1650', 'NVIDIA', 4.0), 'windows'))
        self.assertIn('vram-4gb', self.ids(self.gpu('AMD Radeon RX 570', 'AMD', 3.9), 'linux'))
        self.assertNotIn('vram-4gb', self.ids(self.gpu('NVIDIA GeForce RTX 2060', 'NVIDIA', 6.0), 'linux'))
        self.assertNotIn('vram-4gb', self.ids(self.gpu('NVIDIA GeForce RTX 3060', 'NVIDIA', None), 'linux'))   # unknown
        self.assertNotIn('vram-4gb', self.ids(self.gpu('Intel iGPU', 'Intel', 4.0, 'integrated_gpu'), 'linux'))

    def test_maxwell_and_quadro(self):
        for name in ('NVIDIA GeForce GTX 960', 'NVIDIA GeForce GTX 970', 'NVIDIA Quadro M1000M', 'NVIDIA GeForce GTX 750 Ti'):
            self.assertIn('maxwell-quadro', self.ids(self.gpu(name, 'NVIDIA'), 'linux'), name)
        for name in ('NVIDIA GeForce GTX 1060 6GB', 'NVIDIA GeForce GTX 1650', 'NVIDIA GeForce RTX 2060'):
            self.assertNotIn('maxwell-quadro', self.ids(self.gpu(name, 'NVIDIA'), 'linux'), name)

    def test_matches_first_and_no_gpu_matches_nothing(self):
        rows = report.known_for(self.issues, self.gpu('AMD Radeon RX 570', 'AMD', 4.0), 'windows')
        flags = [r['matched'] for r in rows]
        self.assertEqual(flags, sorted(flags, reverse=True))
        self.assertTrue(flags[0])
        self.assertFalse(any(r['matched'] for r in report.known_for(self.issues, None, 'windows')))
        self.assertFalse(report.matches({'match': {}}, self.gpu('x', 'AMD')))

    def test_vulkaninfo_parsing_with_memory_heaps(self):
        summary = ('Devices:\n========\nGPU0:\n\tapiVersion         = 1.3.280\n\tdriverVersion      = 1.0\n\tvendorID           = 0x1002\n'
                   '\tdeviceType         = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU\n\tdeviceName         = AMD Radeon RX 580 Series\n'
                   '\tdriverName         = AMD proprietary driver\n\tdriverInfo         = 24.9.1\n'
                   'GPU1:\n\tdeviceType         = PHYSICAL_DEVICE_TYPE_CPU\n\tdeviceName         = llvmpipe\n')
        full = ('GPU0:\n\tdeviceName        = AMD Radeon RX 580 Series\nmemoryHeaps: count = 2\n\tmemoryHeaps[0]:\n'
                '\t\tsize   = 8589934592 (0x200000000) (8.00 GiB)\n\t\tflags: count = 1\n\t\t\tMEMORY_HEAP_DEVICE_LOCAL_BIT\n'
                '\tmemoryHeaps[1]:\n\t\tsize   = 17179869184 (16.00 GiB)\n\t\tflags: count = 0\n'
                'GPU1:\n\tdeviceName        = llvmpipe\nmemoryHeaps: count = 1\n\tmemoryHeaps[0]:\n\t\tsize   = 100 (x)\n')
        devices = report.parse_vulkan_devices(summary, full)
        self.assertEqual(devices[0]['deviceName'], 'AMD Radeon RX 580 Series')
        self.assertEqual(devices[0]['vram_gb'], 8.0)
        self.assertEqual(devices[0]['driverInfo'], '24.9.1')
        stub = mock.Mock(side_effect=[mock.Mock(stdout=summary), mock.Mock(stdout=full)])
        info = report.gpu_info(run=stub)
        self.assertEqual((info['name'], info['vendor'], info['driver'], info['vulkan'], info['vram_gb']),
                         ('AMD Radeon RX 580 Series', 'AMD', 'AMD proprietary driver 24.9.1', '1.3', 8.0))
        self.assertIsNone(report.gpu_info(run=mock.Mock(side_effect=FileNotFoundError)))

    def test_nvidia_vram_comes_from_nvidia_smi_without_the_long_vulkaninfo(self):
        summary = ('Devices:\n========\nGPU0:\n\tapiVersion         = 1.4.357\n\tvendorID           = 0x10de\n'
                   '\tdeviceType         = PHYSICAL_DEVICE_TYPE_DISCRETE_GPU\n\tdeviceName         = NVIDIA GeForce RTX 2060\n'
                   '\tdriverName         = NVIDIA\n\tdriverInfo         = 621.01\n')
        stub = mock.Mock(side_effect=[mock.Mock(stdout=summary), mock.Mock(stdout='6144\n')])
        info = report.gpu_info(run=stub)
        self.assertEqual((info['name'], info['vendor'], info['vram_gb']), ('NVIDIA GeForce RTX 2060', 'NVIDIA', 6.0))
        self.assertEqual(stub.call_count, 2)   # --summary and nvidia-smi; the plain vulkaninfo never starts

    def test_cpu_features_from_cpuinfo_and_probe(self):
        name, flags = report.parse_cpuinfo('model name\t: AMD Ryzen 5\nflags\t\t: fpu sse4a avx2 f16c movbe abm\n')
        self.assertEqual(name, 'AMD Ryzen 5')
        self.assertIn('sse4a', flags)
        with mock.patch.object(report.sys, 'platform', 'win32'), mock.patch.object(report.platform, 'processor', return_value='Intel64'):
            info = report.cpu_info(missing_probe=lambda: ['sse4a', 'f16c'])
        self.assertIs(info['features']['SSE4A'], False)
        self.assertIs(info['features']['F16C'], False)
        self.assertIs(info['features']['BMI1'], True)


class VersionTests(unittest.TestCase):
    def test_parse(self):
        self.assertEqual(releases.parse_version('v2026.10.15-61e8942'), ((2026, 10, 15), '61e8942'))
        self.assertEqual(releases.parse_version('v2026.10.15'), ((2026, 10, 15), ''))
        self.assertEqual(releases.parse_version('3f2a9c1d8e41+dirty'), (None, '3f2a9c1d8e41'))
        self.assertEqual(releases.parse_version('nightly'), 'bad')

    def test_compare(self):
        c = releases.compare
        self.assertEqual(c('v2026.10.13-3f2a9c1', 'v2026.10.15-61e8942'), 'newer')
        self.assertEqual(c('v2026.9.30-aaaaaaa', 'v2026.10.1-bbbbbbb'), 'newer')      # numeric, not text, order
        self.assertEqual(c('v2026.10.15-61e8942', 'v2026.10.15-61e8942'), 'same')
        self.assertEqual(c('v2026.10.15-61e8942', 'v2026.10.15-9999999'), 'newer')    # another build, same day
        self.assertEqual(c('v2026.10.16-1111111', 'v2026.10.15-61e8942'), 'older')
        self.assertEqual(c('v2026.10.15', 'v2026.10.15-61e8942'), 'same')
        self.assertEqual(c('61e8942abcde', 'v2026.10.15-61e8942'), 'same')            # only the log's commit is known
        self.assertEqual(c('3f2a9c1d8e41', 'v2026.10.15-61e8942'), 'different')
        self.assertEqual(c('', 'v2026.10.15-61e8942'), 'unknown')
        self.assertEqual(c('weird', 'v2026.10.15-61e8942'), 'unknown')
        self.assertTrue(releases.update_available('v2026.10.13-3f2a9c1', 'v2026.10.15-61e8942'))
        self.assertTrue(releases.update_available('3f2a9c1d8e41', 'v2026.10.15-61e8942'))
        self.assertFalse(releases.update_available('v2026.10.15-61e8942', 'v2026.10.15-61e8942'))
        self.assertFalse(releases.update_available('', 'v2026.10.15-61e8942'))


CI_BODY = '''Build automático do commit 61e8942. **Nenhum arquivo do jogo incluído**.

## Novidades
- Corrige luzes piscando (#72) (PR #80)
- Novo **launcher** com `Suporte` (PR #81)

Issues relatadas por: #72 (@ana). Obrigado!

## What's new
- Fixes flickering lights (#72) (PR #80)
- New launcher (PR #81)

Reported by: #72 (@ana). Thank you!

## Arquivos necessários
- eboot.bin
'''

PR_BODY = '''Intro
<!-- notes:pt-BR -->
- Corrige X (#18)
<!-- /notes -->
<!-- notes:en-US -->
- Fixes X (#18)
- Adds Y
<!-- /notes -->
'''


class NotesTests(unittest.TestCase):
    def test_ci_release_body_sections(self):
        pt = releases.extract_notes(CI_BODY, True)
        en = releases.extract_notes(CI_BODY, False)
        self.assertIn('• Corrige luzes piscando (#72) (PR #80)', pt)
        self.assertIn('Novo launcher com Suporte', pt)
        self.assertIn('Obrigado!', pt)
        self.assertNotIn('Fixes flickering', pt)
        self.assertNotIn('eboot.bin', pt)
        self.assertIn('• Fixes flickering lights', en)
        self.assertIn('Thank you!', en)
        self.assertNotIn('Corrige', en)
        self.assertNotIn('Arquivos', en)

    def test_marker_blocks(self):
        self.assertEqual(releases.extract_notes(PR_BODY, True), '• Corrige X (#18)')
        self.assertEqual(releases.extract_notes(PR_BODY, False), '• Fixes X (#18)\n• Adds Y')

    def test_crlf_and_fallbacks(self):
        self.assertIn('Fixes flickering', releases.extract_notes(CI_BODY.replace('\n', '\r\n'), False))
        self.assertEqual(releases.extract_notes('Just text <!-- hidden -->', True), 'Just text')
        self.assertEqual(releases.extract_notes('', True), '')
        self.assertEqual(releases.extract_notes('## What\'s new\n- only english\n', True), '')   # no pt section: nothing wrong shown


class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class FetchTests(unittest.TestCase):
    def opener(self, payload=None, error=None):
        calls = []

        def open_(request, timeout=None):
            calls.append((request, timeout))
            if error:
                raise error
            return FakeResponse(payload if isinstance(payload, bytes) else json.dumps(payload).encode())
        open_.calls = calls
        return open_

    def test_one_get_with_user_agent_and_timeout_and_no_data(self):
        release = {'tag_name': 'v2026.10.15-61e8942', 'name': 'v2026.10.15', 'body': CI_BODY, 'published_at': '2026-10-15T10:00:00Z',
                   'html_url': 'https://github.com/HayatoG/iss_pc/releases/tag/v2026.10.15-61e8942'}
        opener = self.opener(release)
        got = releases.fetch_latest('v2026.10.13-3f2a9c1', opener=opener)
        self.assertEqual(len(opener.calls), 1)
        request, timeout = opener.calls[0]
        self.assertEqual(request.full_url, 'https://api.github.com/repos/HayatoG/iss_pc/releases/latest')
        self.assertEqual(request.get_method(), 'GET')
        self.assertIsNone(request.data)
        self.assertEqual(timeout, 10)
        self.assertIn('iss_pc-launcher/v2026.10.13-3f2a9c1', request.get_header('User-agent'))
        self.assertEqual(set(k.lower() for k in request.headers), {'user-agent', 'accept'})   # nothing about the player
        self.assertEqual((got['tag'], got['published'], got['url']), ('v2026.10.15-61e8942', '2026-10-15', release['html_url']))

    def test_a_foreign_url_is_replaced_by_the_releases_page(self):
        got = releases.fetch_latest('', opener=self.opener({'tag_name': 'v1', 'html_url': 'https://evil.example/x'}))
        self.assertEqual(got['url'], releases.RELEASES_URL)

    def error_kind(self, **kw):
        with self.assertRaises(releases.UpdateError) as ctx:
            releases.fetch_latest('', opener=self.opener(**kw))
        return ctx.exception.kind

    def test_friendly_error_kinds(self):
        self.assertEqual(self.error_kind(error=urllib.error.URLError(socket.gaierror('no dns'))), 'offline')
        self.assertEqual(self.error_kind(error=urllib.error.URLError(socket.timeout())), 'timeout')
        self.assertEqual(self.error_kind(error=socket.timeout()), 'timeout')
        self.assertEqual(self.error_kind(error=ConnectionResetError()), 'offline')
        # a 403 is the rate limit only when GitHub says so (X-RateLimit-Remaining 0 or Retry-After)
        limited = {'X-RateLimit-Remaining': '0', 'X-RateLimit-Reset': '1792559606'}
        for code, headers, kind in ((404, {}, 'none'), (403, limited, 'limit'), (403, {'Retry-After': '60'}, 'limit'),
                                    (403, {}, 'http'), (429, {}, 'limit'), (500, {}, 'http')):
            self.assertEqual(self.error_kind(error=urllib.error.HTTPError('u', code, 'x', headers, None)), kind)
        self.assertEqual(self.error_kind(payload=b'<html>not json</html>'), 'bad')
        self.assertEqual(self.error_kind(payload={'no': 'tag'}), 'bad')

    def test_every_error_kind_has_a_text(self):
        for kind in ('offline', 'timeout', 'limit', 'http', 'bad', 'none'):
            self.assertIn('upd_err_' + kind, STRINGS)

    def test_the_module_opens_no_socket_by_itself(self):
        text = (ROOT / 'launcher' / 'qt' / 'report.py').read_text()
        self.assertNotRegex(text, r'urllib\.request|import requests|socket\.socket|http\.client')


class StringsTests(unittest.TestCase):
    def test_python_side_keys_exist_in_both_languages(self):
        keys = set()
        for name in ('report.py', 'support.py', 'updates.py'):
            keys |= set(re.findall(r"""\bt\(\s*'((?:rep|sup|upd)_\w+)'""", (ROOT / 'launcher' / 'qt' / name).read_text()))
        keys = {k for k in keys if not k.endswith('_')}   # dynamic ones are listed below
        keys |= {f'upd_{s}' for s in ('newer', 'different', 'same', 'older', 'unknown')}
        self.assertGreater(len(keys), 25)
        for key in keys:
            self.assertIn(key, STRINGS, key)
            self.assertTrue(STRINGS[key]['en'] and STRINGS[key]['pt'], key)


@unittest.skipUnless(HAVE_QT, 'PySide6 not installed')
class ControllerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for key, value in (('XDG_CONFIG_HOME', str(self.root / 'cfg')), ('ISS_USER_DIR', str(self.root / 'user'))):
            patcher = mock.patch.dict(os.environ, {key: value})
            patcher.start()
            self.addCleanup(patcher.stop)
        self.logs = self.root / 'logs'
        self.logs.mkdir()
        (self.logs / 'run-1.log').write_text(LOG + 'C:\\Users\\Maria Silva\\x\n', encoding='utf-8')
        self.opened, self.copied, self.saved = [], [], []
        self.release = {'tag': 'v2026.10.15-61e8942', 'url': 'https://github.com/HayatoG/iss_pc/releases/tag/v2026.10.15-61e8942',
                        'body': CI_BODY, 'published': '2026-10-15'}
        self.fetch_calls = []

        def fetch(version):
            self.fetch_calls.append(version)
            if isinstance(self.release, Exception):
                raise self.release
            return self.release
        info = {'version': 'v2026.10.13-3f2a9c1', 'game': {'title_id': 'CUSA00223', 'app_ver': '01.07'}, 'os': 'Windows 11',
                'cpu': {'name': 'CPU', 'features': {'SSE4A': True, 'AVX2': True, 'F16C': True}}, 'ram': 2 ** 34,
                'gpu': {'name': 'AMD Radeon RX 580 Series', 'driver': 'AMD 24.9', 'vulkan': '1.3', 'vendor': 'AMD',
                        'type': 'discrete_gpu', 'vram_gb': 8.0}}
        probes = {'gpu': lambda: ('GPU', '1.3'), 'pad': lambda: None, 'problems': lambda: [], 'cache': lambda: 0,
                  'save': lambda: None, 'news': lambda: [], 'version': lambda: 'v1', 'save_fixes': lambda s, st: None,
                  'saves': {'running': lambda: False, 'scan': lambda: [], 'open': lambda p: None},
                  'support': {'info': lambda: info, 'logs_dir': lambda: self.logs, 'names': lambda: {'maria'},
                              'os_key': lambda: 'windows', 'clipboard': self.copied.append, 'open_url': self.opened.append},
                  'updates': {'version': lambda: self.installed, 'fetch': fetch, 'open_url': self.opened.append,
                              'save': lambda s: self.saved.append(json.loads(json.dumps(s)))}}
        self.installed = 'v2026.10.13-3f2a9c1'
        settings = dict(logic.tlg_run.DEFAULT_SETTINGS, game_dir='/g', ui_language='en_US')
        self.backend = Backend(settings=settings, probes=probes)
        self.support, self.updates = self.backend.support, self.backend.updates
        from PySide6.QtCore import QCoreApplication
        self.app = QCoreApplication.instance() or QCoreApplication([])

    def pump(self, until, limit=3000):
        import time
        end = time.time() + limit / 1000
        while not until() and time.time() < end:
            self.app.processEvents()
            time.sleep(0.01)
        self.assertTrue(until(), 'timed out')

    def load_support(self):
        self.support.collect()
        self.pump(lambda: self.support.hwReady)
        self.support.selectLog(0)
        self.pump(lambda: self.support._log is not None)

    def test_support_flow_redacts_everything_that_leaves(self):
        self.load_support()
        self.assertEqual(self.support.matchedCount, 1)            # RX 580 on Windows
        self.assertTrue(self.support.known[0]['matched'])
        self.assertIn('<user>', self.support.summaryText)
        self.assertNotIn('maria', self.support.summaryText.lower())
        self.assertIn('2 replacements', self.support.redactionNote.replace('replacements of', 'replacements of'))
        self.support.setDescription('crash in C:\\Users\\Maria Silva\\Desktop')
        text = self.support.reportText
        self.assertIn('### Hardware and version', text)
        self.assertNotIn('Maria', text)
        self.support.copyReport()
        self.assertEqual(self.copied, [text])
        target = self.root / 'out'
        self.support.saveZip(str(target))
        with zipfile.ZipFile(str(target) + '.zip') as z:
            blob = z.read('report.md').decode() + z.read('run-1.redacted.log').decode()
        self.assertNotIn('Maria', blob)
        self.assertNotIn('/home/maria', blob)
        self.assertEqual(report.leftover_home_paths(blob, {'maria'}), [])
        self.assertIn('C:\\Users\\<user>\\x', blob)
        self.assertEqual(self.opened, [])                          # nothing opened without a click

    def test_zip_is_refused_when_redaction_missed_something(self):
        self.load_support()
        with mock.patch.object(report, 'redact', side_effect=lambda t, n=(): (t + '\n/home/leaky/x', 0)):
            self.support.saveZip(str(self.root / 'bad.zip'))
        self.assertFalse((self.root / 'bad.zip').exists())
        self.assertEqual(self.support.tone, 'bad')

    def test_open_issue_uses_the_browser_stub_and_the_short_summary_when_long(self):
        self.load_support()
        self.support.openIssue()
        url = self.opened[-1]
        self.assertTrue(url.startswith('https://github.com/HayatoG/iss_pc/issues/new?title='))
        body = parse_qs(urlparse(url).query)['body'][0]
        self.assertIn('AMD Radeon RX 580', body)
        self.assertNotIn('Maria', url)
        self.support.setDescription('x' * 7000)
        self.assertTrue(self.support.reportIsLong)
        self.support.openIssue()
        self.assertEqual(self.support.tone, 'warn')
        self.assertIn('attach it', self.support.message.lower() if False else self.support.message.replace('Save the .zip and attach it', 'attach it'))
        self.assertLessEqual(len(self.opened[-1]), report.URL_LIMIT)

    def test_links_and_no_logs(self):
        for name, url in report.URLS.items():
            self.support.openLink(name)
            self.assertEqual(self.opened[-1], url)
        self.support.openLink('evil')
        self.assertEqual(len(self.opened), 3)
        (self.logs / 'run-1.log').unlink()
        self.support.refreshLogs()
        self.assertFalse(self.support.hasLogs)
        self.assertIn('No logs yet', self.support.summaryText)

    def test_updates_check_is_manual_and_the_option_is_off_by_default(self):
        self.assertFalse(self.updates.checkOnOpen)
        self.assertFalse(self.updates.available)
        self.assertEqual(self.fetch_calls, [])
        self.backend.start_probes = None   # (not called: the real one starts hardware probes)
        self.updates.check()
        self.pump(lambda: not self.updates.checking)
        self.assertEqual(self.fetch_calls, ['v2026.10.13-3f2a9c1'])
        self.assertTrue(self.updates.available)
        self.assertIn('new version is available', self.updates.statusText)
        self.assertIn('Fixes flickering', self.updates.notes)
        self.assertNotIn('Corrige', self.updates.notes)
        self.backend.setLang('pt_BR')
        self.assertIn('Corrige luzes piscando', self.updates.notes)
        self.assertEqual(self.saved[-1]['update_seen']['tag'], 'v2026.10.15-61e8942')

    def test_known_new_version_survives_a_restart_until_it_is_installed(self):
        self.updates.check()
        self.pump(lambda: not self.updates.checking)
        from qt.updates import Updates
        again = Updates(self.backend, self.backend.probes.get('updates'))
        self.assertTrue(again.available)
        self.installed = 'v2026.10.15-61e8942'
        self.assertFalse(again.available)

    def test_dot_stays_off_when_up_to_date_or_after_an_error(self):
        self.installed = 'v2026.10.15-61e8942'
        self.updates.check()
        self.pump(lambda: not self.updates.checking)
        self.assertFalse(self.updates.available)
        self.assertEqual(self.updates.tone, 'good')
        self.release = releases.UpdateError('offline')
        self.updates.check()
        self.pump(lambda: not self.updates.checking)
        self.assertIn('Could not reach GitHub', self.updates.statusText)
        self.assertEqual(self.updates.tone, 'bad')
        self.assertFalse(self.updates.available)

    def test_download_opens_only_the_release_page(self):
        self.updates.download()
        self.assertEqual(self.opened[-1], releases.RELEASES_URL)
        self.updates.check()
        self.pump(lambda: not self.updates.checking)
        self.updates.download()
        self.assertEqual(self.opened[-1], self.release['url'])

    def test_check_on_open_setting_is_saved_and_start_probes_honours_it(self):
        self.updates.setCheckOnOpen(True)
        self.assertTrue(self.backend.settings['check_updates_on_open'])
        self.assertTrue(self.saved[-1]['check_updates_on_open'])
        with mock.patch.object(self.updates, 'check') as check, mock.patch.object(self.backend.saves, 'scan'), \
                mock.patch.object(self.backend.saves, 'start_timer'), mock.patch.object(self.support, 'collect'):
            self.backend.start_probes()
            check.assert_called_once()
            self.updates.setCheckOnOpen(False)
            check.reset_mock()
            self.backend.start_probes()
            check.assert_not_called()


if __name__ == '__main__':
    unittest.main()
