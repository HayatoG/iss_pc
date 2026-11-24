# SPDX-License-Identifier: GPL-2.0-or-later
"""The player's own add-ons (DLC): their entitlement labels, for ISS_ADDCONT_LABELS.

inFAMOUS Second Son's add-ons (costumes, Cole's Legacy) are inside the base game; a license
unlocks them, so the game only asks sceAppContentGetAddcontInfo whether a label is installed
(port/src/runtime_infamous.c). The labels come from what the player puts in <data folder>/dlc:
add-on packages (.pkg, only their header is read) or dumped add-on folders (sce_sys/param.sfo).
A content id is "UP9000-CUSA00223_00-" followed by the 16-character label. Nothing is copied or
changed. As in the Castlesea inFAMOUS port (launcher.cpp ScanDlcLabels, 8e167dc)."""
from pathlib import Path
from struct import unpack_from

PKG_MAGIC = b'\x7fCNT'
CONTENT_ID_AT = 0x40
CONTENT_ID_SIZE = 36
MAX_DEPTH = 4


def label_of(content_id):
    """The 16-character entitlement label of a content id, or None."""
    if len(content_id) != CONTENT_ID_SIZE or content_id[6] != '-' or content_id[19] != '-':
        return None
    label = content_id[20:]
    return label if label.isascii() and label.isalnum() else None


def pkg_content_id(path):
    try:
        with open(path, 'rb') as f:
            header = f.read(CONTENT_ID_AT + CONTENT_ID_SIZE)
    except OSError:
        return None
    if len(header) < CONTENT_ID_AT + CONTENT_ID_SIZE or not header.startswith(PKG_MAGIC):
        return None
    return header[CONTENT_ID_AT:].decode('ascii', 'replace')


def sfo_content_id(path):
    try:
        data = Path(path).read_bytes()
        magic, _, keys, values, count = unpack_from('<4sIIII', data, 0)
        if magic != b'\0PSF':
            return None
        for i in range(count):
            key, _, size, _, offset = unpack_from('<HHIII', data, 20 + i * 16)
            if data[keys + key:data.index(0, keys + key)] == b'CONTENT_ID':
                return data[values + offset:values + offset + size].rstrip(b'\0').decode('ascii', 'replace')
    except Exception:  # missing or damaged file: no label
        return None
    return None


def found(folder):
    """[{'label', 'source'}] of the add-ons in folder, one per label (sorted by label); source is the
    package or folder it came from, relative to folder. [] without the folder."""
    seen = {}
    root = Path(folder)

    def name(path):
        try:
            return str(path.relative_to(root)) or path.name
        except ValueError:
            return path.name

    def visit(path, depth):
        try:
            entries = list(path.iterdir())
        except OSError:
            return
        sfo = path / 'sce_sys' / 'param.sfo'
        if sfo.is_file():
            label = label_of(sfo_content_id(sfo) or '')
            if label:
                seen.setdefault(label, name(path))
        for entry in entries:
            if entry.is_file() and entry.suffix.lower() == '.pkg':
                label = label_of(pkg_content_id(entry) or '')
                if label:
                    seen.setdefault(label, name(entry))
            elif entry.is_dir() and depth < MAX_DEPTH and entry.name != 'sce_sys':
                visit(entry, depth + 1)

    if root.is_dir():
        visit(root, 0)
    return [{'label': label, 'source': seen[label]} for label in sorted(seen)]


def labels(folder):
    """Labels of the add-ons found in folder (sorted, no duplicates); [] without the folder."""
    return [item['label'] for item in found(folder)]
