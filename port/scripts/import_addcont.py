# SPDX-License-Identifier: GPL-2.0-or-later
"""Inspect/import the player's unencrypted DLC metadata, never package filesystem data.

This is a setup companion to AppContent unlocker support (#249), not a replacement
for it. Inspection is read-only; --install creates <game>-DLC/<CONTENT_ID>/sce_sys/param.sfo.
"""
import argparse
import json
import os
from pathlib import Path
import re
import struct
import sys

from prepare import sfo

LIMIT = 1 << 20
CONTENT_ID = re.compile(r'[A-Z]{2}[0-9]{4}-(CUSA[0-9]{5})_00-([A-Za-z0-9]{16})')


def checked_sfo(data):
    if not 20 <= len(data) <= LIMIT:
        raise ValueError('invalid param.sfo size')
    magic, _, keys, values, count = struct.unpack_from('<4sIIII', data)
    if magic != b'\0PSF' or not 20 + count * 16 <= keys <= values <= len(data):
        raise ValueError('invalid param.sfo tables')
    names = set()
    for index in range(count):
        key, fmt, size, capacity, offset = struct.unpack_from('<HHIII', data, 20 + index * 16)
        end = data.find(b'\0', keys + key, values)
        if keys + key >= values or end < 0 or size > capacity or values + offset + capacity > len(data):
            raise ValueError('invalid param.sfo entry bounds')
        name = data[keys + key:end]
        if name in names or (fmt == 0x404 and size != 4):
            raise ValueError('duplicate/invalid param.sfo entry')
        names.add(name)
    return sfo(data)


def metadata(path):
    """Read bounded SFO bytes, locating PKG PARAM_SFO by ID 0x1000, not entry index 9.

    Layout reference: maxton/LibOrbisPkg PKG/PkgReader.cs, Entry.cs, Enums.cs.
    Encrypted entries are refused; no keys, passcodes or PFS extraction are used.
    """
    path = Path(path)
    with path.open('rb') as stream:
        if path.suffix.lower() != '.pkg':
            return stream.read(LIMIT + 1)
        total = os.fstat(stream.fileno()).st_size
        header = stream.read(0x20)
        if len(header) != 0x20 or header[:4] != b'\x7fCNT':
            raise ValueError('not a PS4 PKG')
        count = struct.unpack_from('>I', header, 0x10)[0]
        table = struct.unpack_from('>I', header, 0x18)[0]
        if not 0 < count <= 4096 or table < 0x20 or table + count * 32 > total:
            raise ValueError('invalid PKG entry table')
        stream.seek(table)
        matches = []
        for _ in range(count):
            entry, _, flags, _, offset, size = struct.unpack('>6I8x', stream.read(32))
            if entry == 0x1000:
                matches.append((flags, offset, size))
        if len(matches) != 1:
            raise ValueError('PKG must contain exactly one PARAM_SFO entry')
        flags, offset, size = matches[0]
        if flags & 0x80000000:
            raise ValueError('encrypted PARAM_SFO is unsupported; supply extracted metadata')
        if not 20 <= size <= LIMIT or offset < table + count * 32 or offset + size > total:
            raise ValueError('invalid PKG PARAM_SFO bounds')
        stream.seek(offset)
        return stream.read(size)


def inspect(game, sources):
    game = Path(game).resolve()
    title = checked_sfo(metadata(game / 'sce_sys/param.sfo')).get('TITLE_ID')
    if not re.fullmatch(r'CUSA[0-9]{5}', title or ''):
        raise ValueError('game metadata has no valid TITLE_ID')
    paths = []
    for source in sources:
        source = Path(source)
        if source.is_dir():
            paths.extend(p for p in source.rglob('*') if p.is_file()
                         and (p.suffix.lower() == '.pkg' or p.name.lower() == 'param.sfo'))
        else:
            paths.append(source)
    if not paths:
        raise ValueError('no .pkg or param.sfo inputs found')
    records = {}
    for path in sorted(set(paths)):
        data = metadata(path)
        values = checked_sfo(data)
        content = values.get('CONTENT_ID', '')
        match = CONTENT_ID.fullmatch(content) if isinstance(content, str) else None
        if values.get('CATEGORY') != 'ac' or not match or match[1] != title:
            raise ValueError(f'{path.name}: expected CATEGORY ac and a DLC content ID for {title}')
        label = match[2]
        if label in records and records[label]['data'] != data:
            raise ValueError(f'conflicting metadata for {label}')
        records[label] = dict(content_id=content, label=label, title=values.get('TITLE', ''), data=data)
    if len(records) > 32:
        raise ValueError('more than 32 add-ons')
    return title, [records[key] for key in sorted(records)]


def install(game, records):
    game = Path(game).resolve()
    root = game.with_name(game.name + '-DLC')
    paths = []
    for item in records:
        dest = root / item['content_id'] / 'sce_sys/param.sfo'
        for part in (root, dest.parent.parent, dest.parent, dest):
            if part.is_symlink() or getattr(part, 'is_junction', lambda: False)():
                raise ValueError('linked DLC destinations are unsupported')
        if dest.exists() and (not dest.is_file() or metadata(dest) != item['data']):
            raise ValueError(f'existing metadata differs: {dest}')
        paths.append((dest, item['data']))
    # Validate every input/destination before creating any directories. Existing data is never overwritten.
    for dest, data in paths:
        dest.parent.mkdir(parents=True, exist_ok=True)
        try:
            with dest.open('xb') as stream:
                stream.write(data)
        except FileExistsError:
            if metadata(dest) != data:
                raise ValueError(f'destination changed during import: {dest}')
    return root


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game', required=True, type=Path)
    parser.add_argument('--install', action='store_true', help='copy validated metadata to the sibling -DLC folder')
    parser.add_argument('sources', nargs='+', type=Path)
    args = parser.parse_args(argv)
    try:
        title, records = inspect(args.game, args.sources)
        game = args.game.resolve()
        root = install(game, records) if args.install else game.with_name(game.name + '-DLC')
        print(json.dumps(dict(title_id=title, installed=bool(args.install), destination=str(root),
            addons=[{k: v for k, v in item.items() if k != 'data'} for item in records]), indent=2))
        print('Metadata only. In-game unlocks require AppContent unlocker support (#249).', file=sys.stderr)
        return 0
    except (OSError, ValueError, struct.error) as error:
        print(f'DLC metadata: {error}', file=sys.stderr)
        return 2


if __name__ == '__main__':
    sys.exit(main())
