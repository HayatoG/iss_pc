#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generic shadPS4/GoldHEN cheat-XML loader, shared by every port's own <game>_patches.py.

Reads third-party patch XML files (the "PATCHES" format shadPS4/GoldHEN cheat tools use) from
a user-writable mods directory, keeps only the ones that match this game's title ID and app
version, and compiles literal writes and unique pattern matches into (offset, bytes,
description) triples the caller merges into its own G3PATCH2 patch file (the loader,
probe.c apply_patches, already
applies that format). A patch whose lines fall outside the eboot's segments is skipped on its
own, never aborting the others or the port's own built-in patches.

Address convention: shadPS4/GoldHEN XML "Address" values assume the eboot mapped at 0x400000,
not any one port's own Ghidra base - that is the convention the cheat files on the internet
are already written for, independent of which PS4 game or port reads them.

Disabled by default: a patch only has any effect once the player (or the file's own
isEnabled="true") turns it on in mods.json, same as shadPS4/GoldHEN. With nothing enabled this
module writes nothing, so a port that never looks at it is byte-for-byte unaffected.
"""
import json
import re
import struct
import sys
from pathlib import Path
import xml.etree.ElementTree as ET

EBOOT_BASE = 0x400000


def eboot_segments(elf):
    """[(vaddr, vaddr+memsz)] of the ELF's PT_LOAD segments, for validating patch addresses."""
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x36)
    segments = []
    for i in range(phnum):
        kind, _, _, vaddr, _, _, memsz, _ = struct.unpack_from('<IIQQQQQQ', elf, phoff + i * phentsize)
        if kind == 1:
            segments.append((vaddr, vaddr + memsz))
    return segments


US_107_ELF_SHA256 = 'b3afd173a379f83e1e408e71fdef22b78f93092d332b79decd48a18a08bfc605'
_verified_code = {}


def verified_107_code(game):
    """True when the folder's eboot is the 1.07 executable (identical in US CUSA00223 and EU
    CUSA00004), byte for byte. Remembered while eboot.bin keeps its size and time."""
    eboot = Path(game) / 'eboot.bin'
    try:
        stat = eboot.stat()
    except OSError:
        return False
    key = (str(eboot), stat.st_size, stat.st_mtime_ns)
    if key not in _verified_code:
        try:
            import hashlib
            from infamous_patches import read_eboot_elf
            elf = read_eboot_elf(game)
            _verified_code[key] = elf is not None and hashlib.sha256(elf).hexdigest() == US_107_ELF_SHA256
        except (OSError, ValueError, ImportError):
            _verified_code[key] = False
    return _verified_code[key]


def game_app_version(game):
    """APP_VER of the game folder's param.sfo ("01.03"), or None when it cannot be read.
    Dumps often carry the base game's param.sfo (APP_VER 01.00, VERSION 01.07) beside the 1.07
    eboot: when the eboot is verified 1.07 code, the version is 1.07 whatever the label says."""
    try:
        from prepare import sfo
        version = sfo((Path(game) / 'sce_sys/param.sfo').read_bytes()).get('APP_VER')
    except (OSError, ValueError, ImportError):
        return None
    if version is not None and version != '01.07' and verified_107_code(game):
        return '01.07'
    return version


def encode(line):
    kind, value = line.get('Type'), line.get('Value')
    if kind == 'bytes':
        return bytes.fromhex(value.replace(' ', ''))
    if kind in ('bytes16', 'bytes32', 'bytes64'):
        return int(value, 0).to_bytes(int(kind[5:]) // 8, 'little')
    if kind == 'float32':
        return struct.pack('<f', float(value))
    if kind == 'float64':
        return struct.pack('<d', float(value))
    if kind == 'utf8':
        return value.encode() + b'\0'
    if kind == 'utf16':
        return value.encode('utf-16-le') + b'\0\0'
    raise ValueError(f'unsupported patch type {kind!r}')


def _pattern(line):
    """Parse a shadPS4/GoldHEN mask line into bytes and a byte mask."""
    tokens = (line.get('Address') or '').split()
    if not tokens:
        raise ValueError('empty mask pattern')
    pattern, mask = bytearray(), bytearray()
    for token in tokens:
        if token in ('?', '??'):
            pattern.append(0)
            mask.append(0)
        elif re.fullmatch(r'[0-9a-fA-F]{2}', token):
            pattern.append(int(token, 16))
            mask.append(0xff)
        else:
            raise ValueError(f'invalid mask byte {token!r}')
    return bytes(pattern), bytes(mask)


def _executable_segments(elf):
    """[(virtual address, file bytes)] of executable PT_LOAD segments."""
    phoff, = struct.unpack_from('<Q', elf, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', elf, 0x36)
    segments = []
    for i in range(phnum):
        kind, flags, offset, vaddr, _, filesz, _, _ = struct.unpack_from(
            '<IIQQQQQQ', elf, phoff + i * phentsize)
        if kind == 1 and flags & 1:
            if offset + filesz > len(elf):
                raise ValueError('executable segment extends past eboot')
            segments.append((vaddr, elf[offset:offset + filesz]))
    return segments


def _compile_mask(line, executable_segments):
    """Resolve one pattern patch only when its signature occurs exactly once."""
    pattern, mask = _pattern(line)
    # Use the longest fixed run as a C-level search anchor, then verify wildcards in Python.
    # Scanning an eboot byte by byte in Python would make every launch noticeably slower.
    runs = []
    start = None
    for i, fixed in enumerate(mask + b'\0'):
        if fixed and start is None:
            start = i
        elif not fixed and start is not None:
            runs.append((start, pattern[start:i]))
            start = None
    if not runs:
        raise ValueError('mask pattern has no fixed bytes')
    anchor_offset, anchor = max(runs, key=lambda run: len(run[1]))
    matches = []
    for vaddr, data in executable_segments:
        search_at = 0
        while True:
            found = data.find(anchor, search_at)
            if found < 0:
                break
            at = found - anchor_offset
            search_at = found + 1
            if at >= 0 and at + len(pattern) <= len(data) and all(
                    not mask[i] or data[at + i] == pattern[i] for i in range(len(pattern))):
                matches.append(vaddr + at)
                if len(matches) > 1:
                    raise ValueError('mask pattern is ambiguous (multiple matches)')
    if not matches:
        raise ValueError('mask pattern did not match this eboot')
    try:
        relative = int(line.get('Offset', '0'), 0)
    except ValueError:
        relative = int(line.get('Offset', '0'), 10)
    # matches[0] is already an image offset. Offset is read two ways: GoldHEN cheat files give it
    # relative to the match ("0" = the first byte of the match); files written for this port give
    # EBOOT_BASE + that ("0x400000" = the first byte). An Offset under EBOOT_BASE (in either
    # direction) is the GoldHEN form. Subtracting EBOOT_BASE from it, as before, wrote 4 MB too
    # early, or skipped the mod when that fell before the eboot.
    if abs(relative) >= EBOOT_BASE:
        relative -= EBOOT_BASE
    offset = matches[0] + relative
    return offset, encode({**line.attrib, 'Type': 'bytes'})


def external_patches(directory, app_version, ids, builtin_files=()):
    """[(key, path, metadata)] of eboot patches for this version in directory/*.xml.
    key is "<file name>/<patch name>" (the launcher's selection key, mods.json). ids: this
    game's title IDs; a file that scopes itself with <ID> entries is kept only when one of
    them is ours, a file with none at all is assumed to be written for this game already."""
    found = []
    directory = Path(directory)
    paths = sorted(directory.glob('*.xml')) if directory.is_dir() else []
    paths.extend(Path(path) for path in builtin_files)
    for path in paths:
        try:
            root = ET.parse(path).getroot()
        except ET.ParseError as error:
            print(f'Mods: {path.name}: {error}', file=sys.stderr)
            continue
        file_ids = {e.text.strip() for e in root.iter('ID') if e.text}
        if file_ids and not file_ids & ids:
            continue
        for meta in root.iter('Metadata'):
            if meta.get('AppVer') == app_version and meta.get('AppElf', 'eboot.bin') == 'eboot.bin':
                found.append((f'{path.name}/{meta.get("Name")}', path, meta))
    return found


def external_selection(found, config):
    """Selected patches: mods.json {"enabled": [...], "disabled": [...]} overrides each file's
    own isEnabled. Nothing the player has not opted into (file default or mods.json) is ever
    returned, so an untouched mods folder changes no bytes."""
    settings = {}
    if config and Path(config).is_file():
        try:
            settings = json.loads(Path(config).read_text())
        except (OSError, ValueError) as error:
            print(f'Mods: cannot read {config}: {error}', file=sys.stderr)
    enabled, disabled = set(settings.get('enabled', [])), set(settings.get('disabled', []))
    return [(key, path, meta) for key, path, meta in found
            if key in enabled or (key not in disabled and meta.get('isEnabled', 'false').lower() == 'true')]


def compile_external(selected, segments, eboot=None):
    """[(offset, bytes, description)] writes of selected patches.

    Literal writes and unique executable-segment mask matches are supported. A patch with any
    invalid line is skipped whole, so a mismatched or malformed cheat does not write somewhere
    wrong.
    """
    writes = []
    executable = None
    for key, _, meta in selected:
        try:
            ours = []
            for line in meta.iter('Line'):
                if line.get('Type') == 'mask':
                    if eboot is None:
                        raise ValueError('mask patches need the eboot to locate their pattern')
                    if executable is None:
                        executable = _executable_segments(eboot)
                    offset, data = _compile_mask(line, executable)
                else:
                    offset = int(line.get('Address') or '', 0) - EBOOT_BASE
                    data = encode(line)
                if not any(start <= offset and offset + len(data) <= end for start, end in segments):
                    raise ValueError(f'address {line.get("Address")} is outside the eboot')
                ours.append((offset, data, f'{key} ({line.get("Address")})'))
        except ValueError as error:
            print(f'Mods: skipped {key}: {error}', file=sys.stderr)
            continue
        writes += ours
        print(f'Mods: {key} ({len(ours)} writes)')
    return writes


def catalogue(directory, app_version, ids):
    """[(key, name, note, enabled_by_file)] of every patch found for this version, whether or
    not it is currently selected - for a launcher's Mods tab to list and let the player toggle."""
    return [(key, meta.get('Name'), meta.get('Note', ''), meta.get('isEnabled', 'false').lower() == 'true')
            for key, _, meta in external_patches(directory, app_version, ids)]
