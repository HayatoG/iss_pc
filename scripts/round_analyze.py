#!/usr/bin/env python3
"""round_analyze.py - resume uma rodada de teste de desempenho do port inFAMOUS.

Uso (sempre com python3 -I):
  python3 -I round_analyze.py summarize [--log LOG] [--mon DIR] [--window SEG]
        [--skip SEG] [--name NOME] [--out DIR] [--sample-secs 5] [--min-fps 0]
  python3 -I round_analyze.py compare --control A.json [B.json ...]
        --variant C.json [D.json ...] [--csv saida.csv] [--noise-k 2]

summarize: le o log do jogo (linhas 'Frame stats' / 'Frame pacing', uma de cada
a cada ~5 s) e os CSVs do round_monitor.sh (gpu.csv, cpu.csv, mem.csv);
imprime o resumo e grava summary.json + summary.csv em --out (se dado).
--window SEG   usa so os ultimos SEG segundos (log: SEG/sample-secs amostras;
               CSVs: t >= t_final - SEG).
--skip SEG     descarta os primeiros SEG segundos (aquecimento), antes da janela.
compare: tabela controle(s) vs variantes, diferenca em % sobre a media dos
controles e o desvio entre controles (ruido); marca '~' quando a diferenca
fica dentro do ruido. Aceita tambem o JSON do benchmark (ISS_BENCH_JSON).

  python3 -I round_analyze.py bench RUN [RUN ...] [--control NOME]
        [--noise-k 2] [--skip-ms 1000] [--csv saida.csv] [--json saida.json]

bench (automated benchmark, scripts/bench.ps1): one row per variant, mean +-
sample stdev of avg_fps, low1_fps, p99_ms, worst_ms, over33, over50, compiles
and the % diff vs the control variant (the first one, or --control). A RUN is
a bench JSON (ISS_BENCH_JSON), a summary.json of 'summarize', a game log (its
last 'Bench:' line, else its Frame stats/pacing lines) or a SHADPS4_FRAME_CSV
file (only its benchmark window when it has the 'bench' column). Runs are
grouped by their label (ISS_BENCH_LABEL), else by the name of their folder
without a leading 'NN-'. '~' = the diff is within --noise-k x the control's
stdev (likely noise); a stdev of 0 (or a single control run) gives no mark.
"""
import argparse, csv, glob, json, math, os, re, statistics as st, sys

NUM = r"([0-9]+(?:\.[0-9]+)?)"
RE_STATS = re.compile(
    r"Frame stats: " + NUM + r" FPS, worst frame " + NUM + r" ms.*?; "
    + r"(\d+) shader/pipeline compiles, " + NUM + r" ms.*?(\d+) pending draws")
RE_PACE = re.compile(
    r"Frame pacing: median " + NUM + r" ms, stddev " + NUM + r" ms, p99 " + NUM
    + r" ms, (\d+) frames over")
RE_IDLE = re.compile(r"GPU thread .*?idle " + NUM + r"%")
THROTTLE = [(0x1, "gpu_idle"), (0x2, "app_clocks"), (0x4, "sw_power_cap"),
            (0x8, "hw_slowdown"), (0x10, "sync_boost"), (0x20, "sw_thermal"),
            (0x40, "hw_thermal"), (0x80, "hw_power_brake")]
# razoes que de fato limitam desempenho (gpu_idle/app_clocks/sync_boost nao)
LIMITING = {"sw_power_cap", "hw_slowdown", "sw_thermal", "hw_thermal", "hw_power_brake"}


def mean(v): return sum(v) / len(v) if v else None
def pct(a, b): return None if (a is None or not b) else 100.0 * (a - b) / b
def r(x, n=2): return None if x is None else round(x, n)


def resolve_log(path):
    if path is None:
        path = "port/out/last_run.log"
        here = os.path.dirname(os.path.abspath(__file__))
        cand = os.path.join(here, "..", path)
        if os.path.exists(cand): path = cand
    return os.path.realpath(path)


def parse_log(path, window_s, skip_s, sample_s, min_fps):
    stats, pace = [], []
    with open(path, errors="replace") as f:
        for line in f:
            if "Frame stats:" in line:
                m = RE_STATS.search(line)
                if m:
                    idle = RE_IDLE.search(line)
                    stats.append(dict(fps=float(m[1]), worst=float(m[2]), comp=int(m[3]),
                                      comp_ms=float(m[4]), pend=int(m[5]),
                                      gpu_idle=float(idle[1]) if idle else None))
            elif "Frame pacing:" in line:
                m = RE_PACE.search(line)
                if m:
                    pace.append(dict(median=float(m[1]), std=float(m[2]), p99=float(m[3]), over=int(m[4])))
    total = len(stats)
    skip_n = int(round(skip_s / sample_s)) if skip_s else 0
    stats, pace = stats[skip_n:], pace[skip_n:]
    if window_s:
        n = max(1, int(round(window_s / sample_s)))
        stats, pace = stats[-n:], pace[-n:]
    if min_fps:
        keep = [i for i, s in enumerate(stats) if s["fps"] >= min_fps]
        pace = [pace[i] for i in keep if i < len(pace)]
        stats = [stats[i] for i in keep]
    out = dict(log=path, log_samples_total=total, samples=len(stats), pacing_samples=len(pace))
    if not stats: return out
    fps = [s["fps"] for s in stats]
    out.update(
        fps_mean=r(mean(fps)), fps_min=r(min(fps)), fps_max=r(max(fps)),
        fps_stdev=r(st.pstdev(fps)),
        worst_frame_ms=r(max(s["worst"] for s in stats)),
        worst_frame_mean_ms=r(mean([s["worst"] for s in stats])),
        shader_compiles=sum(s["comp"] for s in stats),
        shader_compile_ms=r(sum(s["comp_ms"] for s in stats)),
        pending_draws_mean=r(mean([s["pend"] for s in stats])),
        pending_draws_max=max(s["pend"] for s in stats),
    )
    idl = [s["gpu_idle"] for s in stats if s["gpu_idle"] is not None]
    if idl: out["gpu_thread_idle_pct_mean"] = r(mean(idl))
    if pace:
        p99 = [p["p99"] for p in pace]
        out.update(
            pacing_median_ms=r(mean([p["median"] for p in pace]), 3),
            pacing_stddev_ms=r(mean([p["std"] for p in pace]), 3),
            pacing_p99_mean_ms=r(mean(p99), 3), pacing_p99_max_ms=r(max(p99), 3),
            frames_over_1p5x=sum(p["over"] for p in pace),
            # 1% low aproximado: FPS equivalente ao p99 do tempo de frame
            fps_1pct_low=r(1000.0 / mean(p99)) if mean(p99) else None,
            fps_1pct_low_worst_sample=r(1000.0 / max(p99)) if max(p99) else None,
        )
    return out


def read_csv(path):
    try:
        with open(path, newline="") as f: return list(csv.DictReader(f))
    except OSError:
        return []


def fnum(row, key):
    try: return float(row[key])
    except (KeyError, ValueError, TypeError): return None


def window_rows(rows, window_s, skip_s):
    if not rows: return rows
    ts = [fnum(x, "t") for x in rows]
    ts = [t for t in ts if t is not None]
    if not ts: return rows
    t0, t1 = ts[0], ts[-1]
    lo = t0 + (skip_s or 0)
    if window_s: lo = max(lo, t1 - window_s)
    return [x for x in rows if (fnum(x, "t") or 0) >= lo]


def col(rows, key):
    return [v for v in (fnum(x, key) for x in rows) if v is not None]


def parse_mon(d, window_s, skip_s):
    out = {}
    gpu = window_rows(read_csv(os.path.join(d, "gpu.csv")), window_s, skip_s)
    cpu = window_rows(read_csv(os.path.join(d, "cpu.csv")), window_s, skip_s)
    mem = window_rows(read_csv(os.path.join(d, "mem.csv")), window_s, skip_s)
    out["mon_dir"] = os.path.abspath(d)
    out["mon_samples"] = dict(gpu=len(gpu), cpu=len(cpu), mem=len(mem))
    if gpu:
        for key, name in [("util_gpu", "gpu_util"), ("util_mem", "gpu_memctl_util"),
                          ("clk_sm_mhz", "clk_sm"), ("clk_mem_mhz", "clk_mem"),
                          ("power_w", "power"), ("temp_c", "temp")]:
            v = col(gpu, key)
            if v:
                out[name + "_mean"] = r(mean(v)); out[name + "_max"] = r(max(v)); out[name + "_min"] = r(min(v))
        for key, name in [("mem_used_mib", "vram_used_max_mib"), ("proc_vram_mib", "vram_proc_max_mib")]:
            v = col(gpu, key)
            if v: out[name] = r(max(v), 0)
        masks = []
        for x in gpu:
            try: masks.append(int(x["throttle_mask"], 0))
            except (KeyError, ValueError): pass
        if masks:
            act = {}
            for bit, nm in THROTTLE:
                c = sum(1 for m in masks if m & bit)
                if c: act[nm] = r(100.0 * c / len(masks), 1)
            out["throttle_pct_samples"] = act
            out["throttle_limiting"] = sorted(k for k in act if k in LIMITING)
    if cpu:
        tot = col(cpu, "cpu_total")
        if tot: out["cpu_total_mean"] = r(mean(tot)); out["cpu_total_max"] = r(max(tot))
        cores = [k for k in cpu[0] if re.fullmatch(r"cpu\d+", k)]
        means = {k: mean(col(cpu, k)) for k in cores if col(cpu, k)}
        if means:
            hot = max(means, key=means.get)
            out["cpu_hot_core"] = hot
            out["cpu_hot_core_mean"] = r(means[hot])
            out["cpu_core_max"] = r(max(max(col(cpu, k)) for k in means))
        tc = col(cpu, "cpu_temp_c")
        if tc: out["cpu_temp_max"] = r(max(tc))
    if mem:
        for key, name in [("mem_used_mib", "ram_used_max_mib"), ("proc_rss_mib", "rss_max_mib"),
                          ("swap_used_mib", "swap_used_max_mib")]:
            v = col(mem, key)
            if v: out[name] = r(max(v), 0)
        pc = col(mem, "proc_cpu_pct")
        if pc: out["proc_cpu_pct_mean"] = r(mean(pc)); out["proc_cpu_pct_max"] = r(max(pc))
    return out


def flat(d):
    o = {}
    for k, v in d.items():
        if isinstance(v, dict):
            for k2, v2 in v.items(): o[f"{k}.{k2}"] = v2
        elif isinstance(v, list): o[k] = " ".join(map(str, v))
        else: o[k] = v
    return o


def cmd_summarize(a):
    res = dict(name=a.name or os.path.basename(os.path.abspath(a.mon)) if (a.name or a.mon) else "run",
               window_s=a.window, skip_s=a.skip)
    log = resolve_log(a.log)
    if os.path.exists(log):
        res.update(parse_log(log, a.window, a.skip, a.sample_secs, a.min_fps))
    else:
        print(f"aviso: log nao encontrado: {log}", file=sys.stderr)
    if a.mon: res.update(parse_mon(a.mon, a.window, a.skip))
    for k, v in flat(res).items(): print(f"{k:32s} {v}")
    if a.out:
        os.makedirs(a.out, exist_ok=True)
        with open(os.path.join(a.out, "summary.json"), "w") as f: json.dump(res, f, indent=2)
        fl = flat(res)
        with open(os.path.join(a.out, "summary.csv"), "w", newline="") as f:
            w = csv.writer(f); w.writerow(["metric", "value"])
            for k, v in fl.items(): w.writerow([k, v])
        print(f"\ngravado: {a.out}/summary.json e summary.csv")


# metrica, rotulo, "maior e melhor"? (None = neutro)
CMP = [("fps_mean", "FPS medio", True), ("fps_min", "FPS minimo", True),
       ("fps_1pct_low", "1% low (p99)", True), ("worst_frame_ms", "pior frame ms", False),
       ("shader_compiles", "compilacoes shader", False), ("pending_draws_mean", "draws pend. med", False),
       ("pending_draws_max", "draws pend. max", False),
       ("gpu_util_mean", "GPU util med %", None), ("gpu_util_max", "GPU util max %", None),
       ("cpu_total_mean", "CPU total med %", None), ("cpu_hot_core_mean", "nucleo mais carr. med %", None),
       ("cpu_core_max", "nucleo max %", None), ("clk_sm_mean", "clk SM med MHz", True),
       ("power_mean", "potencia med W", None), ("temp_max", "temp max C", False),
       ("vram_used_max_mib", "VRAM max MiB", False), ("rss_max_mib", "RSS max MiB", False),
       ("ram_used_max_mib", "RAM usada max MiB", False),
       # bench JSON (ISS_BENCH_JSON): absent in summary.json, so these rows only show for bench runs
       ("avg_fps", "avg FPS (bench)", True), ("low1_fps", "1% low FPS (bench)", True),
       ("p99_ms", "p99 ms (bench)", False), ("worst_ms", "worst ms (bench)", False),
       ("over33", "frames >33 ms (bench)", False), ("over50", "frames >50 ms (bench)", False),
       ("compiles", "compiles (bench)", False)]


def load_run(p):
    with open(p) as f: d = json.load(f)
    folder = os.path.basename(os.path.dirname(os.path.abspath(p)))
    label = d.get("label") if is_bench(d) else None
    if label not in (None, "", "-") and "name" not in d:
        # bench JSON: every run of a variant has the same label, so the column also names its
        # folder (bench.ps1's 01-A, 03-A...), unless the folder name already contains the label
        d["name"] = folder if label in folder else f"{label} ({folder})"
    d.setdefault("name", folder)
    return d


# ------------------------------------------------------------------ bench (scripts/bench.ps1)
# The game's summary when a TLG_PAD_REPLAY route ends: the 'Bench:' log line and ISS_BENCH_JSON.
BENCH_KEYS = ("frames", "seconds", "avg_fps", "low1_fps", "p99_ms", "worst_ms",
              "over33", "over50", "compiles")
BENCH_INT = {"frames", "over33", "over50", "compiles"}
# metric, higher is better
BENCH_CMP = [("avg_fps", True), ("low1_fps", True), ("p99_ms", False), ("worst_ms", False),
             ("over33", False), ("over50", False), ("compiles", False)]
RE_BENCH = re.compile(r"Bench: (.*?)(?:\s+label=(.*?))?\s*$")


def is_bench(d):
    return isinstance(d, dict) and "avg_fps" in d and "fps_mean" not in d


def _num(key, text):
    try: return int(text) if key in BENCH_INT else float(text)
    except ValueError:
        try: return float(text)
        except ValueError: return None


def parse_bench_line(text):
    """The last 'Bench: frames=... label=...' line of a log (None when there is none)."""
    found = None
    for line in text.splitlines():
        if "Bench: " not in line: continue
        m = RE_BENCH.search(line)
        if not m: continue
        d = {}
        for tok in m[1].split():
            k, sep, v = tok.partition("=")
            if sep: d[k] = _num(k, v)
        if "avg_fps" not in d: continue
        d["label"] = "" if m[2] in (None, "-") else m[2]   # '-': ISS_BENCH_LABEL unset
        found = d
    return found


def bench_from_summary(d):
    """summary.json (summarize) in bench keys: what the 5 s Frame stats/pacing lines can tell."""
    return dict(frames=None, seconds=None, avg_fps=d.get("fps_mean"), low1_fps=d.get("fps_1pct_low"),
                p99_ms=d.get("pacing_p99_mean_ms"), worst_ms=d.get("worst_frame_ms"),
                over33=None, over50=None, compiles=d.get("shader_compiles"),
                label=d.get("name") or "", source="summary")


def bench_from_frames(times_ms, skip_ms=1000.0):
    """Bench keys from per-frame present intervals (ms), skipping the first skip_ms of them, with
    the game's own definitions (gpu_v2/gpu/gpu_host.cpp): p99 = sorted[min(n-1, n*99//100)],
    low1_fps = 1000 / p99, over33 = frames over 33.4 ms."""
    t, skipped = [], 0.0
    for x in times_ms:
        if x is None or x <= 0: continue
        if skipped < skip_ms: skipped += x; continue
        t.append(x)
    if not t:
        return dict(frames=0, seconds=0.0, avg_fps=None, low1_fps=None, p99_ms=None,
                    worst_ms=None, over33=0, over50=0, compiles=None)
    srt = sorted(t)
    secs = sum(t) / 1000.0
    p99 = srt[min(len(srt) - 1, len(srt) * 99 // 100)]
    return dict(frames=len(t), seconds=r(secs), avg_fps=r(len(t) / secs, 1),
                low1_fps=r(1000.0 / p99, 1), p99_ms=r(p99, 1), worst_ms=r(srt[-1], 1),
                over33=sum(1 for x in t if x > 33.4), over50=sum(1 for x in t if x > 50.0), compiles=None)


def bench_window_rows(rows):
    """The rows of the benchmark window of a SHADPS4_FRAME_CSV ('bench' column: 0 outside, 1 inside,
    'bench_start' on the first row of the window, 'bench_end' on the first row after it): from
    bench_start (or the first 1) up to before bench_end (or the next 0, or the end of the file: with
    ISS_BENCH_EXIT the game may exit before the bench_end row). None when the column is absent;
    [] when the window never opened (or opened and closed between two rows: bench_end alone)."""
    if not rows or "bench" not in rows[0]: return None
    out, inside = [], False
    for x in rows:
        mark = (x.get("bench") or "").strip()
        if not inside:
            if mark in ("bench_start", "1"): inside = True
            elif mark == "bench_end": break
            else: continue
        elif mark in ("bench_end", "0", "bench_start"):
            break
        out.append(x)
    return out


def bench_from_csv(path, skip_ms=1000.0):
    """SHADPS4_FRAME_CSV (renderer v2): present_ms per frame; shader_compiles summed when present.
    Only the benchmark window when the CSV has the 'bench' column (the game's window starts at the
    mark, before ISS_BENCH_SKIP_MS, so skip_ms still applies), else the whole file."""
    rows = read_csv(path)
    window = bench_window_rows(rows)
    has_column = window is not None
    if has_column: rows = window
    d = bench_from_frames([fnum(x, "present_ms") for x in rows], skip_ms)
    d["csv_window"] = "bench" if has_column else "whole_file"
    if rows and "shader_compiles" in rows[0]:
        acc, skipped = 0, 0.0
        for x in rows:
            ms = fnum(x, "present_ms") or 0.0
            if ms <= 0: continue
            if skipped < skip_ms: skipped += ms; continue
            acc += int(fnum(x, "shader_compiles") or 0)
        d["compiles"] = acc
    d["label"] = ""
    d["source"] = "frame_csv"
    return d


def load_bench(path, skip_ms=1000.0):
    """One benchmark run in bench keys, plus 'name' (its group), 'path' and 'source'."""
    ext = os.path.splitext(path)[1].lower()
    if ext == ".json":
        with open(path, encoding="utf-8") as f: d = json.load(f)
        if not isinstance(d, dict): raise ValueError(f"{path}: not a JSON object")
        if is_bench(d):
            d.setdefault("source", "bench_json")
        elif "fps_mean" in d or "log" in d:
            d = bench_from_summary(d)
        else:
            raise ValueError(f"{path}: neither a bench JSON nor a summary.json")
    elif ext == ".csv":
        d = bench_from_csv(path, skip_ms)
    else:
        with open(path, errors="replace") as f: text = f.read()
        d = parse_bench_line(text)
        if d is None:
            d = bench_from_summary(parse_log(path, 0, 0, 5.0, 0))
            d.update(label="", source="log_stats")
        else:
            d["source"] = "log_bench"
    if d.get("label") == "-": d["label"] = ""   # the game's placeholder for no ISS_BENCH_LABEL
    folder = os.path.basename(os.path.dirname(os.path.abspath(path)))
    d["name"] = d.get("label") or re.sub(r"^\d+-", "", folder) or folder
    d["path"] = path
    return d


def stats_of(values):
    v = [x for x in values if x is not None]
    if not v: return None, None, 0
    return mean(v), (st.stdev(v) if len(v) > 1 else None), len(v)


def bench_table(runs, control=None, noise_k=2.0):
    """{'groups': [names, control first], 'control', 'runs': {name: n}, 'rows': [per metric]}."""
    groups = []
    for x in runs:
        if x["name"] not in groups: groups.append(x["name"])
    if control is None: control = groups[0] if groups else None
    if control not in groups: raise ValueError(f"control '{control}' is not one of {groups}")
    groups.remove(control); groups.insert(0, control)
    out = dict(groups=groups, control=control, noise_k=noise_k,
               runs={g: sum(1 for x in runs if x["name"] == g) for g in groups}, rows=[])
    for key, better in BENCH_CMP:
        cells = {}
        for g in groups:
            m, sd, n = stats_of([x.get(key) for x in runs if x["name"] == g])
            cells[g] = dict(mean=m, sd=sd, n=n)
        if all(c["n"] == 0 for c in cells.values()): continue
        c = cells[control]
        noise = (100.0 * c["sd"] / abs(c["mean"])) if (c["sd"] is not None and c["mean"]) else None
        for g in groups[1:]:
            v = cells[g]
            d = pct(v["mean"], c["mean"]) if (v["mean"] is not None and c["mean"] is not None) else None
            v["diff_pct"] = d
            if d is None and v["mean"] is not None and c["mean"] == 0:
                v["diff_abs"] = v["mean"]   # control 0 (over33 often is): the absolute difference
            # a stdev of 0 is no noise estimate: a 0% diff is not "within the noise"
            v["noise"] = bool(d is not None and noise and abs(d) <= noise_k * noise)
            v["better"] = None if (d is None or d == 0 or v["noise"]) else ((d > 0) == better)
        out["rows"].append(dict(metric=key, higher_is_better=better, noise_pct=noise, groups=cells))
    return out


def fmt_cell(c, key):
    if c["mean"] is None: return "-"
    # counts: whole numbers stay whole; a mean between two counts keeps one decimal
    nd = 0 if key in BENCH_INT and float(c["mean"]).is_integer() else 1
    s = f"{c['mean']:.{nd}f}"
    if c["sd"] is not None: s += f" +-{c['sd']:.{max(nd, 1)}f}"
    if c.get("diff_pct") is not None:
        s += f" ({c['diff_pct']:+.1f}%{' ~' if c['noise'] else ''})"
    elif "diff_abs" in c:
        s += f" ({c['diff_abs']:+g})"
    return s


def print_table(hdr, rows):
    widths = [max(len(str(x)) for x in [h] + [rw[i] for rw in rows]) for i, h in enumerate(hdr)]
    fmt = lambda rw: "  ".join(str(x).ljust(widths[i]) for i, x in enumerate(rw)).rstrip()
    print(fmt(hdr)); print(fmt(["-" * w for w in widths]))
    for rw in rows: print(fmt(rw))


def cmd_bench(a):
    runs = []
    for p in a.runs:
        try: runs.append(load_bench(p, a.skip_ms))
        except (OSError, ValueError) as e: print(f"error: {e}", file=sys.stderr); sys.exit(2)
    try: t = bench_table(runs, a.control, a.noise_k)
    except ValueError as e: print(f"error: {e}", file=sys.stderr); sys.exit(2)
    hdr = ["variant", "runs"] + [row["metric"] for row in t["rows"]]
    rows = [[g + (" (control)" if g == t["control"] else ""), str(t["runs"][g])]
            + [fmt_cell(row["groups"][g], row["metric"]) for row in t["rows"]] for g in t["groups"]]
    print_table(hdr, rows)
    print(f"\nmean +-stdev per variant; % = diff vs '{t['control']}'; '~' = within {a.noise_k:g}x the "
          "control's stdev when above 0 (likely noise)."
          + (" One control run only: no noise estimate." if t["runs"][t["control"]] < 2 else ""))
    for x in runs:
        # only the game's own summary covers the replay window alone; a log/CSV/summary.json
        # fallback counts the loading screens' compiles too, which says nothing about the cache
        if x.get("compiles") and x.get("source") in ("bench_json", "log_bench"):
            print(f"warning: {x['path']}: {x['compiles']} shader/pipeline compiles in the window (cache not warm)")
        if x.get("frames") is not None and x["frames"] < 100:
            print(f"warning: {x['path']}: only {x['frames']} frames")
        if x.get("source") == "frame_csv" and x.get("csv_window") == "bench":
            print(f"note: {x['path']}: no bench summary, read from the CSV's benchmark window")
        elif x.get("source") not in ("bench_json", "log_bench"):
            print(f"note: {x['path']}: no bench summary, read as {x['source']}"
                  + (" (whole file: no 'bench' column)" if x.get("source") == "frame_csv" else ""))
    if a.csv:
        with open(a.csv, "w", newline="") as f:
            w = csv.writer(f); w.writerow(hdr); w.writerows(rows)
        print(f"written: {a.csv}")
    if a.json:
        with open(a.json, "w") as f: json.dump(dict(t, run_files=[x["path"] for x in runs]), f, indent=2)
        print(f"written: {a.json}")


def cmd_compare(a):
    ctrl = [load_run(p) for p in a.control]
    var = [load_run(p) for p in a.variant]
    cols = [c["name"] for c in ctrl] + ["ctrl_med", "ruido%"] + [v["name"] for v in var]
    rows = []
    for key, label, better in CMP:
        cv = [c[key] for c in ctrl if c.get(key) is not None]
        if not cv and not any(v.get(key) is not None for v in var): continue
        cm = mean(cv)
        sd = st.stdev(cv) if len(cv) > 1 else None
        rng = (max(cv) - min(cv)) if len(cv) > 1 else None
        noise = pct(cm + sd, cm) if (sd is not None and cm) else None  # desvio em % da media
        cells = [c.get(key) for c in ctrl]
        row = [label] + [("-" if x is None else f"{x:g}") for x in cells]
        row += ["-" if cm is None else f"{cm:.2f}", "-" if noise is None else f"{noise:.1f}"]
        for v in var:
            x = v.get(key)
            if x is None or cm is None: row.append("-"); continue
            d = pct(x, cm)
            mark = ""
            if d is not None and noise and abs(d) <= a.noise_k * noise: mark = " ~"
            elif d is not None and cm == 0 and x == 0: d = 0.0
            row.append(f"{x:g} ({d:+.1f}%){mark}" if d is not None else f"{x:g}")
        rows.append(row)
    hdr = ["metrica"] + cols
    widths = [max(len(str(x)) for x in [h] + [rw[i] for rw in rows]) for i, h in enumerate(hdr)]
    fmt = lambda rw: "  ".join(str(x).ljust(widths[i]) for i, x in enumerate(rw))
    print(fmt(hdr)); print(fmt(["-" * w for w in widths]))
    for rw in rows: print(fmt(rw))
    print(f"\n'~' = diferenca dentro de {a.noise_k}x o desvio entre controles (provavel ruido)."
          + ("" if len(ctrl) > 1 else " Com 1 controle so nao ha estimativa de ruido."))
    for v in var:
        t = v.get("throttle_limiting")
        if t: print(f"atencao: {v['name']} teve throttle limitante: {', '.join(t)}")
    for c in ctrl:
        t = c.get("throttle_limiting")
        if t: print(f"atencao: controle {c['name']} teve throttle limitante: {', '.join(t)}")
    if a.csv:
        with open(a.csv, "w", newline="") as f:
            w = csv.writer(f); w.writerow(hdr); w.writerows(rows)
        print(f"gravado: {a.csv}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("summarize")
    s.add_argument("--log"); s.add_argument("--mon"); s.add_argument("--out"); s.add_argument("--name")
    s.add_argument("--window", type=float, default=0); s.add_argument("--skip", type=float, default=0)
    s.add_argument("--sample-secs", type=float, default=5.0); s.add_argument("--min-fps", type=float, default=0)
    c = sp.add_parser("compare")
    c.add_argument("--control", nargs="+", required=True); c.add_argument("--variant", nargs="+", required=True)
    c.add_argument("--csv"); c.add_argument("--noise-k", type=float, default=2.0)
    b = sp.add_parser("bench")
    b.add_argument("runs", nargs="+"); b.add_argument("--control")
    b.add_argument("--noise-k", type=float, default=2.0); b.add_argument("--skip-ms", type=float, default=1000.0)
    b.add_argument("--csv"); b.add_argument("--json")
    a = ap.parse_args()
    {"summarize": cmd_summarize, "compare": cmd_compare, "bench": cmd_bench}[a.cmd](a)


if __name__ == "__main__":
    main()
