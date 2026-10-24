# SPDX-License-Identifier: GPL-2.0-or-later
"""scripts/round_analyze.py: the bench JSON / 'Bench:' line next to the old summary.json format."""
import argparse
import contextlib
import io
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import round_analyze as ra


def bench(label, avg, low1=50.0, p99=20.0, over33=0, compiles=0):
    return dict(frames=5000, seconds=90.0, avg_fps=avg, low1_fps=low1, p99_ms=p99, worst_ms=40.0,
                over33=over33, over50=0, compiles=compiles, label=label)


class Bench(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, rel, data):
        path = self.root / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(data) if not isinstance(data, str) else data, encoding="utf-8")
        return str(path)

    def run_cmd(self, fn, **kw):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            fn(argparse.Namespace(**kw))
        return out.getvalue()

    def test_bench_line_with_label_spaces(self):
        text = ("noise\nBench: frames=5400 seconds=90.02 avg_fps=60.0 low1_fps=41.2 p99_ms=24.3 "
                "worst_ms=88.0 over33=12 over50=3 compiles=0 label=B tuned run\n")
        d = ra.parse_bench_line(text)
        self.assertEqual(d["frames"], 5400)
        self.assertEqual(d["over50"], 3)
        self.assertAlmostEqual(d["p99_ms"], 24.3)
        self.assertEqual(d["label"], "B tuned run")
        self.assertIsNone(ra.parse_bench_line("Frame stats: nothing here\n"))
        self.assertIsNone(ra.parse_bench_line("Bench: window started (first 1000 ms skipped)\n"))
        self.assertEqual(ra.parse_bench_line("Bench: frames=1 seconds=1 avg_fps=1 label=-")["label"], "")

    def test_load_bench_formats(self):
        j = ra.load_bench(self.write("01-A/bench.json", bench("A", 60.0)))
        self.assertEqual((j["name"], j["source"]), ("A", "bench_json"))
        s = ra.load_bench(self.write("ctl/summary.json", dict(name="ctl", fps_mean=55.0, fps_1pct_low=40.0,
                                                             pacing_p99_mean_ms=25.0, shader_compiles=2)))
        self.assertEqual((s["name"], s["avg_fps"], s["low1_fps"], s["compiles"]), ("ctl", 55.0, 40.0, 2))
        log = ra.load_bench(self.write("03-B/game.log", "x\nBench: frames=10 seconds=1.00 avg_fps=10.0 "
                                       "low1_fps=9.0 p99_ms=110.0 worst_ms=120.0 over33=10 over50=10 "
                                       "compiles=0 label=\n"))
        self.assertEqual((log["name"], log["source"]), ("B", "log_bench"))   # folder without 'NN-'
        csv = "frame,present_ms,draws,shader_compiles\n" + "".join(
            f"{i},{ms},100,{c}\n" for i, (ms, c) in enumerate([(500, 5), (500, 5)] + [(10, 0)] * 198 + [(60, 1)], 1))
        c = ra.load_bench(self.write("04-C/frames.csv", csv))
        self.assertEqual(c["frames"], 199)            # the first 1000 ms skipped
        self.assertEqual((c["over33"], c["over50"], c["compiles"]), (1, 1, 1))
        self.assertEqual(c["worst_ms"], 60.0)
        self.assertEqual((c["p99_ms"], c["low1_fps"]), (10.0, 100.0))   # sorted[min(198, 197)]
        frames = ra.bench_from_frames([10.0] * 98 + [40.0, 50.0], skip_ms=0)
        self.assertEqual((frames["p99_ms"], frames["low1_fps"], frames["over33"]), (50.0, 20.0, 2))
        with self.assertRaises(ValueError):
            ra.load_bench(self.write("bad.json", {"something": 1}))

    def test_table_noise_and_diff(self):
        runs = [dict(bench("A", v), name="A", path="a") for v in (60.0, 61.0, 59.0)]
        runs += [dict(bench("B", v, over33=2), name="B", path="b") for v in (60.5, 61.5, 60.0)]
        runs += [dict(bench("C", v), name="C", path="c") for v in (70.0, 71.0, 70.5)]
        t = ra.bench_table(runs)
        self.assertEqual(t["groups"], ["A", "B", "C"])
        fps = next(r for r in t["rows"] if r["metric"] == "avg_fps")
        self.assertTrue(fps["groups"]["B"]["noise"])
        self.assertFalse(fps["groups"]["C"]["noise"])
        self.assertTrue(fps["groups"]["C"]["better"])
        over = next(r for r in t["rows"] if r["metric"] == "over33")
        self.assertEqual(over["groups"]["B"]["diff_abs"], 2)    # control 0: no percentage
        self.assertEqual(ra.bench_table(runs, control="C")["groups"][0], "C")
        with self.assertRaises(ValueError):
            ra.bench_table(runs, control="Z")

    def test_no_noise_mark_without_control_stdev(self):
        # identical control runs: stdev 0 is no noise estimate, so a 0% diff gets no '~'
        runs = [dict(bench("A", 60.0), name="A", path="a") for _ in range(3)]
        runs += [dict(bench("B", 60.0), name="B", path="b") for _ in range(3)]
        t = ra.bench_table(runs)
        fps = next(r for r in t["rows"] if r["metric"] == "avg_fps")
        self.assertEqual(fps["noise_pct"], 0.0)
        cell = fps["groups"]["B"]
        self.assertEqual(cell["diff_pct"], 0.0)
        self.assertFalse(cell["noise"])
        self.assertIsNone(cell["better"])
        self.assertNotIn("~", ra.fmt_cell(cell, "avg_fps"))
        # a single control run: no stdev at all, no mark either
        one = [dict(bench("A", 60.0), name="A", path="a"), dict(bench("B", 60.1), name="B", path="b")]
        cell = next(r for r in ra.bench_table(one)["rows"] if r["metric"] == "avg_fps")["groups"]["B"]
        self.assertFalse(cell["noise"])
        self.assertNotIn("~", ra.fmt_cell(cell, "avg_fps"))
        # a stdev above 0 still marks a diff within it
        noisy = [dict(bench("A", v), name="A", path="a") for v in (59.0, 61.0)]
        noisy += [dict(bench("B", 60.5), name="B", path="b")]
        cell = next(r for r in ra.bench_table(noisy)["rows"] if r["metric"] == "avg_fps")["groups"]["B"]
        self.assertTrue(cell["noise"])
        self.assertIn(" ~)", ra.fmt_cell(cell, "avg_fps"))

    def test_compare_no_noise_mark_without_control_stdev(self):
        a = [self.write(f"A{i}/bench.json", bench("A", 60.0)) for i in (1, 2)]
        b = [self.write("B1/bench.json", bench("B", 60.0))]
        out = self.run_cmd(ra.cmd_compare, control=a, variant=b, csv=None, noise_k=2.0)
        fps = next(line for line in out.splitlines() if line.startswith("avg FPS (bench)"))
        self.assertIn("+0.0%", fps)
        self.assertNotIn("~", fps)
        a = [self.write(f"C{i}/bench.json", bench("A", v)) for i, v in ((1, 59.0), (2, 61.0))]
        out = self.run_cmd(ra.cmd_compare, control=a, variant=b, csv=None, noise_k=2.0)
        fps = next(line for line in out.splitlines() if line.startswith("avg FPS (bench)"))
        self.assertIn("~", fps)

    def test_cmd_bench_prints_table(self):
        files = [self.write(f"{i:02d}-{n}/bench.json", bench(n, v, compiles=3 if i == 1 else 0))
                 for i, (n, v) in enumerate([("A", 60.0), ("B", 66.0), ("A", 61.0), ("B", 67.0)], 1)]
        out = self.run_cmd(ra.cmd_bench, runs=files, control=None, noise_k=2.0, skip_ms=1000.0,
                           csv=str(self.root / "t.csv"), json=str(self.root / "t.json"))
        self.assertIn("A (control)", out)
        self.assertIn("%", out)
        self.assertIn("cache not warm", out)
        self.assertEqual(json.loads((self.root / "t.json").read_text())["control"], "A")

    def frame_csv(self, rel, rows):
        """rows: (present_ms, shader_compiles, bench mark or None for no 'bench' column)."""
        bench = rows[0][2] is not None
        text = "frame,present_ms,shader_compiles" + (",gpu_profile_enabled,bench" if bench else "") + "\n"
        for i, (ms, c, mark) in enumerate(rows, 1):
            text += f"{i},{ms},{c}" + (f",0,{mark}" if bench else "") + "\n"
        return self.write(rel, text)

    def test_csv_bench_window(self):
        # menus (slow frames, compiles) before the window, 1000 ms skipped inside it, then outside
        rows = [(100.0, 9, "0")] * 20 + [(500.0, 3, "bench_start"), (500.0, 3, "1")]
        rows += [(10.0, 0, "1")] * 100 + [(40.0, 1, "1")] + [(200.0, 7, "bench_end")] + [(200.0, 7, "0")] * 5
        c = ra.load_bench(self.frame_csv("01-A/frames.csv", rows))
        self.assertEqual(c["csv_window"], "bench")
        self.assertEqual((c["frames"], c["over33"], c["over50"], c["compiles"]), (101, 1, 0, 1))
        self.assertEqual(c["worst_ms"], 40.0)
        # skip-ms counts from the bench_start row (the mark), not from the start of the file
        c0 = ra.bench_from_csv(self.frame_csv("02-A/frames.csv", rows), skip_ms=0)
        self.assertEqual((c0["frames"], c0["compiles"]), (103, 7))
        # ISS_BENCH_EXIT: the file ends inside the window (no bench_end row)
        eof = ra.bench_from_csv(self.frame_csv("03-A/frames.csv", rows[:-6]), skip_ms=0)
        self.assertEqual(eof["frames"], 103)
        # no bench_start row: the first 1 opens the window
        ones = [(100.0, 0, "0")] * 3 + [(10.0, 0, "1")] * 50 + [(100.0, 0, "bench_end")]
        self.assertEqual(ra.bench_from_csv(self.frame_csv("04-A/frames.csv", ones), skip_ms=0)["frames"], 50)
        # a window too short for a row of its own: bench_end alone, nothing measured
        short = [(10.0, 0, "0")] * 5 + [(10.0, 0, "bench_end")] + [(10.0, 0, "0")] * 5
        self.assertEqual(ra.bench_from_csv(self.frame_csv("05-A/frames.csv", short), skip_ms=0)["frames"], 0)
        # no 'bench' column (older build): the whole file
        whole = ra.bench_from_csv(self.frame_csv("06-A/frames.csv", [(10.0, 0, None)] * 30), skip_ms=0)
        self.assertEqual((whole["frames"], whole["csv_window"]), (30, "whole_file"))

    def test_fmt_cell_counts(self):
        self.assertEqual(ra.fmt_cell(dict(mean=2.0, sd=None), "over33"), "2")
        self.assertEqual(ra.fmt_cell(dict(mean=1.5, sd=0.7071), "over33"), "1.5 +-0.7")
        self.assertEqual(ra.fmt_cell(dict(mean=60.0, sd=None), "avg_fps"), "60.0")
        runs = [dict(bench("A", 60.0, over33=o), name="A", path="a") for o in (1, 2)]
        t = ra.bench_table(runs)
        over = next(r for r in t["rows"] if r["metric"] == "over33")
        self.assertEqual(ra.fmt_cell(over["groups"]["A"], "over33"), "1.5 +-0.7")

    def test_cache_warning_only_for_game_summaries(self):
        csv = self.frame_csv("01-A/frames.csv", [(10.0, 2, None)] * 200)   # loading compiles
        log = self.write("02-A/game.log", "Frame stats: 60.0 FPS, worst frame 20.0 ms, x; "
                                          "4 shader/pipeline compiles, 12.0 ms, 0 pending draws\n")
        out = self.run_cmd(ra.cmd_bench, runs=[csv, log], control=None, noise_k=2.0, skip_ms=0.0,
                           csv=None, json=None)
        self.assertNotIn("cache not warm", out)
        self.assertIn("whole file", out)
        bj = self.write("03-A/bench.json", bench("A", 60.0, compiles=2))
        out = self.run_cmd(ra.cmd_bench, runs=[bj], control=None, noise_k=2.0, skip_ms=0.0,
                           csv=None, json=None)
        self.assertIn("cache not warm", out)

    def test_compare_headers_name_the_folders(self):
        a = [self.write(f"s/{i:02d}-A/bench.json", bench("A", 60.0 + i)) for i in (1, 3)]
        b = [self.write("s/02-B/bench.json", bench("B", 70.0)), self.write("x/run1/bench.json", bench("B", 71.0))]
        self.assertEqual([ra.load_run(p)["name"] for p in a + b], ["01-A", "03-A", "02-B", "B (run1)"])
        out = self.run_cmd(ra.cmd_compare, control=a, variant=b, csv=None, noise_k=2.0)
        header = out.splitlines()[0]
        for name in ("01-A", "03-A", "02-B", "B (run1)"):
            self.assertIn(name, header)

    def test_compare_accepts_bench_and_keeps_summary_rows(self):
        a = [self.write(f"a{i}/summary.json", dict(name=f"a{i}", fps_mean=50.0 + i, fps_min=40.0)) for i in (1, 2)]
        b = [self.write("b1/summary.json", dict(name="b1", fps_mean=60.0, fps_min=41.0))]
        out = self.run_cmd(ra.cmd_compare, control=a, variant=b, csv=None, noise_k=2.0)
        self.assertIn("FPS medio", out)
        self.assertNotIn("(bench)", out)              # old format: no bench rows
        a = [self.write(f"A{i}/bench.json", bench("A", 60.0 + i)) for i in (1, 2)]
        b = [self.write("B1/bench.json", bench("B", 70.0))]
        out = self.run_cmd(ra.cmd_compare, control=a, variant=b, csv=None, noise_k=2.0)
        self.assertIn("avg FPS (bench)", out)
        self.assertIn("+13.8%", out)                  # 70 vs mean(61, 62)


if __name__ == "__main__":
    unittest.main()
