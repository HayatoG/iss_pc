# SPDX-License-Identifier: GPL-2.0-or-later
"""The four main presets (2026-11-23): Performance, Balanced, Quality and True to PS4, all on renderer v2
with its native passes (the renderer this port has fixed the most), each tuned for what its name says.

They are full presets: they start from RENDERER V2 + NATIVE PASSES (presets_v2.V2_NATIVE) and set the
frame-rate, image and detail options on top. The output resolution itself is not part of a preset: the
launcher sets it to the monitor once and the player's own choice stays.

No upscaler in any of them (2026-11-23, the owner's call): every preset draws at the native resolution.
FSR 3.1 / DLSS in renderer v2 get camera-only motion vectors (gpu_v2/gpu/iss_temporal_upscaler.cpp), so
moving characters, cars and smoke look grainy; FSR 1 shows up in crash reports. Both stay available on
the Play tab for players who want them.

Every one of them lists every option any of them sets (_BASE, the game's defaults), so switching from one
to another never leaves the previous one's choice behind. That happened on 2026-11-23: True to PS4 kept
Balanced's automatic quality target of 60 while capped at 30, the governor (gpu/shim/tlg_quality.h) found
every second "too slow" and climbed to its lowest detail level (missing textures, PS1-like models).

Automatic quality (menu:quality_target_fps) in none of them: the governor gives detail back only when the
frame rate runs 30% over the target, which a frame cap near the target, or a PC that cannot get there,
never allows, so it only ever takes detail away (down to missing textures at level 14). Fixed quality
floors (a level the game never goes under) are used instead.

Frames queued stays 1 everywhere: renderer v2's defaults.env sets ISS_FRAMES_AHEAD=1, which wins.
"""
from presets_v2 import V2_NATIVE

# The game's own values (backports_table.py defaults) of every option the presets below set. The
# upscaler switches are listed (off) so that a preset also turns off one chosen on the Play tab.
_BASE = {
    'menu:fps_limit': '60', 'menu:frames_queued': '1',
    'menu:fsr1': '0', 'menu:iss_temporal': '0', 'menu:iss_temporal_mode': '0',
    'menu:rcas': '0', 'menu:rcas_strength': '60',
    'menu:quality_target_fps': '0', 'menu:quality_floor': '1',
    'menu:lod_scale': '1', 'menu:mip_lod_bias': '0',
}


def _tuned(pid, vsync, items):
    merged = dict(V2_NATIVE['items'])
    merged.update(_BASE)
    merged.update(items)
    return {'id': pid, 'exported': '2026-11-23',
            'settings': dict(V2_NATIVE['settings'], vsync=vsync), 'items': merged}


# Highest frame rate and quickest response at native resolution: no cap, no VSync, and a fixed lighter
# detail level (quality floor 3: shorter LOD, fewer shadow cascades and small objects; textures stay).
# No automatic quality (2026-11-23): it gives detail back only above 130% of its target, so a PC that
# cannot reach 78 FPS would keep losing detail down to missing textures, as Balanced and PS4 did.
PERFORMANCE = _tuned('performance', False, {
    'menu:fps_limit': '0',
    'menu:quality_floor': '3',
})

# Steady 60 with the game's own detail: 60 FPS cap with VSync, native resolution, nothing experimental.
BALANCED = _tuned('balanced', True, {})

# Best picture around 50-60 FPS at native resolution: light sharpening, detailed models further away and
# the game's best detail level (quality floor 0); 60 FPS cap with VSync. Distant textures stay at the
# game's sharpness (a negative texture bias shimmered in motion).
QUALITY = _tuned('quality', True, {
    'menu:rcas': '1', 'menu:rcas_strength': '40',
    'menu:lod_scale': '1.5',
    'menu:quality_floor': '0',
})

# Like the PS4 Pro: 30 FPS with VSync, the game's own anti-aliasing and detail, no sharpening.
PS4 = _tuned('ps4', True, {
    'menu:fps_limit': '30',
})

# Maxed out (2026-11-23): everything at its maximum in one preset, the owner's request: the best picture
# the game can draw (its best detail level, detailed models four times as far: LOD 4x, the maximum, light
# sharpening) and nothing holding the frame rate back (no cap, no VSync). For strong PCs; LOD 4x is forced
# on every PC (the owner's call, 2026-11-23: no memory fitting for this one) and costs CPU in the city. Distant textures stay at the game's sharpness (a negative bias shimmers in motion).
MAXED = _tuned('maxed', False, {
    'menu:fps_limit': '0',
    'menu:rcas': '1', 'menu:rcas_strength': '40',
    'menu:lod_scale': '4',
    'menu:quality_floor': '0',
})

# Order on the Graphics screen: Performance to Quality, then Maxed out, True to PS4 last.
TUNED_PRESETS = (PERFORMANCE, BALANCED, QUALITY, MAXED, PS4)
TUNED_IDS = tuple(p['id'] for p in TUNED_PRESETS)


# ------------------------------------------------------------------- fitted to the PC (2026-11-23)
# The presets above are the "plenty of memory" versions. for_hardware() lightens them on PCs with less
# video memory (VRAM) or system memory (RAM), so a click gives settings that fit. Unknown memory counts as
# the middle tier. All of them also turn the automatic texture budget on (renderer v2 measures the card
# while playing); the night lighting size already follows the VRAM on its own (Automatic). Without an
# upscaler the presets cannot lower the resolution; lighter detail levels and the game's own model
# distance are what they can save.
#   VRAM: low < 6 GB (3-4 GB cards), mid 6-9 GB (6-8 GB), high >= 10 GB.
#   RAM:  low < 14 GB (8-12 GB),     mid 14-27 GB (16-24 GB), high >= 28 GB (32 GB+).

def tiers(vram_mb, ram_mb):
    vram = 'mid' if not vram_mb else 'low' if vram_mb < 6 * 1024 else 'mid' if vram_mb < 10 * 1024 else 'high'
    ram = 'mid' if not ram_mb else 'low' if ram_mb < 14 * 1024 else 'mid' if ram_mb < 28 * 1024 else 'high'
    return vram, ram


def for_hardware(pid, vram_mb=None, ram_mb=None):
    """(preset dict fitted to this PC, [change keys]) for a tuned preset id. The change keys name what
    was lightened (strings.json gfx_fit_<key>); an empty list means the full preset."""
    base = next(p for p in TUNED_PRESETS if p['id'] == pid)
    settings, items, changes = dict(base['settings'], vram_auto=True), dict(base['items']), []
    vram, ram = tiers(vram_mb, ram_mb)
    if pid == 'maxed':
        pass                                # forced: LOD 4x and best detail on every PC
    elif pid == 'performance':
        if vram == 'low' or ram == 'low':   # one more step lighter than its usual 3
            items['menu:quality_floor'] = '5'; changes.append('lighter_detail')
    elif pid in ('balanced', 'ps4'):
        if vram == 'low' or ram == 'low':   # keep the lighter detail levels (fewer models, lights, shadows)
            items['menu:quality_floor'] = '3'; changes.append('lighter_detail')
    elif pid == 'quality':
        if vram == 'low' or ram == 'low':
            items['menu:lod_scale'] = '1'; items['menu:quality_floor'] = '1'
            changes.append('native_detail')
        elif vram == 'mid' or ram == 'mid':
            items['menu:lod_scale'] = '1.25'; changes.append('models_125')
    return dict(base, settings=settings, items=items), changes
