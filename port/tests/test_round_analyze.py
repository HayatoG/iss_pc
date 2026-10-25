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

    def test_cpu_ms_per_frame(self):
        # the game's line and JSON carry the process CPU time; the table shows it last, lower is better
        d = ra.parse_bench_line("Bench: frames=5000 seconds=90.00 avg_fps=55.6 low1_fps=40.0 p99_ms=25.0 "
                                "worst_ms=40.0 over33=0 over50=0 compiles=0 cpu_ms_per_frame=31.25 "
                                "cpu_s=156.25 cpu_util=1.74 label=A\n")
        self.assertEqual((d["cpu_ms_per_frame"], d["cpu_s"], d["cpu_util"], d["label"]), (31.25, 156.25, 1.74, "A"))
        cpu = {"A": (30.0, 30.2), "B": (30.1, 30.3), "C": (27.0, 27.1)}
        files = [self.write(f"{i:02d}-{n}/bench.json", dict(bench(n, 55.0 + i % 2 * 0.1), cpu_ms_per_frame=v))
                 for i, (n, v) in enumerate([(n, v) for n in "ABC" for v in cpu[n]], 1)]
        out = self.run_cmd(ra.cmd_bench, runs=files, control=None, noise_k=2.0, skip_ms=1000.0,
                           csv=str(self.root / "t.csv"), json=str(self.root / "t.json"))
        head = out.splitlines()[0]
        self.assertTrue(head.split()[-1] == "cpu_ms/f" and head.index("drift") < head.index("cpu_ms/f"))
        self.assertNotIn("cpu_game_ms/f", head)   # no split in these runs: no column
        b = next(line for line in out.splitlines() if line.startswith("B "))
        c = next(line for line in out.splitlines() if line.startswith("C "))
        self.assertRegex(b, r"30\.20 \+-0\.14 \(\+0\.3% ~\)$")   # within the control's noise
        self.assertRegex(c, r"27\.05 \+-0\.07 \(-10\.1%\)$")
        t = json.loads((self.root / "t.json").read_text())
        row = next(r for r in t["rows"] if r["metric"] == "cpu_ms_per_frame")
        self.assertFalse(row["higher_is_better"])
        self.assertTrue(row["groups"]["C"]["better"])
        self.assertTrue(row["groups"]["B"]["noise"])
        head, *rows = (self.root / "t.csv").read_text().splitlines()
        self.assertTrue(head.endswith(",draws/frame,drift,unreliable,failed_runs,cpu_ms/f,cpu_game_ms/f"))
        self.assertTrue(rows[2].endswith(",27.05 +-0.07 (-10.1%),-"))
        # runs of an older build have no CPU time: no column on the table, '-' in compare.csv
        old = [self.write(f"o{i}/bench.json", bench("A", 55.0)) for i in (1, 2)]
        out = self.run_cmd(ra.cmd_bench, runs=old, control=None, noise_k=2.0, skip_ms=1000.0,
                           csv=str(self.root / "o.csv"), json=None)
        self.assertNotIn("cpu_ms/f", out.splitlines()[0])
        self.assertTrue((self.root / "o.csv").read_text().splitlines()[1].endswith(",-,-"))

    def test_cpu_split(self):
        # the game's per-category CPU time (cycle time): cpu_game = guest + cp + gpu_helpers, the last
        # column, noise marked; the background compiles swing the total, not cpu_game
        line = ("Bench: frames=3400 seconds=59.50 avg_fps=57.1 low1_fps=33.0 p99_ms=30.0 worst_ms=50.0 "
                "over33=1 over50=0 compiles=2 cpu_ms_per_frame=9.05 cpu_s=30.77 cpu_util=0.52 "
                "cpu_guest_ms_per_frame=10.00 cpu_cp_ms_per_frame=15.00 cpu_gpu_helpers_ms_per_frame=5.00 "
                "cpu_compile_ms_per_frame=7.00 cpu_other_ms_per_frame=1.00 cpu_cycles_ms_per_frame=38.00 "
                "label=A\n")
        d = ra.load_bench(self.write("01-A/game.log", line))
        self.assertEqual((d["cpu_cp_ms_per_frame"], d["cpu_compile_ms_per_frame"], d["cpu_game_ms_per_frame"]),
                         (15.0, 7.0, 30.0))
        self.assertNotIn("cpu_game_ms_per_frame", ra.add_cpu_game(dict(cpu_guest_ms_per_frame=1.0)))

        def split(n, guest, cp, helpers, compile_, total):
            return dict(bench(n, 57.0), cpu_ms_per_frame=total, cpu_guest_ms_per_frame=guest,
                        cpu_cp_ms_per_frame=cp, cpu_gpu_helpers_ms_per_frame=helpers,
                        cpu_compile_ms_per_frame=compile_, cpu_other_ms_per_frame=1.0,
                        cpu_cycles_ms_per_frame=guest + cp + helpers + compile_ + 1.0)
        runs = {"A": [(10.0, 15.0, 5.0, 2.0, 4.0), (10.1, 15.0, 5.0, 12.0, 13.0)],
                "B": [(10.0, 15.1, 5.0, 0.0, 16.0), (10.1, 15.0, 5.0, 9.0, 9.0)],
                "C": [(9.0, 13.0, 5.0, 3.0, 12.0), (9.1, 13.0, 5.0, 8.0, 5.0)]}
        files = [self.write(f"{i:02d}-{n}/bench.json", split(n, *v))
                 for i, (n, v) in enumerate([(n, v) for n in "ABC" for v in runs[n]], 1)]
        out = self.run_cmd(ra.cmd_bench, runs=files, control=None, noise_k=2.0, skip_ms=1000.0,
                           csv=str(self.root / "s.csv"), json=str(self.root / "s.json"))
        self.assertEqual(out.splitlines()[0].split()[-2:], ["cpu_ms/f", "cpu_game_ms/f"])
        b = next(line for line in out.splitlines() if line.startswith("B "))
        c = next(line for line in out.splitlines() if line.startswith("C "))
        self.assertRegex(b, r"30\.10 \+-0\.00 \(\+0\.2% ~\)$")   # 30.1, 30.1 vs 30.0, 30.1: noise
        self.assertRegex(c, r"27\.05 \+-0\.07 \(-10\.0%\)$")       # 27.0, 27.1: better
        t = json.loads((self.root / "s.json").read_text())
        rows = {r["metric"]: r for r in t["rows"]}
        self.assertTrue(rows["cpu_ms_per_frame"]["groups"]["C"]["noise"])   # the total: lost in its noise
        self.assertTrue(rows["cpu_game_ms_per_frame"]["groups"]["C"]["better"])
        self.assertTrue(rows["cpu_cp_ms_per_frame"]["groups"]["C"]["better"])
        self.assertTrue(rows["cpu_compile_ms_per_frame"]["split"])
        self.assertEqual(rows["cpu_compile_ms_per_frame"]["groups"]["A"]["mean"], 7.0)
        self.assertEqual(t["drift"]["runs"][0]["cpu"]["cpu_game_ms_per_frame"], 30.0)
        # the categories stay out of the table and compare.csv
        self.assertNotIn("cpu_cp", out)
        chead, *crows = (self.root / "s.csv").read_text().splitlines()
        self.assertTrue(chead.endswith(",failed_runs,cpu_ms/f,cpu_game_ms/f"))
        self.assertTrue(crows[2].endswith(",27.05 +-0.07 (-10.0%)"))
        runs_csv = (self.root / "s_runs.csv").read_text().splitlines()
        self.assertTrue(runs_csv[0].endswith(
            ",failed,cpu_ms_per_frame,cpu_game_ms_per_frame,cpu_guest_ms_per_frame,cpu_cp_ms_per_frame,"
            "cpu_gpu_helpers_ms_per_frame,cpu_compile_ms_per_frame,cpu_other_ms_per_frame,cpu_cycles_ms_per_frame"))
        self.assertTrue(runs_csv[1].endswith(",4.0,30.0,10.0,15.0,5.0,2.0,1.0,33.0"))
        # --cpu-split: guest / cp / gpu_helpers after the existing columns, which keep their order;
        # compare.csv keeps its layout
        out = self.run_cmd(ra.cmd_bench, runs=files, control=None, noise_k=2.0, skip_ms=1000.0,
                           csv=str(self.root / "s2.csv"), json=None, cpu_split=True)
        head = out.splitlines()[0].split()
        self.assertEqual(head[-5:], ["cpu_ms/f", "cpu_game_ms/f", "guest_ms/f", "cp_ms/f", "gpu_helpers_ms/f"])
        self.assertEqual(head[:-3], out.replace("guest_ms/f", "").replace("cp_ms/f", "")
                         .replace("gpu_helpers_ms/f", "").splitlines()[0].split())
        c = next(line for line in out.splitlines() if line.startswith("C "))
        self.assertRegex(c, r"27\.05 \+-0\.07 \(-10\.0%\)\s+9\.05 \+-0\.07 \(-10\.0%\)\s+13\.00 \+-0\.00 \(-13\.3%\)"
                            r"\s+5\.00 \+-0\.00 \(\+0\.0%\)$")
        self.assertIn("the parts of cpu_game_ms/f", out)
        self.assertEqual((self.root / "s2.csv").read_text(), (self.root / "s.csv").read_text())
        # runs without the split: no category columns even with --cpu-split
        plain = [self.write(f"p{i}/bench.json", bench(n, 60.0)) for i, n in enumerate("AB")]
        out = self.run_cmd(ra.cmd_bench, runs=plain, control=None, noise_k=2.0, skip_ms=1000.0,
                           csv=None, json=None, cpu_split=True)
        self.assertNotIn("guest_ms/f", out)

    def test_session_folder(self):
        # a bench.ps1 session folder: its NN-<variant> runs (bench.json, else game.log), no warm-ups,
        # the control from variants.json; files given one by one still work
        s = self.root / "sess"
        self.write("sess/variants.json", dict(variants=["B", "A"], control="B"))
        self.write("sess/warmup-A-1/bench.json", bench("A", 10.0))
        self.write("sess/warmup-B-auto/bench.json", bench("B", 10.0))
        self.write("sess/compare.json", {"x": 1})
        self.write("sess/01-A/bench.json", bench("A", 60.0))
        self.write("sess/02-B/bench.json", bench("B", 66.0))
        self.write("sess/03-B/game.log", "Bench: frames=5000 seconds=90.0 avg_fps=67.0 low1_fps=50.0 "
                                         "p99_ms=20.0 worst_ms=40.0 over33=0 over50=0 compiles=0 label=B\n")
        self.write("sess/04-A/bench.json", bench("A", 61.0))
        (s / "05-A").mkdir()   # no result: skipped with a note
        files, control = ra.expand_runs([str(s)])
        self.assertEqual([Path(f).parent.name + "/" + Path(f).name for f in files],
                         ["01-A/bench.json", "02-B/bench.json", "03-B/game.log", "04-A/bench.json"])
        self.assertEqual(control, "B")
        err = io.StringIO()
        with contextlib.redirect_stderr(err):
            out = self.bench_drift([str(s)], json=str(self.root / "c.json"))
        self.assertIn("05-A: no bench.json", err.getvalue())
        self.assertIn("B (control)", out)
        a = next(line for line in out.splitlines() if line.startswith("A "))
        self.assertRegex(a, r"^A\s+2\s+60\.5 \+-0\.7 \(-9\.0%\)")
        d = json.loads((self.root / "c.json").read_text())
        self.assertEqual((d["control"], len(d["run_files"])), ("B", 4))
        with contextlib.redirect_stderr(io.StringIO()):
            out = self.bench_drift([str(s)], control="A")
        self.assertEqual(out.splitlines()[2].split()[:2], ["A", "(control)"])
        # explicit files: the first run's variant is the control, as before
        out = self.bench_drift([str(s / "01-A/bench.json"), str(s / "02-B/bench.json")])
        self.assertIn("A (control)", out)
        # a folder without run folders
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()) as e:
            self.bench_drift([str(self.root)])
        self.assertIn("without run folders", e.getvalue())

    def test_cmd_bench_prints_table(self):
        files = [self.write(f"{i:02d}-{n}/bench.json", bench(n, v, compiles=3 if i == 1 else 0))
                 for i, (n, v) in enumerate([("A", 60.0), ("B", 66.0), ("A", 61.0), ("B", 67.0)], 1)]
        out = self.run_cmd(ra.cmd_bench, runs=files, control=None, noise_k=2.0, skip_ms=1000.0,
                           csv=str(self.root / "t.csv"), json=str(self.root / "t.json"))
        self.assertIn("A (control)", out)
        self.assertIn("%", out)
        self.assertIn("cache not warm", out)
        self.assertEqual(json.loads((self.root / "t.json").read_text())["control"], "A")

    def test_paired_fps_ci(self):
        # per-round pairs (run.json 'round'): a steady +2% the unpaired table calls noise ('~'),
        # the paired CI excludes 0
        rows = [(1, "A", 60.0), (1, "B", 61.2), (2, "B", 62.2), (2, "A", 61.0), (3, "A", 59.0), (3, "B", 60.1)]
        files = []
        for i, (rd, n, v) in enumerate(rows, 1):
            files.append(self.write(f"s/{i:02d}-{n}/bench.json", bench(n, v)))
            self.write(f"s/{i:02d}-{n}/run.json", dict(status="ok", round=rd, warmup=0))
        runs = [ra.load_bench(f) for f in files]
        p = ra.paired_fps(runs, ["A", "B"], "A")["B"]
        self.assertTrue(p["by_round"])
        self.assertEqual(p["n"], 3)
        d = [100 * (61.2 - 60) / 60, 100 * (62.2 - 61) / 61, 100 * (60.1 - 59) / 59]
        self.assertAlmostEqual(p["mean"], sum(d) / 3)
        self.assertGreater(p["ci95"][0], 0)
        out = self.run_cmd(ra.cmd_bench, runs=files, control=None, noise_k=2.0, skip_ms=1000.0,
                           csv=None, json=str(self.root / "p.json"))
        self.assertIn("B vs A: +1.94% over 3 rounds", out)
        self.assertIn("(excludes 0)", out)
        self.assertEqual(json.loads((self.root / "p.json").read_text())["paired_avg_fps"]["B"]["n"], 3)
        # without run.json: the k-th run of each variant pairs with the k-th control run
        loose = [dict(bench(n, v), name=n, path=str(self.root / f"x{i}" / "bench.json"))
                 for i, (n, v) in enumerate([("A", 60.0), ("B", 60.0), ("A", 62.0), ("B", 61.0)])]
        p = ra.paired_fps(loose, ["A", "B"], "A")["B"]
        self.assertFalse(p["by_round"])
        self.assertEqual(p["n"], 2)
        self.assertLess(p["ci95"][0], 0)   # 0% and -1.6%: the CI includes 0
        # one pair: no CI; a missing control round: that round is not paired
        one = ra.paired_fps(loose[:2], ["A", "B"], "A")["B"]
        self.assertEqual((one["n"], one["ci95"]), (1, None))
        self.assertEqual(ra.paired_fps(loose[1:2], ["A", "B"], "A")["B"]["n"], 0)

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

    # ---------------------------------------------------------------- scene drift
    def drift_run(self, rel, label, avg, draws, window=True):
        """bench.json + game.log whose benchmark window has one Frame stats line per draws value
        (None: a line broken by another thread's output); lines outside the window must not count."""
        stats = lambda n: (f"Frame stats: 50.0 FPS, worst frame 30.0 ms; 0 shader/pipeline compiles, 0.0 ms; "
                           f"0 pending draws; GPU thread 3.50 us/draw, {n} draws/frame, idle 2.0%\n")
        log = stats(2) + stats(109) + ("Runtime: gameplay detected (scripted route)\n" + stats(140)
                                       + "Bench: window started (first 1000 ms skipped)\n" if window else "")
        for n in draws:
            log += stats(n) if n is not None else "Frame stats: 50.0 FPS, worst frame 3GPU [x] Ensure\n"
        log += "Runtime: pad replay finished\nBench: frames=4500 seconds=90.00 avg_fps=50.0\n" + stats(9999)
        self.write(f"{rel}/game.log", log)
        return self.write(f"{rel}/bench.json", bench(label, avg))

    def bench_drift(self, files, **kw):
        args = dict(runs=files, control=None, noise_k=2.0, skip_ms=1000.0, csv=None, json=None,
                    drift_pct=ra.DRIFT_PCT, exclude_drift=False)
        args.update(kw)
        return self.run_cmd(ra.cmd_bench, **args)

    def test_draws_from_log_window(self):
        self.drift_run("01-A", "A", 60.0, [3500, None, 2000])
        self.assertEqual(ra.draws_from_log(str(self.root / "01-A/game.log")), [3500, None, 2000])
        self.drift_run("02-A", "A", 60.0, [3500], window=False)   # no window: unknown
        self.assertIsNone(ra.draws_from_log(str(self.root / "02-A/game.log")))

    def test_no_drift(self):
        same = [3500, 4400, 2050, 5600, 4000]
        files = [self.drift_run(f"{i:02d}-{n}", n, 60.0 + i * 0.1, [round(x * (1 + i / 100)) for x in same])
                 for i, n in enumerate("ABBAAB", 1)]   # at most +6% per line, mean within +-3% of the median
        out = self.bench_drift(files, json=str(self.root / "t.json"), csv=str(self.root / "t.csv"))
        self.assertNotIn("WARNING", out)
        self.assertNotIn("!", out)
        self.assertIn("draws/frame", out.splitlines()[0])
        d = json.loads((self.root / "t.json").read_text())
        self.assertEqual(d["unreliable"], [])
        self.assertEqual(d["drift"]["drift_runs"], 0)
        self.assertEqual([x["drift"] for x in d["drift"]["runs"]], [False] * 6)
        # compare.csv keeps its columns, the new ones at the end; the per-run block is its own file
        self.assertTrue((self.root / "t.csv").read_text().startswith(
            "variant,runs,avg_fps,low1_fps,p99_ms,worst_ms,over33,over50,compiles,draws/frame,drift,unreliable,failed_runs,"
            "cpu_ms/f,cpu_game_ms/f\n"))
        self.assertIn("draws_dev_pct", (self.root / "t_runs.csv").read_text())
        self.assertEqual(len(d["run_files"]), 6)
        self.assertEqual(d["kept_run_files"], d["run_files"])

    def test_drift_detected(self):
        good = [3500, 4400, 2050, 5600, 4400, 4600]
        light = [3500, 4400, 2050, 5600, 2800, 2900]   # same start, then another scene
        files = [self.drift_run("01-A", "A", 52.0, good), self.drift_run("02-B", "B", 53.0, light),
                 self.drift_run("03-B", "B", 52.1, good), self.drift_run("04-A", "A", 52.2, good)]
        out = self.bench_drift(files, json=str(self.root / "t.json"), csv=str(self.root / "t.csv"))
        self.assertIn("WARNING: scene drift: 1 of 4 runs", out)
        self.assertIn("02-B (B)", out)
        self.assertNotIn("01-A (A)", out)
        b = next(line for line in out.splitlines() if line.startswith("B "))
        self.assertIn("!)", b)                          # B's diffs are unreliable
        self.assertIn("1/2", b)
        d = json.loads((self.root / "t.json").read_text())
        self.assertEqual(d["unreliable"], ["B"])
        runs = {x["run"]: x for x in d["drift"]["runs"]}
        self.assertTrue(runs["02-B"]["drift"])
        self.assertLess(runs["02-B"]["draws_dev_pct"], -5)
        self.assertFalse(runs["01-A"]["drift"])
        fps = next(r for r in d["rows"] if r["metric"] == "avg_fps")["groups"]["B"]
        self.assertTrue(fps["unreliable"])
        self.assertIsNone(fps["better"])
        self.assertIn("02-B,B,", (self.root / "t_runs.csv").read_text())
        self.assertIn("B,2,", (self.root / "t.csv").read_text())
        # a looser threshold lets it pass; a drift run in the control makes every variant unreliable
        self.assertNotIn("WARNING", self.bench_drift(files, drift_pct=30.0))
        out = self.bench_drift(files, control="B")
        self.assertIn("diffs of A are unreliable", out)

    def test_exclude_drift(self):
        good = [3500, 4400, 2050, 5600, 4400, 4600]
        light = [3500, 4400, 2050, 5600, 2800, 2900]
        files = [self.drift_run("01-A", "A", 52.0, good), self.drift_run("02-B", "B", 60.0, light),
                 self.drift_run("03-B", "B", 52.1, good), self.drift_run("04-A", "A", 52.2, good),
                 self.drift_run("05-A", "A", 52.1, good), self.drift_run("06-B", "B", 52.0, good)]
        out = self.bench_drift(files, exclude_drift=True, json=str(self.root / "t.json"))
        self.assertIn("5 of 6 runs remain (A 3, B 2)", out)
        self.assertNotIn("!)", out)
        d = json.loads((self.root / "t.json").read_text())
        self.assertEqual(d["runs"], {"A": 3, "B": 2})
        self.assertEqual(d["drift"]["runs_used"], 5)
        self.assertTrue(next(x for x in d["drift"]["runs"] if x["run"] == "02-B")["excluded"])
        fps = next(r for r in d["rows"] if r["metric"] == "avg_fps")["groups"]["B"]
        self.assertAlmostEqual(fps["mean"], 52.05)    # without the 60.0 of the drift run
        # every control run drifted: an error, not an empty comparison
        heavy = [x * 2 for x in good]
        # (B spread over 5%: not a variant of its own draw count, see test_variant_draws)
        files = [self.drift_run("11-A", "A", 52.0, heavy), self.drift_run("12-B", "B", 52.0, good),
                 self.drift_run("13-B", "B", 52.0, [round(x * 1.03) for x in good]),
                 self.drift_run("14-B", "B", 52.0, [round(x * 0.97) for x in good])]
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                self.bench_drift(files, exclude_drift=True)

    def test_drift_with_different_sample_counts(self):
        good = [3500, 4400, 2050, 5600, 4400, 4600, 3900]
        files = [self.drift_run("01-A", "A", 52.0, good), self.drift_run("02-B", "B", 52.0, good[:-1]),
                 self.drift_run("03-B", "B", 52.0, good + [4100]), self.drift_run("04-A", "A", 52.0, good[:-1])]
        out = self.bench_drift(files, json=str(self.root / "t.json"))
        self.assertNotIn("WARNING", out)
        d = json.loads((self.root / "t.json").read_text())
        self.assertEqual([len(x["draws"]) for x in d["drift"]["runs"]], [7, 6, 8, 6])
        self.assertEqual([x["drift"] for x in d["drift"]["runs"]], [False] * 4)
        # a run one line longer that then drifts is still caught
        files.append(self.drift_run("05-A", "A", 52.0, good[:4] + [1500, 1500, 1500, 1500]))
        self.assertIn("05-A (A)", self.bench_drift(files))

    def test_run_without_frame_stats_is_unknown(self):
        good = [3500, 4400, 2050, 5600]
        files = [self.drift_run("01-A", "A", 52.0, good), self.drift_run("02-B", "B", 52.0, good),
                 self.drift_run("03-B", "B", 52.0, good), self.drift_run("04-A", "A", 52.0, [])]
        files.append(self.write("05-A/bench.json", bench("A", 52.0)))       # no game.log at all
        out = self.bench_drift(files, json=str(self.root / "t.json"))
        self.assertNotIn("WARNING", out)
        self.assertIn("04-A", out)
        self.assertIn("draws/frame unknown", out)
        a = next(line for line in out.splitlines() if line.startswith("A (control)"))
        self.assertIn("0/3 ?2", a)
        d = json.loads((self.root / "t.json").read_text())
        runs = {x["run"]: x for x in d["drift"]["runs"]}
        self.assertIsNone(runs["04-A"]["drift"])
        self.assertIsNone(runs["05-A"]["draws_mean"])
        # too few runs with draws: no check, no crash
        out = self.bench_drift(files[3:] + files[:1])
        self.assertIn("drift unchecked", out)
        self.assertIn("unchecked", next(line for line in out.splitlines() if line.startswith("A (control)")))
        self.assertNotIn("?", next(line for line in out.splitlines() if line.startswith("A (control)")))
        self.assertNotIn("WARNING", out)


    def test_drift_inconclusive(self):
        # two scenes, half the runs each: the median (3500) flags every run, so no verdict
        hi, lo = [4000] * 5, [3000] * 5
        files = [self.drift_run("01-A", "A", 52.0, hi), self.drift_run("02-B", "B", 52.0, lo),
                 self.drift_run("03-B", "B", 52.0, hi), self.drift_run("04-A", "A", 52.0, lo)]
        out = self.bench_drift(files, json=str(self.root / "t.json"))
        self.assertIn("WARNING: drift check inconclusive", out)
        self.assertNotIn("WARNING: scene drift", out)
        self.assertNotIn("!)", out)
        d = json.loads((self.root / "t.json").read_text())
        self.assertEqual(d["drift"]["state"], "inconclusive")
        self.assertEqual([x["drift"] for x in d["drift"]["runs"]], [None] * 4)
        self.assertIn("inconclusive", next(line for line in out.splitlines() if line.startswith("B ")))
        # --exclude-drift has nothing to leave out then
        self.assertNotIn("excluded", self.bench_drift(files, exclude_drift=True))

    def test_variant_draws(self):
        # B draws 25% less in every run and tightly: the variant does it (culling, LOD), not drift
        files = [self.drift_run(f"0{i}-A", "A", 52.0, [4000 + i] * 5) for i in (1, 4, 5)]
        files += [self.drift_run(f"0{i}-B", "B", 60.0, [3000 + i] * 5) for i in (2, 3, 6)]
        out = self.bench_drift(files, json=str(self.root / "t.json"))
        self.assertNotIn("WARNING", out)
        self.assertNotIn("!", out)
        self.assertIn("note: B renders -25.0% draws/frame", out)
        self.assertIn("not like-for-like", out)
        self.assertIn("variant", next(line for line in out.splitlines() if line.startswith("B ")))
        d = json.loads((self.root / "t.json").read_text())
        self.assertEqual(list(d["drift"]["variant_draws"]), ["B"])
        self.assertAlmostEqual(d["drift"]["variant_draws"]["B"], -25.0, delta=0.1)
        self.assertEqual(d["unreliable"], [])
        # a drift run among the control's runs: the control is no reference, so no variant verdict;
        # three scenes then, which the median cannot sort out either: inconclusive, never a false '!'
        files.append(self.drift_run("07-A", "A", 52.0, [2000] * 5))
        out = self.bench_drift(files)
        self.assertNotIn("note: B renders", out)
        self.assertIn("drift check inconclusive", out)
        self.assertNotIn("!)", out)

    def test_incomplete_window_is_unknown(self):
        # crash or timeout: the window opened but never ended, so the series is not used
        self.drift_run("01-A", "A", 52.0, [4000] * 3)
        log = self.root / "01-A/game.log"
        text = log.read_text()
        log.write_text(text[:text.index("Runtime: pad replay finished")])
        self.assertIsNone(ra.draws_from_log(str(log)))
        self.assertIsNone(ra.draws_fingerprint(ra.load_bench(str(self.root / "01-A/bench.json")))["draws_mean"])

    def test_exclude_drift_edge_cases(self):
        good = [3500, 4400, 2050, 5600, 4400, 4600]
        light = [x // 2 for x in good]
        files = [self.drift_run("01-A", "A", 52.0, good), self.drift_run("02-B", "B", 52.0, light),
                 self.drift_run("03-A", "A", 52.0, good), self.drift_run("04-C", "C", 52.0, good),
                 self.drift_run("05-A", "A", 52.0, good)]
        # every run of B drifted: listed, also in the JSON, with a drift count over all runs
        out = self.bench_drift(files, exclude_drift=True, json=str(self.root / "t.json"))
        self.assertIn("B: all 1 runs excluded (drift), no comparison", out)
        d = json.loads((self.root / "t.json").read_text())
        self.assertEqual(d["excluded_variants"], [{"variant": "B", "runs": 1, "reason": "drift"}])
        self.assertEqual(len(d["run_files"]), 5)
        self.assertEqual(len(d["kept_run_files"]), 4)
        # an unknown control is that error, not "every run of the control drifted"
        err = io.StringIO()
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(err):
            self.bench_drift(files, control="Z", exclude_drift=True)
        self.assertIn("not one of", err.getvalue())
        # the drift count of a variant matches the table also under --exclude-drift
        files.append(self.drift_run("06-C", "C", 52.0, light))
        self.bench_drift(files, exclude_drift=True, json=str(self.root / "t.json"))
        d = json.loads((self.root / "t.json").read_text())
        self.assertEqual((d["draws"]["C"]["drift"], d["draws"]["C"]["total"]), (1, 2))
        self.assertEqual(d["runs"]["C"], 1)

    def test_control_drift_with_no_other_variant(self):
        good = [4000] * 5
        files = [self.drift_run(f"0{i}-A", "A", 52.0, good) for i in (1, 2, 3)]
        files.append(self.drift_run("04-A", "A", 52.0, [2000] * 5))
        out = self.bench_drift(files)
        self.assertIn("in the control's own runs", out)
        self.assertNotIn("diffs of  are", out)
        # and the count-diff branch of fmt_cell marks an unreliable cell too
        self.assertEqual(ra.fmt_cell(dict(mean=2.0, sd=None, diff_abs=2.0, unreliable=True), "over33"), "2 (+2 !)")

    # ---------------------------------------------------------------- failed runs
    def bench_run(self, rel, label, avg, status="ok"):
        self.write(f"{rel}/run.json", dict(status=status, variant=label))
        return self.write(f"{rel}/bench.json", bench(label, avg))

    def crashed_run(self, rel, label, status="exit 3, no bench result"):
        """bench.ps1 run that crashed before its window: run.json, a game.log of loading frames only."""
        self.write(f"{rel}/run.json", dict(status=status, variant=label))
        return self.write(f"{rel}/game.log", "Frame stats: 240.0 FPS, worst frame 9.0 ms, x; "
                                             "4 shader/pipeline compiles, 12.0 ms, 0 pending draws\n")

    def test_failed_runs_left_out(self):
        files = [self.bench_run("01-A", "A", 50.0), self.bench_run("02-B", "B", 55.0),
                 self.crashed_run("03-B", "B"), self.bench_run("04-A", "A", 51.0),
                 self.bench_run("05-B", "B", 56.0)]
        out = self.bench_drift(files, csv=str(self.root / "c.csv"), json=str(self.root / "c.json"))
        fps = next(line for line in out.splitlines() if line.startswith("B "))
        self.assertIn("55.5 +-0.7", fps)                    # 240 FPS of the loading screens not counted
        self.assertRegex(fps, r"^B\s+2\s")
        self.assertIn("WARNING: 1 of 5 runs failed", out)
        self.assertIn("03-B (B): status 'exit 3, no bench result'; no bench summary or window (read as log_stats)", out)
        self.assertNotIn("read as log_stats", out.split("WARNING")[0])
        d = json.loads((self.root / "c.json").read_text())
        self.assertEqual([(f["run"], f["variant"]) for f in d["failed_runs"]], [("03-B", "B")])
        self.assertEqual(len(d["run_files"]), 5)
        self.assertEqual(len(d["kept_run_files"]), 4)
        self.assertEqual(d["runs"]["B"], 2)
        runs_csv = (self.root / "c_runs.csv").read_text().splitlines()
        self.assertIn(",excluded,failed,cpu_ms_per_frame,", runs_csv[0])
        row = next(line for line in runs_csv if line.startswith("03-B"))
        self.assertIn("failed,true,", row)
        self.assertIn("status 'exit 3", row)
        head, *rows = (self.root / "c.csv").read_text().splitlines()
        self.assertTrue(head.endswith(",unreliable,failed_runs,cpu_ms/f,cpu_game_ms/f"))
        self.assertEqual([r.rsplit(",", 3)[1:] for r in rows], [["0", "-", "-"], ["1", "-", "-"]])

    def test_failed_status_with_bench_json_and_ok_log_stats(self):
        # status not ok excludes a run even with a bench.json; a log with run.json status ok but
        # no Bench line is failed too (no window), while a log outside a bench.ps1 folder is kept
        files = [self.bench_run("01-A", "A", 50.0), self.bench_run("02-A", "A", 51.0),
                 self.bench_run("03-A", "A", 90.0, status="timeout"), self.crashed_run("04-A", "A", status="ok"),
                 self.write("loose/game.log", "Frame stats: 60.0 FPS, worst frame 20.0 ms, x; "
                                              "0 shader/pipeline compiles, 0.0 ms, 0 pending draws\n")]
        out = self.bench_drift(files, json=str(self.root / "c.json"))
        d = json.loads((self.root / "c.json").read_text())
        self.assertEqual({f["run"]: f["reason"] for f in d["failed_runs"]},
                         {"03-A": "status 'timeout'",
                          "04-A": "no bench summary or window (read as log_stats)"})
        self.assertEqual(d["runs"], {"A": 2, "loose": 1})
        self.assertIn("loose/game.log: no bench summary, read as log_stats", out.replace("\\", "/"))

    def test_every_run_of_a_variant_or_the_control_failed(self):
        files = [self.bench_run("01-A", "A", 50.0), self.crashed_run("02-B", "B"), self.bench_run("03-A", "A", 51.0)]
        out = self.bench_drift(files, json=str(self.root / "c.json"))
        self.assertIn("B: all 1 runs excluded (failed), no comparison", out)
        d = json.loads((self.root / "c.json").read_text())
        self.assertEqual(d["excluded_variants"], [{"variant": "B", "runs": 1, "reason": "failed"}])
        err = io.StringIO()
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(err):
            self.bench_drift(files, control="B")
        self.assertIn("every run of the control 'B' failed", err.getvalue())
        self.assertIn("02-B: status", err.getvalue())

if __name__ == "__main__":
    unittest.main()
