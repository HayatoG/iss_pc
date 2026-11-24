# SPDX-License-Identifier: GPL-2.0-or-later
"""Graphics screen logic that does not need Qt: presets, the VRAM budget rule and VRAM detection.

Presets only set switches that already exist (the launcher's Video box and the table in
backports_table.py, which are also the options of the in-game Insert menu); none adds a key.
"""
import glob
import os
import re
import subprocess
from pathlib import Path

import backports_table
from presets_current import CURRENT
from presets_amd import AMD
from presets_rx580 import RX580
from presets_view import REC1, SECO
from presets_apolar import APOLAR_PRESETS
from presets_v2 import V2_PRESETS
from presets_tuned import TUNED_PRESETS, TUNED_IDS, for_hardware

# This PC's memory in MB (2026-11-23): the tuned presets are fitted to it (presets_tuned.for_hardware).
# Filled by the Graphics screen (VRAM when detected, remembered in settings.json; RAM read at start).
HARDWARE = {'vram_mb': None, 'ram_mb': None}

# ------------------------------------------------------------------ personal preset (2026-11-23)
# "Renderer V2 - Custom Personal Graphics": renderer v2 with native passes, everything else the player's.
# While it is the selected preset (settings['graphics_mode'] == PERSONAL_ID) every graphics option the
# player changes is remembered in settings['personal_graphics']; clicking it again restores them.
PERSONAL_ID = 'personal'
PERSONAL_BASE = {'TLG_RENDERER': '2', 'TLG_NATIVE_PASSES': '1'}
PERSONAL_SETTING_KEYS = ('vsync', 'display_mode', 'allow_hdr', 'show_fps', 'vram_auto', 'vram_budget_mb',
                         'gpu_readbacks', 'v2_lighting_read_window', 'v2_amd_native_passes')


def personal_item_ids():
    """The graphics options the personal preset remembers: every Image, View and Performance option of
    the in-game menu plus the renderer switches (not controls, overlay or fixes)."""
    ids = [i.id for i in backports_table.ITEMS if i.group == 'menu' and i.tab in ('Image', 'View', 'Performance')]
    ids += [i.id for i in backports_table.ITEMS if i.id.startswith('menu:iss_temporal')]
    ids += ['TLG_RENDERER', 'TLG_NATIVE_PASSES', 'TLG_BARRIER_MERGE']
    return tuple(dict.fromkeys(i for i in ids if any(x.id == i for x in backports_table.ITEMS)))


def personal_capture(settings, states):
    """The player's current graphics options, as the personal preset keeps them."""
    return {'settings': {k: settings[k] for k in PERSONAL_SETTING_KEYS if k in settings},
            'items': {i: states.get(i) for i in personal_item_ids()}}


def personal_targets(settings, states):
    """(settings, items) the personal preset applies: the remembered options, or on the first click the
    current ones with renderer v2 and native passes on."""
    saved = settings.get('personal_graphics')
    if isinstance(saved, dict) and isinstance(saved.get('items'), dict):
        items = {i: saved['items'].get(i) for i in personal_item_ids()}
        return dict(saved.get('settings') or {}), items
    items = {i: states.get(i) for i in personal_item_ids()}
    for item_id, value in PERSONAL_BASE.items():
        items[item_id] = backports_table.normalize(item_by_id(item_id), value)
    return {}, items


def remember_personal(settings, states):
    """While the personal preset is selected, keeps the player's latest choices (the caller saves)."""
    if settings.get('graphics_mode') == PERSONAL_ID:
        settings['personal_graphics'] = personal_capture(settings, states)

# ------------------------------------------------------------------------------------- presets
# Keys a preset sets (anything a preset does not list goes back to the default of the table).
PRESET_SETTINGS = ('vsync',)
PRESET_ITEMS = ('menu:fps_limit', 'menu:frames_queued', 'menu:fsr1', 'menu:rcas', 'menu:rcas_strength',
                'TLG_BARRIER_MERGE', 'TLG_EXPLICIT_RANGE', 'TLG_BARRIER_SPLIT',
                'TLG_ASYNC_SHADERS', 'TLG_STALE_READBACK', 'TLG_DEFERRED_READBACK', 'TLG_BACKGROUND_WARMUP')
# Full presets (presets_current.py) also set keys the plain ones never touch (GPU accuracy, read-ahead,
# write merge, draw distance...): those keys are applied, and compared, only for the preset that lists them.
FULL_PRESETS = {p['id']: p for p in (REC1, CURRENT, AMD, RX580, SECO, *APOLAR_PRESETS, *V2_PRESETS, *TUNED_PRESETS)}
# Defaults of the launcher settings a full preset may set (infamous_run.DEFAULT_SETTINGS; a test keeps them equal).
SETTING_DEFAULTS = {'vsync': True, 'gpu_readbacks': '2', 'eager_readback': False, 'write_merge': True,
                    'show_fps': False, 'precompile_shaders': True, 'vram_budget_mb': 3500,
                    'vram_auto': True}
# id, settings overrides, item overrides (values as strings; unlisted = default)
PRESETS = (
    (PERSONAL_ID, {}, {}),      # applied by personal_targets (the player's own options), never compared
    (REC1['id'], REC1['settings'], REC1['items']),
    (CURRENT['id'], CURRENT['settings'], CURRENT['items']),
    (AMD['id'], AMD['settings'], AMD['items']),
    (RX580['id'], RX580['settings'], RX580['items']),
    (SECO['id'], SECO['settings'], SECO['items']),
    *((p['id'], p['settings'], p['items']) for p in APOLAR_PRESETS),
    *((p['id'], p['settings'], p['items']) for p in V2_PRESETS),
    # Performance, Balanced, Quality, True to PS4 (2026-11-23): renderer v2 + native passes, tuned (presets_tuned.py)
    *((p['id'], p['settings'], p['items']) for p in TUNED_PRESETS),
)
DEFAULT_PRESET = 'balanced'


def fit_notes(preset_id):
    """The change keys of a tuned preset on this PC ([] = full preset or not a tuned one)."""
    if preset_id not in TUNED_IDS:
        return []
    return for_hardware(preset_id, HARDWARE['vram_mb'], HARDWARE['ram_mb'])[1]


def item_by_id(item_id):
    return next(i for i in backports_table.ITEMS if i.id == item_id)


def _full_targets(overrides, items):
    """Targets of a full preset: the plain keys plus every key the preset lists."""
    settings = {'vsync': overrides.get('vsync', True)}
    stored = {item_id: backports_table.normalize(item_by_id(item_id), items.get(item_id))
              for item_id in PRESET_ITEMS}
    settings.update(overrides)
    for item_id, value in items.items():
        stored[item_id] = backports_table.normalize(item_by_id(item_id), value)
    return settings, stored


def preset_targets(preset_id):
    """({settings key: value}, {item id: stored value or None}) for a preset: every key a preset
    can set, so applying one also undoes what another set."""
    if preset_id in TUNED_IDS:         # fitted to this PC's VRAM and RAM
        fitted, _ = for_hardware(preset_id, HARDWARE['vram_mb'], HARDWARE['ram_mb'])
        return _full_targets(fitted['settings'], fitted['items'])
    _, overrides, items = next(p for p in PRESETS if p[0] == preset_id)
    if preset_id in FULL_PRESETS:      # only a full preset owns the extra keys
        return _full_targets(overrides, items)
    stored = {item_id: backports_table.normalize(item_by_id(item_id), items.get(item_id))
              for item_id in PRESET_ITEMS}
    return {'vsync': overrides.get('vsync', True)}, stored


def custom_targets(preset):
    """The targets of a preset that is not in PRESETS (an imported file, see preset_share.py): it is
    applied as a full preset. `preset` has the layout of presets_current.CURRENT."""
    return _full_targets(preset.get('settings', {}), preset.get('items', {}))


def apply_targets(settings, states, targets):
    """Puts the targets of a preset into the launcher settings and the fix states (in place); the
    caller saves them (Backend._save_fixes in Qt, tlg_run.save_experimental in Tk)."""
    settings.update(targets[0])
    states.update(targets[1])


def same_setting(key, current, wanted):
    return _same_setting(key, current, wanted)


def _same_setting(key, current, wanted):
    default = SETTING_DEFAULTS.get(key, True)
    value = default if current is None else current
    return str(value).lower() == str(wanted).lower()


def matching_preset(settings, states):
    """The id of the preset the current switches equal, or None (custom). `states` = fix states.
    The tuned main presets are checked first (2026-11-23): they set more keys than RENDERER V2 + NATIVE
    PASSES, which would otherwise also match Balanced and take its name."""
    if settings.get('graphics_mode') == PERSONAL_ID:      # the player's own setup, whatever it holds
        return PERSONAL_ID
    tuned = [p['id'] for p in TUNED_PRESETS]
    for preset_id in tuned + [p[0] for p in PRESETS if p[0] not in tuned and p[0] != PERSONAL_ID]:
        want_settings, want_items = preset_targets(preset_id)
        if all(_same_setting(k, settings.get(k), v) for k, v in want_settings.items()) and \
           all(states.get(i) == v for i, v in want_items.items()):
            return preset_id
    return None


# --------------------------------------------------------------------------------- VRAM budget
# The Tk launcher's rule ("6 GB card: 3500 · 8 GB: 5000 · 12 GB: 8500 · 16 GB: 12000 · 24 GB: 19000"),
# in MB of VRAM to give the texture cache; in between the budget follows the line joining the points.
BUDGET_POINTS = ((6, 3500), (8, 5000), (12, 8500), (16, 12000), (24, 19000))
BUDGET_MIN, BUDGET_MAX = 2000, 24000      # the Tk spin box range


def suggested_budget_mb(vram_gb):
    """MB of texture-cache budget for a card with this much VRAM (rounded to 100)."""
    gb = float(vram_gb)
    points = BUDGET_POINTS
    if gb <= points[0][0]:
        value = points[0][1] * gb / points[0][0]
    elif gb >= points[-1][0]:
        value = points[-1][1] + (gb - points[-1][0]) * 800
    else:
        value = points[-1][1]
        for (g0, b0), (g1, b1) in zip(points, points[1:]):
            if g0 <= gb <= g1:
                value = b0 + (b1 - b0) * (gb - g0) / (g1 - g0)
                break
    value = min(value, gb * 1024 * 0.85)   # never close to the whole card
    return int(min(max(round(value / 100) * 100, BUDGET_MIN), BUDGET_MAX))


def budget_is_too_high(budget_mb, vram_mb):
    """True when the budget leaves less than ~10% of the VRAM to the rest of the game."""
    return bool(vram_mb) and budget_mb > vram_mb * 0.9


# --------------------------------------------------------------------------- VRAM detection
def parse_nvidia_smi(text):
    """MB of the largest card in `nvidia-smi --query-gpu=memory.total --format=csv,noheader,nounits`."""
    sizes = [int(m) for m in re.findall(r'^\s*(\d+)\s*$', text or '', re.M)]
    return max(sizes) if sizes else None


def parse_vulkan_heaps(text, device=None):
    """MB of the biggest DEVICE_LOCAL heap in `vulkaninfo` output, of the device whose name
    contains `device` (else of the largest device)."""
    best, name, size, local, heap = {}, '', None, False, False

    def close():
        if heap and size and local:
            best[name] = max(best.get(name, 0), size)
    for line in (text or '').splitlines():
        m = re.match(r'\s*deviceName\s*=\s*(.+?)\s*$', line)
        if m:
            close()
            size, local, heap, name = None, False, False, m.group(1)
            continue
        if re.match(r'\s*memoryHeaps\[\d+\]:', line):
            close()
            size, local, heap = None, False, True
            continue
        if not heap:
            continue
        m = re.match(r'\s*size\s*=\s*(\d+)', line)
        if m and size is None:
            size = int(m.group(1))
        elif 'MEMORY_HEAP_DEVICE_LOCAL_BIT' in line:
            local = True
        elif re.match(r'\s*memoryTypes\[', line) or re.match(r'\s*memoryHeapCount', line):
            close()
            size, local, heap = None, False, False
    close()
    if device:
        for found, value in best.items():
            if device.lower() in found.lower() or found.lower() in device.lower():
                return value // (1024 * 1024)
    return max(best.values()) // (1024 * 1024) if best else None


def read_sysfs_vram(pattern='/sys/class/drm/card*/device/mem_info_vram_total'):
    """MB of the largest amdgpu card (Linux, no tools needed)."""
    sizes = []
    for path in glob.glob(pattern):
        try:
            sizes.append(int(Path(path).read_text().strip()))
        except (OSError, ValueError):
            pass
    return max(sizes) // (1024 * 1024) if sizes else None


def detect_vram_mb(gpu_name=None, run=subprocess.run, sysfs=read_sysfs_vram, timeout=15):
    """VRAM in MB: nvidia-smi, then the amdgpu sysfs file, then vulkaninfo's memory heaps.
    None when none of them answers. Slow (a process): call it off the UI thread."""
    flags = {'stdin': subprocess.DEVNULL, 'capture_output': True, 'text': True, 'timeout': timeout,
             'creationflags': getattr(subprocess, 'CREATE_NO_WINDOW', 0)}
    try:
        value = parse_nvidia_smi(run(['nvidia-smi', '--query-gpu=memory.total', '--format=csv,noheader,nounits'],
                                     **flags).stdout)
        if value:
            return value
    except (OSError, subprocess.SubprocessError):
        pass
    value = sysfs()
    if value:
        return value
    try:
        env = dict(os.environ)
        try:
            import tlg_vulkan
            tlg_vulkan.configure(env)
        except Exception:
            pass
        return parse_vulkan_heaps(run([os.environ.get('TLG_VULKANINFO', 'vulkaninfo')], env=env, **flags).stdout,
                                  gpu_name)
    except (OSError, subprocess.SubprocessError):
        return None
