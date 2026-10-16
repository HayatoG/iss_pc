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
fica dentro do ruido.
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
       ("ram_used_max_mib", "RAM usada max MiB", False)]


def load_run(p):
    with open(p) as f: d = json.load(f)
    d.setdefault("name", os.path.basename(os.path.dirname(os.path.abspath(p))))
    return d


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
            if d is not None and noise is not None and abs(d) <= a.noise_k * noise: mark = " ~"
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
    a = ap.parse_args()
    (cmd_summarize if a.cmd == "summarize" else cmd_compare)(a)


if __name__ == "__main__":
    main()
