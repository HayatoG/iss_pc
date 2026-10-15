#!/usr/bin/env python3
"""Index of the OrbShdr shaders inside the inFAMOUS Second Son dump.

The tool only reads the dump (streaming, 64 MiB blocks) and stores hash, file, offset,
size and stage. No game bytes are copied: `lookup --dump` writes a shader to stdout only
when asked, nothing is kept.

OrbShdr block (AmdGpu::BinaryInfo in video_core/amdgpu/regs_shader.h, 28 bytes). It sits
right after the code it describes, so the code is [header_offset - length, header_offset):
    0..6   "OrbShdr"            7  version
    8..11  u32 bit0 pssl_or_cg, bit1 cached, bits2-5 stage type, bits6-7 source, bits8-31 length
    12..15 usage info          16..23 u64 shader_hash (the key the port uses)   24..27 crc32
A block is accepted only when length != 0, length % 4 == 0 and length <= header_offset
(the same test as BinaryInfoMatchesCode); anything else counts as "rejected".

Index (SQLite, default ~/decomp/iss_ref/shader_index/shader_index.sqlite):
    files(id, path)            path relative to the scanned root
    occ(hash, file_id, offset, size, stage, type)
                               every occurrence; offset is where the CODE starts; hash is the
                               u64 stored as signed int64 (use `lookup`, not hand-written SQL)
    shaders(hash PK, file_id, offset, size, stage, type, n_occ, n_variants)
                               one canonical row per hash (first occurrence in path order);
                               n_variants > 1 means the same hash appeared with several sizes
    meta(key, value)           root, counts, scan time

Usage:
    shader_index.py build  [--root DIR] [--out DB] [--limit N] [--include-psarc]
    shader_index.py lookup HASH [--dump] [--db DB] [--root DIR]
    shader_index.py verify [--db DB] [--caches DIR ...]
    shader_index.py stats  [--db DB]
"""
from __future__ import annotations

import argparse
import glob
import os
import re
import sqlite3
import struct
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterator, Optional, Sequence

HOME = Path.home()
DEFAULT_ROOT = HOME / "decomp/infamous_pc/game/CUSA00223/art/cache"
DEFAULT_DB = HOME / "decomp/iss_ref/shader_index/shader_index.sqlite"
DEFAULT_CACHES = (
    str(HOME / "decomp/infamous_pc*/port/user/cache"),
    str(HOME / "decomp/infamous_pc*/port/user/shader"),
)

# OrbShdr layout (see module docstring).
SIGNATURE = b"OrbShdr"
HEADER_SIZE = 28
FLAGS_OFFSET = 8
HASH_OFFSET = 16
TYPE_SHIFT, TYPE_MASK = 2, 0xF
LENGTH_SHIFT = 8

BLOCK_SIZE = 64 << 20
# A header may straddle two blocks: read this much past the block end.
BLOCK_OVERLAP = HEADER_SIZE - 1
PROGRESS_EVERY = 20  # files

# Names for the 4-bit type field. 0/1/4 are confirmed on this dump by the export targets in the
# code (MRT exports = PS, pos+param exports = VS, no exports = CS); 2 is an assumption. Other
# values print as T<n>.
STAGE_NAMES = {0: "PS", 1: "VS", 2: "GS", 4: "CS"}

SQLITE_INT64_LIMIT = 1 << 63
HASH_BITS = 1 << 64
SPV_NAME = re.compile(r"0x([0-9a-fA-F]{16})_[^/]*\.spv$")


class ShaderIndexError(Exception):
    """Fatal, user-facing problem (missing index, bad hash text, unreadable file)."""


@dataclass(frozen=True)
class ShaderRef:
    """One OrbShdr found in a file; offset points at the start of the code."""
    hash: int
    offset: int
    size: int
    type: int

    @property
    def stage(self) -> str:
        return stage_name(self.type)


@dataclass
class ScanCounts:
    headers: int = 0
    rejected: int = 0   # signature present but fields inconsistent with the code span
    truncated: int = 0  # signature within HEADER_SIZE bytes of EOF

    def add(self, other: "ScanCounts") -> None:
        self.headers += other.headers
        self.rejected += other.rejected
        self.truncated += other.truncated


def stage_name(type_id: int) -> str:
    return STAGE_NAMES.get(type_id, f"T{type_id}")


def to_signed(value: int) -> int:
    """SQLite integers are signed 64-bit; hashes are unsigned."""
    return value - HASH_BITS if value >= SQLITE_INT64_LIMIT else value


def from_signed(value: int) -> int:
    return value + HASH_BITS if value < 0 else value


def parse_header(buf: bytes, pos: int, file_offset: int) -> Optional[ShaderRef]:
    """Decode the OrbShdr at buf[pos:]; None when it fails the BinaryInfoMatchesCode test."""
    flags = struct.unpack_from("<I", buf, pos + FLAGS_OFFSET)[0]
    length = flags >> LENGTH_SHIFT
    if length == 0 or length % 4 or length > file_offset:
        return None
    (shader_hash,) = struct.unpack_from("<Q", buf, pos + HASH_OFFSET)
    return ShaderRef(shader_hash, file_offset - length, length, (flags >> TYPE_SHIFT) & TYPE_MASK)


def scan_stream(f, size: int, counts: ScanCounts) -> Iterator[ShaderRef]:
    """Yield every valid OrbShdr of an open binary file, reading one block at a time."""
    for start in range(0, size, BLOCK_SIZE):
        f.seek(start)
        buf = f.read(BLOCK_SIZE + BLOCK_OVERLAP)
        i = buf.find(SIGNATURE)
        # Headers that start at or after BLOCK_SIZE belong to the next block.
        while 0 <= i < BLOCK_SIZE:
            if i + HEADER_SIZE > len(buf):
                counts.truncated += 1
            else:
                ref = parse_header(buf, i, start + i)
                if ref:
                    counts.headers += 1
                    yield ref
                else:
                    counts.rejected += 1
            i = buf.find(SIGNATURE, i + 1)


def scan_file(path: Path, counts: ScanCounts) -> Iterator[ShaderRef]:
    try:
        with open(path, "rb") as f:
            yield from scan_stream(f, os.fstat(f.fileno()).st_size, counts)
    except OSError as exc:
        raise ShaderIndexError(f"cannot read {path}: {exc}") from exc


def list_dump_files(root: Path, include_psarc: bool) -> list[Path]:
    suffixes = (".xpps", ".psarc") if include_psarc else (".xpps",)
    return sorted(p for p in root.rglob("*") if p.is_file() and p.suffix in suffixes)


SCHEMA = """
CREATE TABLE files(id INTEGER PRIMARY KEY, path TEXT NOT NULL);
CREATE TABLE occ(hash INTEGER, file_id INTEGER, offset INTEGER, size INTEGER,
                 stage TEXT, type INTEGER);
CREATE TABLE meta(key TEXT PRIMARY KEY, value TEXT);
"""
CANONICAL = """
CREATE INDEX occ_hash ON occ(hash);
CREATE TABLE shaders AS
  SELECT o.hash, o.file_id, o.offset, o.size, o.stage, o.type, c.n_occ, c.n_variants
  FROM occ o JOIN (SELECT MIN(rowid) AS first, hash, COUNT(*) AS n_occ,
                          COUNT(DISTINCT size) AS n_variants
                   FROM occ GROUP BY hash) c ON o.rowid = c.first;
CREATE UNIQUE INDEX shaders_hash ON shaders(hash);
"""


def log(msg: str) -> None:
    print(msg, file=sys.stderr, flush=True)


def build_index(root: Path, out: Path, limit: int, include_psarc: bool) -> None:
    if not root.is_dir():
        raise ShaderIndexError(f"dump root not found: {root}")
    files = list_dump_files(root, include_psarc)
    if limit:
        files = files[:limit]
    if not files:
        raise ShaderIndexError(f"no .xpps files under {root}")
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix(out.suffix + ".tmp")  # replaced atomically, a crash keeps the old index
    tmp.unlink(missing_ok=True)
    start = time.time()
    counts, scanned = ScanCounts(), 0
    db = sqlite3.connect(tmp)
    try:
        db.executescript("PRAGMA journal_mode=OFF; PRAGMA synchronous=OFF;" + SCHEMA)
        for n, path in enumerate(files):
            db.execute("INSERT INTO files VALUES(?,?)", (n, str(path.relative_to(root))))
            rows = [(to_signed(r.hash), n, r.offset, r.size, r.stage, r.type)
                    for r in scan_file(path, counts)]
            db.executemany("INSERT INTO occ VALUES(?,?,?,?,?,?)", rows)
            scanned += path.stat().st_size
            if n % PROGRESS_EVERY == 0 or n == len(files) - 1:
                log(f"[{n + 1}/{len(files)}] {path.name}: {counts.headers} headers, "
                    f"{scanned / 2**30:.1f} GiB, {time.time() - start:.0f}s")
        db.executescript(CANONICAL)
        meta = {"root": root, "files": len(files), "headers": counts.headers,
                "rejected": counts.rejected, "truncated": counts.truncated,
                "scanned_bytes": scanned, "scan_seconds": round(time.time() - start, 1),
                "scan_time": time.strftime("%F %T")}
        db.executemany("INSERT INTO meta VALUES(?,?)", [(k, str(v)) for k, v in meta.items()])
        db.commit()
        db.execute("VACUUM")
    finally:
        db.close()
    os.replace(tmp, out)
    log(f"done in {time.time() - start:.0f}s -> {out}")


def open_index(path: Path) -> sqlite3.Connection:
    if not path.is_file():
        raise ShaderIndexError(f"index not found: {path} (run `build` first)")
    return sqlite3.connect(path)


def parse_hash(text: str) -> int:
    """Hex with 0x prefix or any a-f digit, otherwise decimal."""
    t = text.strip().lower()
    try:
        value = int(t, 16) if t.startswith("0x") or re.search("[a-f]", t) else int(t)
    except ValueError as exc:
        raise ShaderIndexError(f"not a hash: {text!r}") from exc
    if not 0 <= value < HASH_BITS:
        raise ShaderIndexError(f"hash out of 64-bit range: {text!r}")
    return value


def find_shader(db: sqlite3.Connection, shader_hash: int):
    return db.execute(
        "SELECT f.path, s.offset, s.size, s.stage, s.n_occ, s.n_variants FROM shaders s "
        "JOIN files f ON f.id = s.file_id WHERE s.hash = ?", (to_signed(shader_hash),)).fetchone()


def read_shader(root: Path, path: str, offset: int, size: int) -> bytes:
    full = root / path
    try:
        with open(full, "rb") as f:
            f.seek(offset)
            data = f.read(size)
    except OSError as exc:
        raise ShaderIndexError(f"cannot read {full}: {exc}") from exc
    if len(data) != size:
        raise ShaderIndexError(f"{full} is shorter than the index says (dump changed?)")
    return data


def cache_hashes(patterns: Sequence[str]) -> dict[int, str]:
    """Program hashes named by the .spv files of the port caches (0x<hash>_<perm>.spv)."""
    found: dict[int, str] = {}
    for pattern in patterns:
        for directory in glob.glob(pattern):
            for dirpath, _, names in os.walk(directory):
                for name in names:
                    m = SPV_NAME.search(name)
                    if m:
                        found.setdefault(int(m.group(1), 16), os.path.join(dirpath, name))
    return found


# --- CLI ---------------------------------------------------------------------------------

def cmd_build(a: argparse.Namespace) -> int:
    build_index(a.root, a.out, a.limit, a.include_psarc)
    a.db = a.out
    return cmd_stats(a)


def cmd_stats(a: argparse.Namespace) -> int:
    db = open_index(a.db)
    meta = dict(db.execute("SELECT key, value FROM meta"))
    print(f"scanned {meta.get('files')} files, {meta.get('headers')} headers "
          f"({meta.get('rejected')} rejected, {meta.get('truncated')} truncated), "
          f"{meta.get('scan_seconds')}s")
    print("unique shaders:", db.execute("SELECT COUNT(*) FROM shaders").fetchone()[0])
    for stage, n in db.execute("SELECT stage, COUNT(*) FROM shaders GROUP BY stage ORDER BY 2 DESC"):
        print(f"  {stage:<4} {n}")
    print("hashes seen with several sizes:",
          db.execute("SELECT COUNT(*) FROM shaders WHERE n_variants > 1").fetchone()[0])
    print(f"index size: {a.db.stat().st_size / 2**20:.1f} MiB")
    return 0


def cmd_lookup(a: argparse.Namespace) -> int:
    db = open_index(a.db)
    shader_hash = parse_hash(a.hash)
    row = find_shader(db, shader_hash)
    if not row:
        log(f"not found: 0x{shader_hash:016x}")
        return 1
    path, offset, size, stage, n_occ, n_variants = row
    line = (f"hash=0x{shader_hash:016x} file={path} offset={offset} size={size} "
            f"stage={stage} occurrences={n_occ} variants={n_variants}")
    if not a.dump:
        print(line)
        return 0
    log(line)
    root = a.root or Path(dict(db.execute("SELECT key, value FROM meta"))["root"])
    sys.stdout.buffer.write(read_shader(root, path, offset, size))
    return 0


def cmd_verify(a: argparse.Namespace) -> int:
    db = open_index(a.db)
    known = {from_signed(r[0]) for r in db.execute("SELECT hash FROM shaders")}
    wanted = cache_hashes(a.caches or DEFAULT_CACHES)
    missing = sorted(h for h in wanted if h not in known)
    covered = len(wanted) - len(missing)
    print(f"cache programs: {len(wanted)}  in index: {covered}  "
          f"coverage: {100.0 * covered / max(1, len(wanted)):.2f}%")
    for h in missing[:20]:
        print(f"  missing 0x{h:016x}  {wanted[h]}")
    return 0 if not missing else 2


def make_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="Index of OrbShdr shaders in the game dump.")
    sub = p.add_subparsers(dest="cmd", required=True)

    b = sub.add_parser("build", help="scan the dump and write the index")
    b.add_argument("--root", type=Path, default=DEFAULT_ROOT)
    b.add_argument("--out", type=Path, default=DEFAULT_DB)
    b.add_argument("--limit", type=int, default=0, help="only the first N files (quick test)")
    b.add_argument("--include-psarc", action="store_true")
    b.set_defaults(run=cmd_build)

    lk = sub.add_parser("lookup", help="where is this shader hash?")
    lk.add_argument("hash")
    lk.add_argument("--dump", action="store_true", help="write the shader bytes to stdout")
    lk.add_argument("--db", type=Path, default=DEFAULT_DB)
    lk.add_argument("--root", type=Path, help="dump root (default: the one stored in the index)")
    lk.set_defaults(run=cmd_lookup)

    v = sub.add_parser("verify", help="coverage of the hashes in the port caches")
    v.add_argument("--db", type=Path, default=DEFAULT_DB)
    v.add_argument("--caches", nargs="*", help="glob(s) of cache dirs")
    v.set_defaults(run=cmd_verify)

    s = sub.add_parser("stats", help="counts per stage and index size")
    s.add_argument("--db", type=Path, default=DEFAULT_DB)
    s.set_defaults(run=cmd_stats)
    return p


def lower_priority() -> None:
    """Moderate CPU and IO: the scan reads 15+ GiB while the machine is in use."""
    try:
        os.nice(10)
        os.system(f"ionice -c3 -p {os.getpid()} >/dev/null 2>&1")
    except OSError:
        pass


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = make_parser().parse_args(argv)
    lower_priority()
    try:
        return args.run(args)
    except ShaderIndexError as exc:
        log(f"error: {exc}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
