# SPDX-License-Identifier: GPL-2.0-or-later
"""Presets that only change the draw distance of the currently recommended setup (presets_current.py).

- RECOMMENDED 1: the recommended setup with the draw distance and LOD it used until 2026-10-15 (shorter
  view, sharper textures). In the city it held ~30 FPS where the native view gave 20-22 on a player's PC.
- BARE ("seco"): every draw-distance option at the lowest value of its range (the in-game menu's sliders,
  backports_table.py), textures at their blurriest bias. For weak PCs; the far clip at its minimum cuts the
  view a lot.
The recommended preset itself ("RECOMMENDED 2") keeps the game's native view.
"""
from presets_current import CURRENT

VIEW_15_10 = {'menu:lod_scale': '0.6', 'menu:region_lod': '0', 'menu:region_fade': '0.8',
              'menu:near_clip': '10', 'menu:far_clip': '1418000', 'menu:mip_lod_bias': '1.25'}

# the lowest of each range (backports_table.MENU: lo); near_clip stays native (it is not a distance limit)
VIEW_BARE = {'menu:lod_scale': '0.25', 'menu:region_lod': '0', 'menu:region_fade': '0.25',
             'menu:near_clip': '10', 'menu:far_clip': '1000', 'menu:mip_lod_bias': '3'}

REC1 = {
    'id': 'rec1',
    'exported': '2026-10-16',
    'names': {'en': 'OLDER COMPATIBILITY 1', 'pt': 'COMPATIBILIDADE ANTIGA 1'},
    'what': {
        'en': 'The recommended setup with the shorter draw distance of 15/10: lighter in the city.',
        'pt': 'A configuração recomendada com o alcance de visão menor de 15/10: mais leve na cidade.',
    },
    'settings': dict(CURRENT['settings']),
    'items': dict(CURRENT['items'], **VIEW_15_10),
}

SECO = {
    'id': 'seco',
    'exported': '2026-10-16',
    'names': {'en': 'BARE', 'pt': 'SECO'},
    'what': {
        'en': 'BARE-BONES MACHINES, USE THIS ONE. The recommended setup with every draw-distance option at its minimum.',
        'pt': 'MÁQUINAS SECAS E LISAS, USEM ESSE. A configuração recomendada com tudo de distância no mínimo.',
    },
    'settings': dict(CURRENT['settings']),
    'items': dict(CURRENT['items'], **VIEW_BARE),
}
