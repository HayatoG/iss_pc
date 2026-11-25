"""Link tables of a KCAP package: relocations, class ids, imports and exports.

A package may hold several link tables; each one is an item whose data starts with "KNLI"
(the last item is not always one of them, and BSS items can overlap one: tables are taken
once per offset). A table is a chain of chunks {4-byte tag, u32 size, payload[size]} that ends
with " DNE":
    "KNLI" (ILNK)  i32 n, n x {i32 type, i32 count}, then the int16 position streams
    " DIC" (CID)   u64 n, n x {u64 local, u64 class hash}       local = object start
    "KNLX" (XLNK)  u64 0, u64 16, u64 n, n x {16-byte name hash, u64 local}
                   local = u64 slot that the loader fills with the address of the export
    "PYTO" (OTYP), "PLPS" (SPLP)  not used by the index (only counted)

ILNK position streams (one per relocation type). Each int16 `s`:
    s >= 0                    position = base + 4*s (relative to doff)
    s < 0 and s & 0x4000      base = (s & 0x3fff) << 17
    s < 0, no 0x4000          run: s & 0x3fff more positions, 4 bytes apart (never seen on the
                              game; decoded as in the reference script and counted)
Relocation types seen: 0 CPU pointers (u64), 1 T# (not seen), 2 V# base, 3 GPU pointers (u64,
fetch shaders), 4 GPU addresses packed in registers/descriptors (Gnm code pointer, T# base).

Exports: an item whose first u64 points 0x10 past its own start, followed by u64 nbuckets and
nbuckets x {u64 entries, u64 count}; entries are 32 bytes {u32, 16-byte hash, u32 0, u32 offset,
u32 0}. Only a few shared packages have them; they resolve the XLNK hashes of the others.
"""
from __future__ import annotations

import struct
from array import array
from collections import Counter, defaultdict
from dataclasses import dataclass, field

TAG_ILNK = b"KNLI"
TAG_CID = b" DIC"
TAG_XLNK = b"KNLX"
TAG_OTYP = b"PYTO"
TAG_SPLP = b"PLPS"
TAG_END = b" DNE"
LINK_ITEM_TYPE = 2
TAGS = {TAG_ILNK: "ILNK", TAG_CID: "CID", TAG_XLNK: "XLNK", TAG_OTYP: "OTYP",
        TAG_SPLP: "SPLP", TAG_END: "END"}

RELOC_CPU = 0
RELOC_TSHARP = 1
RELOC_VSHARP = 2
RELOC_GPU_PTR = 3
RELOC_GPU_ADDR = 4

EXPORT_ENTRY = 32
EXPORT_MAX_BUCKETS = 1 << 20


@dataclass
class LinkTable:
    relocs: dict = field(default_factory=lambda: defaultdict(set))   # type -> set of positions
    cid: dict = field(default_factory=dict)                         # local -> class hash
    imports: dict = field(default_factory=dict)                     # local -> 16-byte hash
    tables: int = 0
    stats: Counter = field(default_factory=Counter)

    def positions(self, rtype: int) -> set:
        return self.relocs.get(rtype, set())


def decode_positions(values, stats: Counter = None) -> list:
    """Positions (relative to doff) of one ILNK stream of int16 values."""
    out = []
    base = 0
    cur = 0
    for s in values:
        if s >= 0:
            cur = base + 4 * s
            out.append(cur)
        elif s & 0x4000:
            base = (s & 0x3FFF) << 17
        else:
            if stats is not None:
                stats["ilnk_runs"] += 1
            for _ in range(s & 0x3FFF):
                cur += 4
                out.append(cur)
    return out


def _int16s(buf, pos: int, count: int) -> array:
    a = array("h")
    a.frombytes(bytes(buf[pos:pos + 2 * count]))
    if array("h", [1]).tobytes() != b"\x01\x00":   # big-endian host
        a.byteswap()
    return a


def parse_table(buf, pos: int, end: int, lt: LinkTable) -> int:
    """Parse one KNLI chain at absolute `pos`; returns the absolute end position."""
    while pos + 8 <= end:
        tag = bytes(buf[pos:pos + 4])
        (size,) = struct.unpack_from("<I", buf, pos + 4)
        p = pos + 8
        if tag not in TAGS:
            lt.stats["bad_tag"] += 1
            return pos
        if tag == TAG_END:
            return p
        if p + size > end:
            lt.stats["chunk_past_end"] += 1
            return pos
        lt.stats["chunk_" + TAGS[tag]] += 1
        if tag == TAG_ILNK:
            (n,) = struct.unpack_from("<i", buf, p)
            heads = [struct.unpack_from("<ii", buf, p + 4 + 8 * i) for i in range(max(0, n))]
            sp = p + 4 + 8 * len(heads)
            for rtype, count in heads:
                lt.relocs[rtype].update(decode_positions(_int16s(buf, sp, count), lt.stats))
                sp += 2 * count
        elif tag == TAG_CID:
            (n,) = struct.unpack_from("<Q", buf, p)
            n = min(n, (size - 8) // 16)
            for i in range(n):
                local, cls = struct.unpack_from("<QQ", buf, p + 8 + 16 * i)
                lt.cid.setdefault(local, cls)
        elif tag == TAG_XLNK:
            _a, _b, n = struct.unpack_from("<3Q", buf, p)
            n = min(n, (size - 24) // 24)
            for i in range(n):
                e = p + 24 + 24 * i
                h = bytes(buf[e:e + 16])
                # {hash16, u32 local, i32 addend} (FUN_0130a700, all.c l.692753): slot = export + addend
                local, addend = struct.unpack_from("<Ii", buf, e + 16)
                if addend:
                    lt.stats["xlnk_addend"] += 1
                lt.imports.setdefault(local, h)
        pos = p + size
    lt.stats["no_end"] += 1
    return pos


def read_links(pack) -> LinkTable:
    """All link tables of an open `kcap.Pack`."""
    lt = LinkTable()
    ends = {}   # several items (BSS ones included) can start at the same offset: use the largest
    for it in pack.k.items:
        # the loader (FUN_0130fc00 -> FUN_0130a310, all.c l.695800/692853) walks the chunk chain
        # of every link item whatever its first tag: in 54 packages the table is split into a
        # KNLI-only item and items that start with KNLX, " DIC", PYTO or PLPS
        if not pack.valid(it.off, 8):
            continue
        tag = pack.read(it.off, 4)
        if tag == TAG_ILNK or (it.type == LINK_ITEM_TYPE and tag in TAGS and tag != TAG_END):
            ends[it.off] = max(ends.get(it.off, 0), it.end)
    for off in sorted(ends):
        parse_table(pack.mm, pack.doff + off, pack.doff + min(ends[off], pack.size), lt)
        lt.tables += 1
    for rtype, pos in lt.relocs.items():   # a relocation must leave room for its u64
        bad = {x for x in pos if not pack.valid(x, 8)}
        if bad:
            lt.stats["reloc_out_of_data"] += len(bad)
            pos.difference_update(bad)
    return lt


def read_exports(pack) -> list:
    """(16-byte hash, offset) of every export hash table in the package, in table order."""
    out = []
    for it in pack.k.items:
        if it.size < 0x20 or not pack.valid(it.off, 16):
            continue
        if pack.u64(it.off) != it.off + 0x10:
            continue
        nb = pack.u64(it.off + 8)
        if not 0 < nb <= EXPORT_MAX_BUCKETS or not pack.valid(it.off + 0x10, 16 * nb):
            continue
        entries = []
        ok = True
        for b in range(nb):
            ep, en = struct.unpack_from("<QQ", pack.mm, pack.doff + it.off + 0x10 + 16 * b)
            if en == 0:
                continue
            if not (it.off <= ep and ep + EXPORT_ENTRY * en <= it.end and pack.valid(ep, EXPORT_ENTRY * en)):
                ok = False
                break
            for j in range(en):
                e = ep + EXPORT_ENTRY * j
                h = pack.read(e + 4, 16)
                z0, off, z1 = struct.unpack_from("<III", pack.mm, pack.doff + e + 20)
                if z0 or z1:
                    ok = False
                    break
                entries.append((h, off))
            if not ok:
                break
        if ok and entries:
            out.extend(entries)
    return out
