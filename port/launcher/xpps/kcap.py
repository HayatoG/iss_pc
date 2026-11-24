"""KCAP container (.xpps): header, sections, items and BSS.

Layout (all little-endian; offsets are file offsets unless noted):
    0x00  "KCAP"
    0x04  u16 version (13 on every package seen), u16 variant
    0x08  u32 flags
    0x18  u32 table offset (0x98), 0x1C u32 table size
    0x28  u32 data offset (`doff`), 0x2C u32 data size (doff + size = file size when the
          package is complete)
    0x78  16-byte content hash
Table (at the table offset):
    6 x u64 {0, nsec, 0, nitem, 0, extra}; `extra` = 1 adds 16 bytes after the items
    nsec x 0x28  section {u32 flags, u32 size, u32 off, u32 pad, u64 pad, u64 first_item, u64 count}
    nitem x 12   item {u32 size, u32 off, u32 flags}; off is relative to doff; flags bits 16+ are
                 the item type, bits 0-15 the alignment (0x10 = CPU structures, 0x100 = GPU memory)
Every pointer stored inside the data is an offset relative to doff (u64, only 4-byte aligned).
Items may run past the end of the data: that part is BSS (zero-filled memory at load time).
"""
from __future__ import annotations

import mmap
import os
import struct
from dataclasses import dataclass, field
from typing import Iterator, Optional

MAGIC = b"KCAP"
HEADER_SIZE = 0x98
TABLE_HEAD = struct.Struct("<6Q")
SECTION = struct.Struct("<IIIIQQQ")
ITEM = struct.Struct("<III")
TYPE_SHIFT = 16
ALIGN_MASK = 0xFFFF
GPU_ALIGN = 0x100


class KcapError(ValueError):
    """The file is not a KCAP package or its table is inconsistent."""


@dataclass(frozen=True)
class Section:
    flags: int
    size: int
    off: int
    first_item: int
    count: int


@dataclass(frozen=True)
class Item:
    index: int
    size: int
    off: int      # relative to doff
    flags: int

    @property
    def type(self) -> int:
        return self.flags >> TYPE_SHIFT

    @property
    def align(self) -> int:
        return self.flags & ALIGN_MASK

    @property
    def end(self) -> int:
        return self.off + self.size


@dataclass
class Kcap:
    version: int
    variant: int
    flags: int
    table_off: int
    table_size: int
    doff: int
    dsize: int
    content_hash: bytes
    sections: list = field(default_factory=list)
    items: list = field(default_factory=list)
    table_extra: int = 0

    @property
    def data_end(self) -> int:
        """End of the stored data, relative to doff (BSS starts here)."""
        return self.dsize

    def bss_items(self) -> list:
        return [it for it in self.items if it.end > self.dsize]

    def bss_bytes(self) -> int:
        return sum(it.end - max(it.off, self.dsize) for it in self.bss_items())

    def item_at(self, rel: int) -> Optional[Item]:
        """Item that contains the data offset `rel` (first match in table order)."""
        for it in self.items:
            if it.off <= rel < it.end:
                return it
        return None


def parse(buf, size: Optional[int] = None) -> Kcap:
    """Parse the header and table of a KCAP held in `buf` (bytes, bytearray or mmap)."""
    size = len(buf) if size is None else size
    if size < HEADER_SIZE or bytes(buf[0:4]) != MAGIC:
        raise KcapError("not a KCAP package")
    version, variant = struct.unpack_from("<HH", buf, 4)
    (flags,) = struct.unpack_from("<I", buf, 8)
    table_off, table_size = struct.unpack_from("<II", buf, 0x18)
    doff, dsize = struct.unpack_from("<II", buf, 0x28)
    content_hash = bytes(buf[0x78:0x88])
    if table_off + TABLE_HEAD.size > size or doff > size:
        raise KcapError("table or data offset past the end of the file")
    _, nsec, _, nitem, _, extra = TABLE_HEAD.unpack_from(buf, table_off)
    pos = table_off + TABLE_HEAD.size
    if pos + nsec * SECTION.size + nitem * ITEM.size > size:
        raise KcapError("table runs past the end of the file")
    k = Kcap(version, variant, flags, table_off, table_size, doff, dsize, content_hash,
             table_extra=extra)
    for _ in range(nsec):
        fl, sz, off, _p0, _p1, first, cnt = SECTION.unpack_from(buf, pos)
        k.sections.append(Section(fl, sz, off, first, cnt))
        pos += SECTION.size
    for i in range(nitem):
        sz, off, fl = ITEM.unpack_from(buf, pos)
        k.items.append(Item(i, sz, off, fl))
        pos += ITEM.size
    return k


class Pack:
    """A package opened read-only with mmap; offsets of the helpers are relative to doff."""

    def __init__(self, path: str):
        self.path = path
        self._f = open(path, "rb")
        try:
            self.file_size = os.fstat(self._f.fileno()).st_size
            self.mm = mmap.mmap(self._f.fileno(), 0, access=mmap.ACCESS_READ) if self.file_size else b""
            self.k = parse(self.mm, self.file_size)
        except Exception:
            self.close()
            raise
        self.doff = self.k.doff
        # Readable data (BSS and truncated tails read as missing).
        self.size = min(self.k.dsize, self.file_size - self.doff)

    def close(self) -> None:
        mm = getattr(self, "mm", None)
        if isinstance(mm, mmap.mmap):
            try:
                mm.close()
            except BufferError:   # a memoryview is still alive; the GC will release it
                pass
        self._f.close()

    def __enter__(self) -> "Pack":
        return self

    def __exit__(self, *exc) -> None:
        self.close()

    def valid(self, rel: int, n: int = 1) -> bool:
        return 0 <= rel and rel + n <= self.size

    def u32(self, rel: int) -> int:
        return struct.unpack_from("<I", self.mm, self.doff + rel)[0]

    def u64(self, rel: int) -> int:
        return struct.unpack_from("<Q", self.mm, self.doff + rel)[0]

    def read(self, rel: int, n: int) -> bytes:
        if rel < 0:
            return b""
        end = min(rel + n, self.size)
        return bytes(self.mm[self.doff + rel:self.doff + end]) if end > rel else b""


def iter_packages(root: str) -> Iterator[str]:
    """Sorted relative paths of the .xpps files below `root` (symlinks followed)."""
    found = []
    for dirpath, _, names in os.walk(root, followlinks=True):
        for name in names:
            if name.endswith(".xpps"):
                found.append(os.path.relpath(os.path.join(dirpath, name), root).replace(os.sep, "/"))
    return iter(sorted(found))
