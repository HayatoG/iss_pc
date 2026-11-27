#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Dynamic Keyboard & Mouse button prompt atlas generator for inFAMOUS Second Son.

Reads the game's original 512x512 RGBA8 button-icon font atlas (SBitmap OID 29c11c0cac5b95ed,
PS4 Thin_1dThin tiled, bottom-up Y) from any lang_*_text.xpps file, replaces the 28 PS4 button
glyph slots (U+F080..U+F0E6) with crisp, anti-aliased keycap and mouse icons matching the
player's effective keybinds (default + custom ISS_KEY_MAP / tlg.ini), and writes the patched
1 MiB tiled atlas to out/kbd_prompts_atlas.bin for transparent runtime substitution in
port/src/runtime_file.c.

All non-button glyphs (game logo, karma/shard/mission/editor icons) remain bit-for-bit intact.
Pure Python standard library: works identically in source runs, frozen Qt/Tk launchers, and CI.
"""
import argparse
import hashlib
import json
import math
import struct
import sys
import zlib
from pathlib import Path

ATLAS_W = 512
ATLAS_H = 512
ATLAS_BYTES = ATLAS_W * ATLAS_H * 4
KCAP_DATA_OFFSET = 0x208
BITMAP_OID = bytes.fromhex('29c11c0cac5b95ed')
EXPECTED_DW1 = 0x00A00000  # RGBA8 UNORM
EXPECTED_DW2 = 0x007FC1FF  # 512x512

# Exact glyph rectangles in the 512x512 icon atlas (verified across all 15 language packs).
# Format: (codepoint, x, y_top_down, w, h, button_role)
BUTTON_GLYPHS = (
    (0xF080, 153, 263, 50, 50, 'circle'),
    (0xF081,   0, 263, 50, 50, 'square'),
    (0xF082,  51, 263, 50, 50, 'triangle'),
    (0xF083, 102, 263, 50, 50, 'cross'),
    (0xF084,   0, 212, 58, 50, 'l1'),
    (0xF085,  59, 212, 58, 50, 'l2'),
    (0xF086, 118, 212, 58, 50, 'r1'),
    (0xF087, 257,   0, 58, 50, 'r2'),
    (0xF088, 177, 212, 58, 50, 'options'),
    (0xF089, 236, 212, 58, 50, 'options'),
    (0xF08A, 295,  51, 58, 50, 'left_stick'),
    (0xF08B, 354,  51, 58, 50, 'l3'),
    (0xF08C, 413,  51, 58, 50, 'right_stick'),
    (0xF08D, 316,   0, 58, 50, 'r3'),
    (0xF090, 204, 263, 50, 50, 'dpad_all'),
    (0xF091, 255, 263, 50, 50, 'right'),
    (0xF092, 306, 102, 50, 50, 'up'),
    (0xF093, 357, 102, 50, 50, 'left'),
    (0xF094, 408, 102, 50, 50, 'down'),
    (0xF098,   0, 314, 50, 50, 'touchpad_left'),
    (0xF0A2, 375,   0, 58, 50, 'touchpad_left'),
    (0xF0E0, 204, 314, 50, 50, 'l1'),
    (0xF0E1, 255, 314, 50, 50, 'l2'),
    (0xF0E2, 306, 153, 50, 50, 'r1'),
    (0xF0E3, 357, 153, 50, 50, 'r2'),
    (0xF0E4, 408, 153, 50, 50, 'l3'),
    (0xF0E5, 459, 153, 50, 50, 'r3'),
    (0xF0E6, 434,   0, 58, 50, 'options'),
)

# Default keyboard binds (mirrors port/launcher/padsettings.py and port/src/runtime_pad.c).
DEFAULT_KEYS = {
    'cross': 'Space',
    'circle': 'Left Shift',
    'square': 'E',
    'triangle': 'Q',
    'l1': '1',
    'r1': '3',
    'l2': 'R',
    'r2': 'F',
    'l3': 'Z',
    'r3': 'C',
    'options': 'Return',
    'up': 'I',
    'left': 'J',
    'down': 'K',
    'right': 'L',
    'touchpad_left': 'Tab',
    'touchpad_right': 'Backspace',
}

# Mouse defaults active in runtime_pad.c when mouse_enabled is on and not overridden.
DEFAULT_MOUSE_OVERRIDES = {
    'r2': 'Mouse Left',
    'l2': 'Mouse Right',
    'r3': 'Mouse Middle',
}

PS4_BUTTON_ALIASES = {
    'touchpad': 'touchpad_left',
}

VALID_BUTTONS = frozenset(DEFAULT_KEYS) | frozenset(PS4_BUTTON_ALIASES)

MOUSE_SOURCE_KIND = {
    'mouse left': 'mouse_left',
    'mouse right': 'mouse_right',
    'mouse middle': 'mouse_middle',
    'mouse x1': 'mouse_x1',
    'mouse x2': 'mouse_x2',
    'wheel up': 'wheel_up',
    'wheel down': 'wheel_down',
    'wheel left': 'wheel_left',
    'wheel right': 'wheel_right',
}

ARROW_KEY_KIND = {
    'up': 'arrow_up',
    'down': 'arrow_down',
    'left': 'arrow_left',
    'right': 'arrow_right',
}

SPECIAL_KEY_LABELS = {
    'space': 'SPACE',
    'return': 'ENTER',
    'enter': 'ENTER',
    'escape': 'ESC',
    'backspace': 'BKSP',
    'tab': 'TAB',
    'capslock': 'CAPS',
    'left shift': 'SHIFT',
    'right shift': 'RSHF',
    'lshift': 'SHIFT',
    'rshift': 'RSHF',
    'left ctrl': 'CTRL',
    'right ctrl': 'RCTL',
    'lctrl': 'CTRL',
    'rctrl': 'RCTL',
    'left alt': 'ALT',
    'right alt': 'RALT',
    'lalt': 'ALT',
    'ralt': 'RALT',
    'left gui': 'WIN',
    'right gui': 'RWIN',
    'insert': 'INS',
    'delete': 'DEL',
    'home': 'HOME',
    'end': 'END',
    'pageup': 'PGUP',
    'pagedown': 'PGDN',
    'printscreen': 'PRTS',
    'scrolllock': 'SCRL',
    'pause': 'PAUS',
    'numlock': 'NUML',
    'menu': 'MENU',
    'application': 'APP',
    'keypad enter': 'KPE',
    'keypad /': 'KP/',
    'keypad *': 'KP*',
    'keypad -': 'KP-',
    'keypad +': 'KP+',
    'keypad .': 'KP.',
    'keypad =': 'KP=',
}


def parse_key_map(spec):
    """Parses ISS_KEY_MAP / tlg.ini key_map into {ps4_button: first_source_or_None}."""
    overrides = {}
    if not spec:
        return overrides
    for raw in str(spec).split(','):
        entry = raw.strip()
        if not entry or '=' not in entry:
            continue
        button, _, names = entry.partition('=')
        button = button.strip().lower()
        names = names.strip()
        if button not in VALID_BUTTONS:
            continue
        button = PS4_BUTTON_ALIASES.get(button, button)
        if names.lower() == 'none':
            overrides[button] = None
            continue
        sources = [n.strip() for n in names.split('|') if n.strip()]
        if sources:
            overrides[button] = sources[0]
    return overrides


def effective_bindings(key_map='', mouse_enabled=True):
    """Returns {role: source_name_or_None} for every button role in the icon atlas."""
    bindings = dict(DEFAULT_KEYS)
    if mouse_enabled:
        bindings.update(DEFAULT_MOUSE_OVERRIDES)
    bindings.update(parse_key_map(key_map))
    bindings['left_stick'] = '__WASD__'
    bindings['right_stick'] = '__MOUSE_MOVE__' if mouse_enabled else '__ARROWS__'
    bindings['dpad_all'] = '__DPAD__'
    return bindings


def key_display_spec(source):
    """Maps an SDL key/mouse source name to (kind, label).

    kind is one of: 'none', 'mouse_*', 'wheel_*', 'arrow_*', 'text'.
    """
    if not source:
        return ('none', '-')
    low = source.strip().lower()
    if low in MOUSE_SOURCE_KIND:
        return (MOUSE_SOURCE_KIND[low], '')
    if low in ARROW_KEY_KIND:
        return (ARROW_KEY_KIND[low], '')
    if low in SPECIAL_KEY_LABELS:
        return ('text', SPECIAL_KEY_LABELS[low])
    if low.startswith('keypad ') and len(low) == 8 and low[7].isdigit():
        return ('text', 'KP' + low[7])
    cleaned = source.strip().upper()
    if len(cleaned) <= 5:
        return ('text', cleaned)
    # Compact fallback for any uncommon SDL key name.
    compact = ''.join(ch for ch in cleaned if ch.isalnum())
    return ('text', (compact or cleaned)[:5])


def short_cluster_label(source):
    """1-char (or arrow token) label for a 4-key mini-cluster cell."""
    if not source:
        return '-'
    low = source.strip().lower()
    if low in ARROW_KEY_KIND:
        return ARROW_KEY_KIND[low]
    if low.startswith('keypad ') and len(low) == 8:
        return low[7]
    if low in MOUSE_SOURCE_KIND:
        return 'M'
    _, lbl = key_display_spec(source)
    return lbl[0] if lbl else '-'


# ---------------------------------------------------------------------------
# PS4 Thin_1dThin (mode 13, 32bpp RGBA8, bottom-up Y) tiling & untiling
# ---------------------------------------------------------------------------

def _microtile_elem(lx, ly):
    return (
        (lx & 1)
        | ((lx & 2) << 1)
        | ((lx & 4) << 2)
        | ((ly & 1) << 1)
        | ((ly & 2) << 2)
        | ((ly & 4) << 3)
    )


def untile_atlas(src):
    """Untiles 512x512 PS4 Thin_1dThin bottom-up RGBA8 into top-down RGBA8 bytearray."""
    if len(src) != ATLAS_BYTES:
        raise ValueError(f'Expected {ATLAS_BYTES} bytes, got {len(src)}')
    out = bytearray(ATLAS_BYTES)
    tiles_x = ATLAS_W // 8
    for ty in range(ATLAS_H // 8):
        for tx in range(tiles_x):
            tile_base = (ty * tiles_x + tx) * 64
            base_x = tx * 8
            base_y = ty * 8
            for ly in range(8):
                dst_row = (ATLAS_H - 1 - (base_y + ly)) * ATLAS_W + base_x
                for lx in range(8):
                    soff = (tile_base + _microtile_elem(lx, ly)) * 4
                    doff = (dst_row + lx) * 4
                    out[doff:doff + 4] = src[soff:soff + 4]
    return out


def tile_atlas(rgba):
    """Tiles top-down 512x512 RGBA8 back into PS4 Thin_1dThin bottom-up RGBA8 bytes."""
    if len(rgba) != ATLAS_BYTES:
        raise ValueError(f'Expected {ATLAS_BYTES} bytes, got {len(rgba)}')
    out = bytearray(ATLAS_BYTES)
    tiles_x = ATLAS_W // 8
    for ty in range(ATLAS_H // 8):
        for tx in range(tiles_x):
            tile_base = (ty * tiles_x + tx) * 64
            base_x = tx * 8
            base_y = ty * 8
            for ly in range(8):
                src_row = (ATLAS_H - 1 - (base_y + ly)) * ATLAS_W + base_x
                for lx in range(8):
                    soff = (src_row + lx) * 4
                    doff = (tile_base + _microtile_elem(lx, ly)) * 4
                    out[doff:doff + 4] = rgba[soff:soff + 4]
    return bytes(out)


def find_lang_atlas_source(game_dir):
    """Finds any lang_*_text.xpps in game_dir/art/cache with the 512x512 icon atlas.

    Returns (path, tex_offset). All 15 languages carry the exact same 512x512 atlas.
    """
    cache = Path(game_dir) / 'art' / 'cache'
    if not cache.is_dir():
        return None, None
    # Prefer English first for deterministic logs, then any other language pack.
    candidates = sorted(cache.glob('lang_*_text.xpps'))
    candidates.sort(key=lambda p: (0 if p.name == 'lang_english_text.xpps' else 1, p.name))
    for path in candidates:
        try:
            with open(path, 'rb') as f:
                header = f.read(4096)
        except OSError:
            continue
        pos = header.find(BITMAP_OID)
        if pos < 0x20:
            continue
        dw0, dw1, dw2 = struct.unpack_from('<3I', header, pos - 0x20)
        if dw1 == EXPECTED_DW1 and dw2 == EXPECTED_DW2:
            tex_off = KCAP_DATA_OFFSET + dw0
            if path.stat().st_size >= tex_off + ATLAS_BYTES:
                return path, tex_off
    return None, None


def read_original_atlas(game_dir):
    """Reads the original 512x512 top-down RGBA8 icon atlas from the game dump."""
    path, tex_off = find_lang_atlas_source(game_dir)
    if path is None:
        raise FileNotFoundError(
            f'Could not locate 512x512 button icon atlas in {Path(game_dir) / "art" / "cache"}'
        )
    with open(path, 'rb') as f:
        f.seek(tex_off)
        raw = f.read(ATLAS_BYTES)
    return untile_atlas(raw), path


# ---------------------------------------------------------------------------
# Crisp analytic SDF renderer (pure white RGB=255,255,255 + smooth alpha mask)
# Matches Sucker Punch's exact HUD tintable glyph format.
# ---------------------------------------------------------------------------

def _clamp01(v):
    return 0.0 if v <= 0.0 else (1.0 if v >= 1.0 else v)


def _cov(dist, feather=0.75):
    """Converts signed distance (negative=inside) to anti-aliased coverage [0, 1]."""
    return _clamp01(0.5 - dist / (2.0 * feather))


def _sd_round_box(px, py, cx, cy, hw, hh, r):
    qx = abs(px - cx) - (hw - r)
    qy = abs(py - cy) - (hh - r)
    ox = qx if qx > 0.0 else 0.0
    oy = qy if qy > 0.0 else 0.0
    inside = qx if qx > qy else qy
    if inside > 0.0:
        inside = 0.0
    return math.hypot(ox, oy) + inside - r


def _sd_segment(px, py, x1, y1, x2, y2):
    vx = x2 - x1
    vy = y2 - y1
    wx = px - x1
    wy = py - y1
    vv = vx * vx + vy * vy
    if vv <= 1e-9:
        return math.hypot(wx, wy)
    t = (wx * vx + wy * vy) / vv
    if t < 0.0:
        t = 0.0
    elif t > 1.0:
        t = 1.0
    return math.hypot(wx - vx * t, wy - vy * t)


def _sd_arc(px, py, cx, cy, rx, ry, a0, a1, steps=12):
    """Distance to an elliptical arc sampled as polyline segments (exact enough at <=60px)."""
    best = 1e9
    px0 = cx + rx * math.cos(a0)
    py0 = cy + ry * math.sin(a0)
    for i in range(1, steps + 1):
        ang = a0 + (a1 - a0) * (i / steps)
        px1 = cx + rx * math.cos(ang)
        py1 = cy + ry * math.sin(ang)
        d = _sd_segment(px, py, px0, py0, px1, py1)
        if d < best:
            best = d
        px0, py0 = px1, py1
    return best


# Clean geometric sans-serif stroke font in normalized [0..1] x [0..1] glyph coordinates.
# Each entry is a tuple of segments (x1, y1, x2, y2). Curves use clean multi-segment polylines.
def _arc_segs(cx, cy, rx, ry, deg0, deg1, steps=8):
    segs = []
    a0 = math.radians(deg0)
    a1 = math.radians(deg1)
    x0 = cx + rx * math.cos(a0)
    y0 = cy + ry * math.sin(a0)
    for i in range(1, steps + 1):
        a = a0 + (a1 - a0) * (i / steps)
        x1 = cx + rx * math.cos(a)
        y1 = cy + ry * math.sin(a)
        segs.append((x0, y0, x1, y1))
        x0, y0 = x1, y1
    return tuple(segs)


VECTOR_FONT = {
    'A': ((0.06, 0.96, 0.50, 0.04), (0.50, 0.04, 0.94, 0.96), (0.22, 0.66, 0.78, 0.66)),
    'B': (
        (0.14, 0.04, 0.14, 0.96), (0.14, 0.04, 0.56, 0.04), (0.14, 0.48, 0.58, 0.48),
        (0.14, 0.96, 0.58, 0.96),
        *_arc_segs(0.56, 0.26, 0.30, 0.22, -90, 90, 6),
        *_arc_segs(0.58, 0.72, 0.32, 0.24, -90, 90, 6),
    ),
    'C': _arc_segs(0.56, 0.50, 0.42, 0.46, 42, 318, 12),
    'D': (
        (0.14, 0.04, 0.14, 0.96), (0.14, 0.04, 0.46, 0.04), (0.14, 0.96, 0.46, 0.96),
        *_arc_segs(0.46, 0.50, 0.44, 0.46, -90, 90, 10),
    ),
    'E': ((0.16, 0.04, 0.16, 0.96), (0.16, 0.04, 0.88, 0.04), (0.16, 0.49, 0.78, 0.49), (0.16, 0.96, 0.88, 0.96)),
    'F': ((0.16, 0.04, 0.16, 0.96), (0.16, 0.04, 0.88, 0.04), (0.16, 0.50, 0.78, 0.50)),
    'G': (
        *_arc_segs(0.54, 0.50, 0.42, 0.46, 15, 325, 12),
        (0.54, 0.52, 0.94, 0.52), (0.94, 0.52, 0.94, 0.96),
    ),
    'H': ((0.14, 0.04, 0.14, 0.96), (0.86, 0.04, 0.86, 0.96), (0.14, 0.49, 0.86, 0.49)),
    'I': ((0.50, 0.04, 0.50, 0.96), (0.26, 0.04, 0.74, 0.04), (0.26, 0.96, 0.74, 0.96)),
    'J': ((0.76, 0.04, 0.76, 0.68), *_arc_segs(0.46, 0.68, 0.30, 0.28, 0, 165, 8)),
    'K': ((0.14, 0.04, 0.14, 0.96), (0.88, 0.04, 0.18, 0.54), (0.38, 0.40, 0.90, 0.96)),
    'L': ((0.16, 0.04, 0.16, 0.96), (0.16, 0.96, 0.88, 0.96)),
    'M': ((0.10, 0.96, 0.10, 0.04), (0.10, 0.04, 0.50, 0.62), (0.50, 0.62, 0.90, 0.04), (0.90, 0.04, 0.90, 0.96)),
    'N': ((0.14, 0.96, 0.14, 0.04), (0.14, 0.04, 0.86, 0.96), (0.86, 0.96, 0.86, 0.04)),
    'O': _arc_segs(0.50, 0.50, 0.40, 0.46, 0, 360, 14),
    'P': (
        (0.16, 0.04, 0.16, 0.96), (0.16, 0.04, 0.56, 0.04), (0.16, 0.54, 0.56, 0.54),
        *_arc_segs(0.56, 0.29, 0.32, 0.25, -90, 90, 8),
    ),
    'Q': (*_arc_segs(0.50, 0.48, 0.40, 0.44, 0, 360, 14), (0.58, 0.66, 0.92, 0.98)),
    'R': (
        (0.16, 0.04, 0.16, 0.96), (0.16, 0.04, 0.56, 0.04), (0.16, 0.52, 0.56, 0.52),
        *_arc_segs(0.56, 0.28, 0.32, 0.24, -90, 90, 8),
        (0.52, 0.52, 0.90, 0.96),
    ),
    'S': (
        *_arc_segs(0.52, 0.26, 0.36, 0.22, 115, 340, 8),
        (0.28, 0.43, 0.74, 0.55),
        *_arc_segs(0.48, 0.74, 0.38, 0.22, -65, 165, 8),
    ),
    'T': ((0.08, 0.04, 0.92, 0.04), (0.50, 0.04, 0.50, 0.96)),
    'U': ((0.14, 0.04, 0.14, 0.64), (0.86, 0.04, 0.86, 0.64), *_arc_segs(0.50, 0.64, 0.36, 0.32, 0, 180, 8)),
    'V': ((0.10, 0.04, 0.50, 0.96), (0.50, 0.96, 0.90, 0.04)),
    'W': ((0.06, 0.04, 0.28, 0.96), (0.28, 0.96, 0.50, 0.36), (0.50, 0.36, 0.72, 0.96), (0.72, 0.96, 0.94, 0.04)),
    'X': ((0.12, 0.04, 0.88, 0.96), (0.88, 0.04, 0.12, 0.96)),
    'Y': ((0.10, 0.04, 0.50, 0.52), (0.90, 0.04, 0.50, 0.52), (0.50, 0.52, 0.50, 0.96)),
    'Z': ((0.12, 0.04, 0.88, 0.04), (0.88, 0.04, 0.12, 0.96), (0.12, 0.96, 0.88, 0.96)),
    '0': (*_arc_segs(0.50, 0.50, 0.36, 0.46, 0, 360, 14), (0.26, 0.78, 0.74, 0.22)),
    '1': ((0.24, 0.24, 0.54, 0.04), (0.54, 0.04, 0.54, 0.96), (0.22, 0.96, 0.84, 0.96)),
    '2': (*_arc_segs(0.50, 0.28, 0.36, 0.24, 195, 395, 8), (0.82, 0.40, 0.14, 0.96), (0.14, 0.96, 0.88, 0.96)),
    '3': (
        *_arc_segs(0.48, 0.27, 0.36, 0.23, 210, 450, 8),
        *_arc_segs(0.48, 0.73, 0.38, 0.23, -90, 155, 8),
    ),
    '4': ((0.70, 0.96, 0.70, 0.04), (0.70, 0.04, 0.10, 0.68), (0.10, 0.68, 0.90, 0.68)),
    '5': (
        (0.84, 0.04, 0.20, 0.04), (0.20, 0.04, 0.16, 0.46), (0.16, 0.46, 0.54, 0.44),
        *_arc_segs(0.52, 0.70, 0.36, 0.26, -80, 155, 8),
    ),
    '6': (
        *_arc_segs(0.52, 0.50, 0.38, 0.46, 120, 320, 8),
        *_arc_segs(0.50, 0.68, 0.36, 0.28, 0, 360, 12),
    ),
    '7': ((0.12, 0.04, 0.88, 0.04), (0.88, 0.04, 0.34, 0.96)),
    '8': (*_arc_segs(0.50, 0.26, 0.32, 0.22, 0, 360, 10), *_arc_segs(0.50, 0.72, 0.37, 0.24, 0, 360, 12)),
    '9': (
        *_arc_segs(0.50, 0.32, 0.36, 0.28, 0, 360, 12),
        *_arc_segs(0.48, 0.50, 0.38, 0.46, -60, 140, 8),
    ),
    '-': ((0.18, 0.50, 0.82, 0.50),),
    '=': ((0.16, 0.36, 0.84, 0.36), (0.16, 0.66, 0.84, 0.66)),
    '+': ((0.16, 0.50, 0.84, 0.50), (0.50, 0.16, 0.50, 0.84)),
    '/': ((0.80, 0.06, 0.20, 0.94),),
    '\\': ((0.20, 0.06, 0.80, 0.94),),
    '*': ((0.50, 0.14, 0.50, 0.86), (0.18, 0.32, 0.82, 0.68), (0.82, 0.32, 0.18, 0.68)),
    '.': ((0.48, 0.84, 0.52, 0.84),),
    ',': ((0.54, 0.78, 0.40, 0.98),),
    ';': ((0.50, 0.34, 0.50, 0.38), (0.54, 0.74, 0.40, 0.96)),
    "'": ((0.50, 0.10, 0.50, 0.38),),
    '`': ((0.38, 0.10, 0.62, 0.34),),
    '[': ((0.66, 0.04, 0.34, 0.04), (0.34, 0.04, 0.34, 0.96), (0.34, 0.96, 0.66, 0.96)),
    ']': ((0.34, 0.04, 0.66, 0.04), (0.66, 0.04, 0.66, 0.96), (0.66, 0.96, 0.34, 0.96)),
    '#': ((0.32, 0.08, 0.24, 0.92), (0.76, 0.08, 0.68, 0.92), (0.12, 0.34, 0.88, 0.34), (0.08, 0.66, 0.84, 0.66)),
}


def _glyph_segments_scaled(ch, x0, y0, gw, gh):
    segs = VECTOR_FONT.get(ch, VECTOR_FONT['-'])
    return [(x0 + a * gw, y0 + b * gh, x0 + c * gw, y0 + d * gh) for a, b, c, d in segs]


def _arrow_segments_scaled(direction, x0, y0, gw, gh):
    cx = x0 + gw * 0.5
    cy = y0 + gh * 0.5
    if direction == 'arrow_up':
        return [
            (cx, y0 + gh * 0.14, cx, y0 + gh * 0.88),
            (x0 + gw * 0.18, y0 + gh * 0.46, cx, y0 + gh * 0.14),
            (x0 + gw * 0.82, y0 + gh * 0.46, cx, y0 + gh * 0.14),
        ]
    if direction == 'arrow_down':
        return [
            (cx, y0 + gh * 0.12, cx, y0 + gh * 0.86),
            (x0 + gw * 0.18, y0 + gh * 0.54, cx, y0 + gh * 0.86),
            (x0 + gw * 0.82, y0 + gh * 0.54, cx, y0 + gh * 0.86),
        ]
    if direction == 'arrow_left':
        return [
            (x0 + gw * 0.14, cy, x0 + gw * 0.88, cy),
            (x0 + gw * 0.46, y0 + gh * 0.18, x0 + gw * 0.14, cy),
            (x0 + gw * 0.46, y0 + gh * 0.82, x0 + gw * 0.14, cy),
        ]
    return [
        (x0 + gw * 0.12, cy, x0 + gw * 0.86, cy),
        (x0 + gw * 0.54, y0 + gh * 0.18, x0 + gw * 0.86, cy),
        (x0 + gw * 0.54, y0 + gh * 0.82, x0 + gw * 0.86, cy),
    ]


def _min_seg_dist(px, py, segs):
    best = 1e9
    for x1, y1, x2, y2 in segs:
        d = _sd_segment(px, py, x1, y1, x2, y2)
        if d < best:
            best = d
    return best


def _render_keycap(w, h, kind, label):
    """Renders a crisp white rounded keycap badge with dark/transparent cutout lettering."""
    buf = bytearray(w * h * 4)
    cx = w * 0.5
    cy = h * 0.5
    margin_x = 3.0 if w >= 54 else 2.5
    margin_y = 4.0
    hw = w * 0.5 - margin_x
    hh = h * 0.5 - margin_y
    radius = 7.0

    # Inner keycap face sits slightly higher to give a subtle bottom keycap bevel edge.
    face_cy = cy - 1.5
    face_hw = hw - 2.0
    face_hh = hh - 2.5
    face_r = 5.0

    segs = []
    if kind.startswith('arrow_'):
        aw = min(face_hw * 1.15, 22.0)
        ah = min(face_hh * 1.15, 22.0)
        segs = _arrow_segments_scaled(kind, cx - aw * 0.5, face_cy - ah * 0.5, aw, ah)
        stroke = 2.4
    else:
        text = label or '-'
        n = len(text)
        avail_w = face_hw * 2.0 - 5.5
        avail_h = face_hh * 2.0 - 5.5
        if n == 1:
            gh = min(avail_h * 0.86, 23.0)
            gw = min(avail_w * 0.62, gh * 0.72)
            gap = 0.0
            stroke = 2.65
        elif n == 2:
            gh = min(avail_h * 0.74, 19.5)
            gap = 3.2
            gw = min((avail_w - gap) / 2.0, gh * 0.68)
            stroke = 2.15
        elif n == 3:
            gh = min(avail_h * 0.62, 16.5)
            gap = 2.4
            gw = min((avail_w - gap * 2.0) / 3.0, gh * 0.64)
            stroke = 1.75
        elif n == 4:
            gh = min(avail_h * 0.54, 14.2)
            gap = 1.9
            gw = min((avail_w - gap * 3.0) / 4.0, gh * 0.60)
            stroke = 1.48
        else:
            gh = min(avail_h * 0.46, 12.2)
            gap = 1.4
            gw = min((avail_w - gap * (n - 1)) / n, gh * 0.58)
            stroke = 1.22
        total_w = n * gw + max(0, n - 1) * gap
        start_x = cx - total_w * 0.5
        start_y = face_cy - gh * 0.5
        for i, ch in enumerate(text):
            segs.extend(_glyph_segments_scaled(ch, start_x + i * (gw + gap), start_y, gw, gh))

    dim_factor = 0.45 if kind == 'none' else 1.0
    for y in range(h):
        py = y + 0.5
        row_off = y * w * 4
        for x in range(w):
            px = x + 0.5
            d_outer = _sd_round_box(px, py, cx, cy, hw, hh, radius)
            cov_outer = _cov(d_outer, 0.7)
            if cov_outer <= 0.0:
                continue
            d_face = _sd_round_box(px, py, cx, face_cy, face_hw, face_hh, face_r)
            cov_face = _cov(d_face, 0.7)
            # Base keycap: bright top face (1.0) with softer bottom/outer bevel rim (0.52).
            base_alpha = cov_outer * (0.52 + 0.48 * cov_face)
            # Punch out crisp glyph strokes on the keycap face.
            if segs and cov_face > 0.0:
                d_txt = _min_seg_dist(px, py, segs) - stroke
                cov_txt = _cov(d_txt, 0.55)
                base_alpha *= (1.0 - cov_txt * cov_face)
            a = int(round(_clamp01(base_alpha * dim_factor) * 255.0))
            if a > 0:
                o = row_off + x * 4
                buf[o] = 255
                buf[o + 1] = 255
                buf[o + 2] = 255
                buf[o + 3] = a
    return buf


def _render_mouse(w, h, kind):
    """Renders a crisp white-alpha mouse silhouette highlighting the bound mouse button/wheel."""
    buf = bytearray(w * h * 4)
    cx = w * 0.5
    cy = h * 0.5
    body_hw = 13.0
    body_hh = 20.0
    body_r = 11.5

    if kind in ('mouse_x1', 'mouse_x2'):
        cx += 2.5
    elif kind in ('wheel_up', 'wheel_down', 'wheel_right'):
        cx -= 4.0
    elif kind == 'wheel_left':
        cx += 4.0
    elif kind == 'mouse_move':
        body_hw = 10.5
        body_hh = 16.5
        body_r = 9.5

    split_y = cy - 2.0

    # Extra arrow/side segments for wheel or movement prompts.
    extra_segs = []
    if kind == 'wheel_up':
        extra_segs = _arrow_segments_scaled('arrow_up', cx + body_hw + 1.5, cy - 15.0, 10.5, 14.0)
    elif kind == 'wheel_down':
        extra_segs = _arrow_segments_scaled('arrow_down', cx + body_hw + 1.5, cy - 1.0, 10.5, 14.0)
    elif kind == 'wheel_left':
        extra_segs = _arrow_segments_scaled('arrow_left', cx - body_hw - 12.0, cy - 11.5, 10.5, 11.0)
    elif kind == 'wheel_right':
        extra_segs = _arrow_segments_scaled('arrow_right', cx + body_hw + 1.5, cy - 11.5, 10.5, 11.0)
    elif kind == 'mouse_move':
        extra_segs = (
            _arrow_segments_scaled('arrow_left', cx - body_hw - 13.0, cy - 5.5, 11.0, 11.0)
            + _arrow_segments_scaled('arrow_right', cx + body_hw + 2.0, cy - 5.5, 11.0, 11.0)
        )

    wheel_hw = 2.6
    wheel_hh = 5.4
    wheel_cy = split_y - 7.2
    # Side button indicators for X1 (lower thumb) and X2 (upper thumb).
    side_x = cx - body_hw - 2.6

    for y in range(h):
        py = y + 0.5
        row_off = y * w * 4
        for x in range(w):
            px = x + 0.5
            d_body = _sd_round_box(px, py, cx, cy, body_hw, body_hh, body_r)
            cov_body = _cov(d_body, 0.7)
            alpha = 0.0
            if cov_body > 0.0:
                # Crisp outline around entire mouse body + subtle translucent interior.
                border = _cov(abs(d_body + 1.4) - 1.4, 0.65)
                # Horizontal divider & top center divider.
                d_hdiv = abs(py - split_y) - 0.95
                d_vdiv = abs(px - cx) - 0.95 if py < split_y else 99.0
                div_cut = max(_cov(d_hdiv, 0.55), _cov(d_vdiv, 0.55))

                # Scroll wheel cutout + wheel pill.
                d_wheel = _sd_round_box(px, py, cx, wheel_cy, wheel_hw, wheel_hh, 2.2)
                d_wheel_halo = _sd_round_box(px, py, cx, wheel_cy, wheel_hw + 1.8, wheel_hh + 1.8, 3.4)
                cov_wheel = _cov(d_wheel, 0.55)
                cov_wheel_halo = _cov(d_wheel_halo, 0.55)

                # Base body fill (soft alpha 0.34, bright border 0.92).
                base = max(0.32 * cov_body, 0.92 * border * cov_body)
                if kind == 'mouse_move':
                    base = max(0.75 * cov_body, 1.0 * border * cov_body)

                # Highlight active button quadrant brightly (1.0) with clean divider gaps.
                in_top = _cov(py - (split_y - 1.4), 0.55)
                in_left = _cov(px - (cx - 1.4), 0.55)
                in_right = _cov((cx + 1.4) - px, 0.55)

                if kind == 'mouse_left':
                    hi = cov_body * in_top * in_left * (1.0 - cov_wheel_halo)
                    base = max(base, hi)
                elif kind == 'mouse_right':
                    hi = cov_body * in_top * in_right * (1.0 - cov_wheel_halo)
                    base = max(base, hi)

                # Carve dividers & wheel halo cleanly, then draw wheel itself.
                base *= (1.0 - 0.85 * div_cut * (1.0 - border))
                base *= (1.0 - 0.85 * cov_wheel_halo)
                wheel_bright = 1.0 if kind in ('mouse_middle', 'wheel_up', 'wheel_down', 'wheel_left', 'wheel_right') else 0.78
                alpha = max(base, cov_wheel * wheel_bright)

            # Side buttons (X1 lower / X2 upper) on left edge of mouse.
            if kind in ('mouse_x1', 'mouse_x2'):
                d_x2 = _sd_round_box(px, py, side_x, split_y + 1.0, 2.2, 4.4, 1.4)
                d_x1 = _sd_round_box(px, py, side_x, split_y + 11.5, 2.2, 4.4, 1.4)
                alpha = max(alpha, _cov(d_x2, 0.55) * (1.0 if kind == 'mouse_x2' else 0.38))
                alpha = max(alpha, _cov(d_x1, 0.55) * (1.0 if kind == 'mouse_x1' else 0.38))

            if extra_segs:
                d_arr = _min_seg_dist(px, py, extra_segs) - 1.55
                alpha = max(alpha, _cov(d_arr, 0.55))

            a = int(round(_clamp01(alpha) * 255.0))
            if a > 0:
                o = row_off + x * 4
                buf[o] = 255
                buf[o + 1] = 255
                buf[o + 2] = 255
                buf[o + 3] = a
    return buf


def _render_four_key_cluster(w, h, top_lbl, left_lbl, down_lbl, right_lbl):
    """Renders an inverted-T 4-key cluster (used for WASD, Arrow Stick, and D-Pad All)."""
    buf = bytearray(w * h * 4)
    # 3 columns x 2 rows of mini keycaps.
    gap = 2.0
    margin_x = 2.0
    margin_y = 4.0
    kw = (w - 2.0 * margin_x - 2.0 * gap) / 3.0
    kh = (h - 2.0 * margin_y - gap) / 2.0
    hw = kw * 0.5
    hh = kh * 0.5
    rad = 3.2

    cells = (
        (margin_x + kw + gap + hw, margin_y + hh, top_lbl),
        (margin_x + hw, margin_y + kh + gap + hh, left_lbl),
        (margin_x + kw + gap + hw, margin_y + kh + gap + hh, down_lbl),
        (margin_x + 2.0 * (kw + gap) + hw, margin_y + kh + gap + hh, right_lbl),
    )

    cell_segs = []
    for cx, cy, lbl in cells:
        gw = kw * 0.52
        gh = kh * 0.58
        if lbl.startswith('arrow_'):
            segs = _arrow_segments_scaled(lbl, cx - gw * 0.65, cy - gh * 0.6, gw * 1.3, gh * 1.2)
            stroke = 1.25
        else:
            segs = _glyph_segments_scaled(lbl[0] if lbl else '-', cx - gw * 0.5, cy - gh * 0.5, gw, gh)
            stroke = 1.35
        cell_segs.append((cx, cy, segs, stroke))

    for y in range(h):
        py = y + 0.5
        row_off = y * w * 4
        for x in range(w):
            px = x + 0.5
            best_a = 0.0
            for cx, cy, segs, stroke in cell_segs:
                d_box = _sd_round_box(px, py, cx, cy, hw, hh, rad)
                cov_box = _cov(d_box, 0.6)
                if cov_box <= 0.0:
                    continue
                d_txt = _min_seg_dist(px, py, segs) - stroke
                cov_txt = _cov(d_txt, 0.5)
                a = cov_box * (1.0 - cov_txt)
                if a > best_a:
                    best_a = a
            ia = int(round(_clamp01(best_a) * 255.0))
            if ia > 0:
                o = row_off + x * 4
                buf[o] = 255
                buf[o + 1] = 255
                buf[o + 2] = 255
                buf[o + 3] = ia
    return buf


def render_button_glyph(w, h, role, bindings):
    """Renders one button glyph bitmap (w*h*4 RGBA bytes) for the given PS4 button role."""
    src = bindings.get(role)
    if src == '__WASD__':
        return _render_four_key_cluster(w, h, 'W', 'A', 'S', 'D')
    if src == '__ARROWS__':
        return _render_four_key_cluster(w, h, 'arrow_up', 'arrow_left', 'arrow_down', 'arrow_right')
    if src == '__MOUSE_MOVE__':
        return _render_mouse(w, h, 'mouse_move')
    if src == '__DPAD__':
        return _render_four_key_cluster(
            w, h,
            short_cluster_label(bindings.get('up')),
            short_cluster_label(bindings.get('left')),
            short_cluster_label(bindings.get('down')),
            short_cluster_label(bindings.get('right')),
        )
    kind, label = key_display_spec(src)
    if kind.startswith('mouse_') or kind.startswith('wheel_'):
        return _render_mouse(w, h, kind)
    return _render_keycap(w, h, kind, label)


def _blit_rect(atlas, gx, gy, gw, gh, patch):
    for row in range(gh):
        dst_off = ((gy + row) * ATLAS_W + gx) * 4
        src_off = row * gw * 4
        atlas[dst_off:dst_off + gw * 4] = patch[src_off:src_off + gw * 4]


def build_patched_atlas_rgba(base_rgba, key_map='', mouse_enabled=True):
    """Returns a new 512x512 top-down RGBA8 bytearray with all 28 button slots replaced."""
    atlas = bytearray(base_rgba)
    bindings = effective_bindings(key_map=key_map, mouse_enabled=mouse_enabled)
    cache = {}
    for _, gx, gy, gw, gh, role in BUTTON_GLYPHS:
        cache_key = (gw, gh, role)
        if cache_key not in cache:
            cache[cache_key] = render_button_glyph(gw, gh, role, bindings)
        _blit_rect(atlas, gx, gy, gw, gh, cache[cache_key])
    return atlas


def write_png(path, w, h, rgba, dark_bg=None):
    """Writes an RGBA8 buffer as a PNG (optionally composited over an RGB tuple for previews)."""
    if dark_bg is not None:
        comp = bytearray(w * h * 4)
        br, bg, bb = dark_bg
        for i in range(0, len(rgba), 4):
            r, g, b, a = rgba[i], rgba[i + 1], rgba[i + 2], rgba[i + 3]
            comp[i] = (r * a + br * (255 - a)) // 255
            comp[i + 1] = (g * a + bg * (255 - a)) // 255
            comp[i + 2] = (b * a + bb * (255 - a)) // 255
            comp[i + 3] = 255
        data = bytes(comp)
    else:
        data = bytes(rgba)

    def chunk(tag, payload):
        return (
            struct.pack('>I', len(payload))
            + tag
            + payload
            + struct.pack('>I', zlib.crc32(tag + payload) & 0xFFFFFFFF)
        )

    raw = b''.join(b'\x00' + data[y * w * 4:(y + 1) * w * 4] for y in range(h))
    png = (
        b'\x89PNG\r\n\x1a\n'
        + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(raw, 6))
        + chunk(b'IEND', b'')
    )
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    Path(path).write_bytes(png)


def _signature(source_path, key_map, mouse_enabled):
    payload = json.dumps({
        'v': 1,
        'source': str(source_path),
        'bindings': effective_bindings(key_map=key_map, mouse_enabled=mouse_enabled),
    }, sort_keys=True)
    return hashlib.sha256(payload.encode('utf-8')).hexdigest()


def generate(game_dir, out_path, key_map='', mouse_enabled=True, preview_png=None, force=False):
    """Generates out_path (1 MiB Thin_1dThin RGBA8 atlas). Skips work if cached signature matches."""
    out_path = Path(out_path)
    meta_path = out_path.with_suffix(out_path.suffix + '.json')
    source_path, _ = find_lang_atlas_source(game_dir)
    if source_path is None:
        raise FileNotFoundError(
            f'No lang_*_text.xpps containing the 512x512 icon atlas found in {game_dir}'
        )
    sig = _signature(source_path, key_map, mouse_enabled)
    if not force and not preview_png and out_path.is_file() and out_path.stat().st_size == ATLAS_BYTES:
        try:
            if json.loads(meta_path.read_text()).get('signature') == sig:
                return f'Mods: Keyboard prompts atlas up to date ({out_path.name})'
        except (OSError, ValueError):
            pass

    base_rgba, src_path = read_original_atlas(game_dir)
    patched_rgba = build_patched_atlas_rgba(base_rgba, key_map=key_map, mouse_enabled=mouse_enabled)
    tiled = tile_atlas(patched_rgba)

    out_path.parent.mkdir(parents=True, exist_ok=True)
    tmp = out_path.with_suffix(out_path.suffix + '.tmp')
    tmp.write_bytes(tiled)
    tmp.replace(out_path)
    meta_path.write_text(json.dumps({
        'signature': sig,
        'source': src_path.name,
        'mouse_enabled': bool(mouse_enabled),
        'key_map': key_map or '',
        'bindings': effective_bindings(key_map=key_map, mouse_enabled=mouse_enabled),
    }, indent=2))

    if preview_png:
        write_png(preview_png, ATLAS_W, ATLAS_H, patched_rgba, dark_bg=(24, 29, 38))

    return f'Mods: Keyboard prompts atlas generated ({len(BUTTON_GLYPHS)} glyphs -> {out_path.name})'


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game_dir', help='Path to the merged inFAMOUS Second Son game directory')
    parser.add_argument('--out', required=True, help='Output path for the 1 MiB tiled atlas binary')
    parser.add_argument('--key-map', default='', help='Effective ISS_KEY_MAP / tlg.ini key_map string')
    parser.add_argument('--mouse', choices=('0', '1'), default='1', help='1 if mouse input is enabled')
    parser.add_argument('--preview-png', help='Optional dark-background PNG preview path')
    parser.add_argument('--force', action='store_true', help='Regenerate even if cached signature matches')
    args = parser.parse_args(argv)
    msg = generate(
        args.game_dir,
        args.out,
        key_map=args.key_map,
        mouse_enabled=(args.mouse == '1'),
        preview_png=args.preview_png,
        force=args.force,
    )
    print(msg)
    return 0


if __name__ == '__main__':
    sys.exit(main())
