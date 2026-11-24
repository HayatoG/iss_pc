# SPDX-License-Identifier: GPL-2.0-or-later
"""The "currently recommended" preset: the settings the maintainer plays with (2026-10-16).

A full preset: besides the switches the four plain presets set (qt/gfxlogic.py), it also sets GPU
accuracy, read-ahead and write merge. The plain presets leave those alone. Draw distance and LOD are never
changed: the preset lists them at the game's native values, so applying it puts them back to native.
Left out on purpose (they depend on the machine): the VRAM budget, window mode, languages, and the
AMD workarounds (BLOCK1D, BC_TINY, NO_MSAA), which stay on "Automatic".
"""
import os

CURRENT = {
    'id': 'current',
    'exported': '2026-10-16',
    'names': {'en': 'OLDER COMPATIBILITY 2', 'pt': 'COMPATIBILIDADE ANTIGA 2'},
    'what': {
        'en': 'The maintainer\'s own setup: exact GPU reads (Precise), read-ahead off, no FSR 1 or sharpening, '
              'and the driver pipeline cache. Changes GPU accuracy too.',
        'pt': 'A configuração do próprio autor: leituras exatas da GPU (Preciso), leitura adiantada desligada, '
              'sem FSR 1 nem nitidez e o cache de pipelines do driver. '
              'Também muda a precisão da GPU.',
    },
    # launcher settings (settings.json): keys the plain presets do not touch, plus VSync
    'settings': {'vsync': True, 'gpu_readbacks': '2', 'eager_readback': False, 'write_merge': True},
    # table items (backports_table.py ids), values as stored strings; a value equal to the default is "off the list"
    'items': {
        'menu:fsr1': '0',   # FSR 1 and sharpening off (2026-10-16): listed so that applying the preset turns them off
        'menu:rcas': '0',
        # draw distance and LOD stay native: listed with the game's own values (the defaults), so applying
        # this preset also sends them back to native if they were changed (lod_bands has no launcher item)
        'menu:lod_scale': '1',
        'menu:region_lod': '1',
        'menu:region_fade': '1',
        'menu:near_clip': '10',
        'menu:far_clip': '10000000',
        'menu:mip_lod_bias': '0',
        'BP06': '1',        # the default; listed so that leaving the AMD preset turns it back on
        'BP07': '1',
        'BP08': '1',
        'TLG_BARRIER_MERGE': '0',
        # this port's renderer (presets_v2.py has the renderer v2 ones, RENDERER V2 + NATIVE PASSES is
        # the Windows default): listed so that applying this preset after RENDERER V2 brings the renderer back
        'TLG_RENDERER': '1',
        'TLG_NATIVE_PASSES': '0',
        # renderer v2's unified memory at its default (backports_table.py: off on Windows), inherited by
        # the renderer v2 presets
        'ISS_UNIFIED_MEMORY': '0' if os.name == 'nt' else '1',
    },
    # in the export for reference, never applied
    'not_applied': {
        'vram_budget_mb': 3500, 'display_mode': 'Windowed', 'block1d': 'auto (1 on this PC)',
        'bc_tiny': 'auto (1 on this PC)', 'no_msaa': 'auto (1 on this PC)', 'engine_fps': 120,
    },
}
