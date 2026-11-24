# SPDX-License-Identifier: GPL-2.0-or-later
"""launcher/perf_report.py: what held the frame rate back, read from the game's own log
("Your last session" on the Graphics tab)."""
import sys
import tempfile
import unittest
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'launcher'))
import perf_report  # noqa: E402


def window(count, gpu_ms=None, busy=None, p99=16, over50=0, lod='1.00'):
    text = f'PRESENT_ACQUIRE fence=1 count={count} call_ms=0.007 fence_avg_ms=0.003\n'
    if gpu_ms is not None:
        text += f'GPU_FRAME_MS avg={gpu_ms:.2f} busy_pct={busy:.1f} frames={count}\n'
    text += (f'FRAME_PACE p99_ms={p99} over33={over50 * 2} over50={over50} frames={count}\n'
             f'SESSION_SETTINGS lod_scale={lod} quality_floor=1 fps_limit=0 render_resolution=0\n')
    return text


class PerfReportTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)

    def log(self, windows, limit=0, vram=None, head='GPU: renderer v2 (gpu_v2)\n', name='run-1.log'):
        text = head + f'VIDEOOUT fps_limit={limit} (menu)\n'
        for w in windows:
            text += window(*w)
            if vram:
                text += f'VRAM_BREAKDOWN usage={vram[0]} MB budget_now={vram[1]} MB images=1 MB\n'
        path = Path(self.dir.name) / name
        path.write_text(text, encoding='utf-8')
        return path

    def test_processor_limit_measured(self):
        report = perf_report.read(self.log([(1155, 5.3, 41)] * 8))
        self.assertEqual(77.0, report['fps'])
        self.assertEqual(('cpu', {'fps': 77, 'busy': 41, 'sure': True}), perf_report.verdict(report))
        self.assertEqual([], perf_report.notes(report))

    def test_graphics_card_limit_with_full_memory_and_freezes(self):
        report = perf_report.read(self.log([(720, 19.8, 95, 70, 2)] * 8, vram=(10800, 11316)))
        self.assertEqual('gpu', perf_report.verdict(report)[0])
        keys = [key for key, _ in perf_report.notes(report)]
        self.assertEqual(['vram', 'hitches_vram'], keys)

    def test_without_the_card_time_the_wait_decides(self):
        report = perf_report.read(self.log([(1155,)] * 8))
        self.assertIsNone(report['gpu_ms'])
        self.assertEqual(('cpu', {'fps': 77}), perf_report.verdict(report))   # no 'sure': "probably"

    def test_frame_rate_limit_and_the_engine_ceiling(self):
        self.assertEqual('limit', perf_report.verdict(perf_report.read(self.log([(900, 5, 30)] * 8, limit=60)))[0])
        self.assertEqual('ceiling', perf_report.verdict(perf_report.read(self.log([(1790, 5, 50)] * 8)))[0])

    def test_short_classic_and_missing(self):
        self.assertEqual('short', perf_report.verdict(perf_report.read(self.log([(900, 5, 30)] * 3)))[0])
        classic = self.log([(900,)] * 8, head='GPU: classic renderer\n')
        self.assertEqual('classic', perf_report.verdict(perf_report.read(classic))[0])
        self.assertIsNone(perf_report.read(Path(self.dir.name) / 'nope.log'))
        self.assertEqual(('none', {}), perf_report.verdict(None))

    def test_settings_changed_during_the_session(self):
        report = perf_report.read(self.log([(1155, 5.3, 41)] * 4 + [(1155, 5.3, 41, 16, 0, '2.00')] * 4))
        self.assertTrue(report['settings_changed'])
        self.assertIn(('mixed', {}), perf_report.notes(report))

    def test_summary_and_latest_log(self):
        self.log([(1155, 5.3, 41)] * 8, name='run-20261124-010000.log')
        newest = self.log([(1155, 5.3, 41)] * 8, name='run-20261124-020000.log')
        self.assertEqual(newest, perf_report.latest_log(self.dir.name))
        self.assertEqual({'fps': 77, 'p99_ms': 16, 'busy_pct': 41, 'hitches_per_min': 0.0},
                         perf_report.summary(perf_report.read(newest)))


if __name__ == '__main__':
    unittest.main()
