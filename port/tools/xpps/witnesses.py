"""Observed witnesses: the resource descriptors the game really binds, recorded while playing.

    python -m xpps.witnesses merge <witnesses.jsonl ...> -o <table.json>

The job generator (jobs.py) makes its records from the game files alone, so the descriptors of
the engine's buffers come from a model (or a fallback of stride 16): the precompile then builds
shader permutations the game never asks for, and the game compiles its own ones while playing.
Renderer v2 with ISS_RECORD_WITNESSES=1 (gpu_v2/gpu/witness_record.inl) writes one JSON line
per graphics pipeline it compiled in play:
    {"vs": "0x..", "ps": "0x..", "VS": [[flat dword, value], ...], "PS": [...],
     "vb": [[user data register, dword offset, V# dword 0..3], ...]}
with every address replaced by the placeholders of jobs.words_of. `merge` checks that and groups
the lines by shader pair into the table jobs.py reads (DATA_DIR/<serial>-witnesses.json):
    {"format": 1, "pairs": {"<vs>:<ps>": [{"VS": {dword: value}, "PS": {...},
                                           "vb": [[sgpr, offset, w0, w1, w2, w3]]}, ...]}}
For a pair in the table, jobs.py writes a copy of each of its records per observed variant, with
those dwords and vertex V# in place of the modelled ones.
"""
from __future__ import annotations

import argparse
import copy
import json
import os
import sys
from typing import Dict, Iterable, List, Optional

FORMAT = 1
DATA_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
# Copies of one record per observed variant, at most (a shader pair with many material states).
MAX_VARIANTS = 32


def table_path(serial: str = "CUSA00223") -> str:
    return os.path.join(DATA_DIR, f"{serial}-witnesses.json")


# V# dword 1: bits 0-7 are address bits 32-39, bits 8-15 padding (AmdGpu::Buffer::_padding0)
V_ADDRESS_HI, V_PADDING = 0xFF, 0xFF00


def _clean_vsharp(words) -> bool:
    """A vertex V# as the recorder writes it: placeholder address, size bound/unbound only."""
    return words[0] in (0, 0x100) and words[1] & V_ADDRESS_HI == 0 and words[2] in (0, 1)


def parse_line(line: str) -> Optional[tuple]:
    """(pair key, variant) of one recorded line, None for a malformed or unclean one."""
    try:
        d = json.loads(line)
        vs, ps = int(d["vs"], 16), int(d["ps"], 16)
        stages = {}
        for name in ("VS", "PS"):
            stages[name] = {int(i): int(v) & 0xFFFFFFFF for i, v in d[name]}
        vb = [[int(x) & 0xFFFFFFFF for x in a] for a in d["vb"]]
        for a in vb:
            if len(a) == 6:
                a[3] &= ~V_PADDING & 0xFFFFFFFF  # lines of recorders that kept the padding
    except (ValueError, KeyError, TypeError):
        return None
    if any(len(a) != 6 or not _clean_vsharp(a[2:]) for a in vb):
        return None
    return f"{vs:#x}:{ps:#x}", {"VS": stages["VS"], "PS": stages["PS"], "vb": vb}


def _freeze(variant: dict) -> str:
    return json.dumps({"VS": sorted(variant["VS"].items()), "PS": sorted(variant["PS"].items()),
                       "vb": variant["vb"]}, sort_keys=True)


def merge(lines: Iterable[str], table: Optional[dict] = None) -> tuple:
    """(table, stats): the variants of every line added to `table` (or a new one), once each."""
    table = table or {"format": FORMAT, "pairs": {}}
    pairs = table["pairs"]
    seen = {key: {_freeze(_load_variant(v)) for v in vs} for key, vs in pairs.items()}
    stats = {"lines": 0, "rejected": 0, "added": 0}
    for line in lines:
        if not line.strip():
            continue
        stats["lines"] += 1
        parsed = parse_line(line)
        if parsed is None:
            stats["rejected"] += 1
            continue
        key, variant = parsed
        frozen = _freeze(variant)
        if frozen in seen.setdefault(key, set()):
            continue
        seen[key].add(frozen)
        pairs.setdefault(key, []).append(_dump_variant(variant))
        stats["added"] += 1
    return table, stats


def _dump_variant(v: dict) -> dict:
    return {"VS": {str(i): x for i, x in sorted(v["VS"].items())},
            "PS": {str(i): x for i, x in sorted(v["PS"].items())}, "vb": v["vb"]}


def _load_variant(v: dict) -> dict:
    return {"VS": {int(i): x for i, x in v["VS"].items()},
            "PS": {int(i): x for i, x in v["PS"].items()}, "vb": v["vb"]}


def load(path: Optional[str] = None) -> Dict[tuple, List[dict]]:
    """{(vs, ps): [variant]} of the table, empty when there is none (or it is unreadable)."""
    path = path or table_path()
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
    except (OSError, ValueError):
        return {}
    if data.get("format") != FORMAT:
        return {}
    out = {}
    for key, variants in data.get("pairs", {}).items():
        vs, ps = (int(x, 16) for x in key.split(":"))
        out[(vs, ps)] = [_load_variant(v) for v in variants[:MAX_VARIANTS]]
    return out


def apply(rec: dict, variant: dict, sw_ps: int, sw_vs: int) -> dict:
    """A copy of a graphics record with an observed variant's dwords and vertex V#."""
    out = copy.deepcopy(rec)
    for st in out["stages"]:
        words = variant["PS"] if st["sw"] == sw_ps else variant["VS"] if st["sw"] == sw_vs else {}
        if words:
            flat = st["flat"]
            top = max(words) + 1
            if top > len(flat):
                flat.extend([0] * (top - len(flat)))
            for i, v in words.items():
                flat[i] = v
            st["ud"] = flat[:16]
        if st["sw"] == sw_vs and st.get("vsharps") and variant["vb"]:
            by_load = {(a[0], a[1]): tuple(a[2:]) for a in variant["vb"]}
            st["vsharps"] = [(sg, off, by_load.get((sg, off), w)) for sg, off, w in st["vsharps"]]
    return out


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(prog="python -m xpps.witnesses", description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    m = sub.add_parser("merge", help="add recorded lines to a witness table")
    m.add_argument("inputs", nargs="+", help="<serial>-witnesses.jsonl files (renderer v2)")
    m.add_argument("-o", "--out", required=True, help="table to write (updated when it exists)")
    a = ap.parse_args(argv)
    table = None
    if os.path.exists(a.out):
        with open(a.out, encoding="utf-8") as f:
            table = json.load(f)
    totals = {"lines": 0, "rejected": 0, "added": 0}
    for path in a.inputs:
        with open(path, encoding="utf-8") as f:
            table, stats = merge(f, table)
        for k in totals:
            totals[k] += stats[k]
    tmp = a.out + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(table, f, sort_keys=True, separators=(",", ":"))
    os.replace(tmp, a.out)
    print(json.dumps({**totals, "pairs": len(table["pairs"])}, sort_keys=True))
    return 0


if __name__ == "__main__":
    sys.exit(main())
