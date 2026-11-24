# SPDX-License-Identifier: GPL-2.0-or-later
"""The currently recommended setup (presets_current.py, RECOMMENDED 2: this port's renderer) on
renderer v2 (port/gpu_v2), with and without its native passes. All three list the renderer, so
applying one undoes the others' choice."""
from presets_current import CURRENT

V2_ON = {
    'id': 'renderer_v2',
    'exported': '2026-10-20',
    'names': {'en': 'RENDERER V2', 'pt': 'RENDERER V2'},
    'what': {
        'en': 'The recommended setup on renderer v2 (experimental): its own shader cache, compiled again '
              'on the first start. Native passes stay off (NVIDIA driver crash).',
        'pt': 'A configuração recomendada no renderer v2 (experimental): cache de shaders próprio, '
              'compilado de novo na primeira partida. Os native passes ficam desligados (crash no driver NVIDIA).',
    },
    'settings': dict(CURRENT['settings']),
    'items': dict(CURRENT['items'], TLG_RENDERER='2', TLG_NATIVE_PASSES='0'),
}

V2_NATIVE = {
    'id': 'renderer_v2_native',
    'exported': '2026-10-20',
    'names': {'en': 'RENDERER V2 + NATIVE PASSES', 'pt': 'RENDERER V2 + NATIVE PASSES'},
    'what': {
        'en': 'RENDERER V2 with its native passes on and unified memory off: the default on Windows (11-20 -> '
              '~40-45 FPS in gameplay on an RTX 3060). On NVIDIA\'s Linux driver: frame drops and crashes.',
        'pt': 'RENDERER V2 com os native passes ligados e a memória unificada desligada: o padrão no Windows '
              '(11-20 -> ~40-45 FPS no gameplay numa RTX 3060). No driver NVIDIA do Linux: quedas de quadros '
              'e travamentos.',
    },
    'settings': dict(CURRENT['settings']),
    'items': dict(CURRENT['items'], TLG_RENDERER='2', TLG_NATIVE_PASSES='1'),
}


def _by_vram(gb, budget_mb):
    """RENDERER V2 + NATIVE PASSES with the texture cache budget of a card of `gb` GB. Renderer v2
    keeps 1.5 GB free for buffers on cards up to 12 GB whatever the budget, and binds only the part
    of the GPU heap the game reads (#497: 5.6 -> 3.1 GB of VRAM on an RTX 2060)."""
    return {
        'id': f'renderer_v2_{gb}gb',
        'exported': '2026-10-22',
        'names': {'en': f'RENDERER V2 · {gb} GB', 'pt': f'RENDERER V2 · {gb} GB'},
        'what': {
            'en': f'RENDERER V2 + NATIVE PASSES for a {gb} GB card: fixed texture cache budget {budget_mb} MB '
                  '(automatic budget off).',
            'pt': f'RENDERER V2 + NATIVE PASSES para placa de {gb} GB: orçamento fixo do cache de texturas '
                  f'{budget_mb} MB (orçamento automático desligado).',
        },
        # A fixed budget: the automatic one (ISS_GC_BUDGET_MB=auto) off.
        'settings': dict(CURRENT['settings'], vram_budget_mb=budget_mb, vram_auto=False),
        'items': dict(V2_NATIVE['items']),
    }


# The launcher's VRAM rule (qt/gfxlogic.py): 8 GB 5000, 12 GB 8500. A 6 GB card takes 3500, the
# default budget: RENDERER V2 + NATIVE PASSES as it stands, so no preset of its own (the launcher
# could not tell the two apart).
V2_8GB = _by_vram(8, 5000)
V2_12GB = _by_vram(12, 8500)

# The sized ones first: a preset without a VRAM budget (V2_NATIVE) matches any budget, so the sized
# one with the player's budget is the one shown as current.
# 2026-11-23: the 8 GB one is replaced by the personal preset (qt/gfxlogic.py, PERSONAL_ID); the other three
# only show under "Experimental presets" (GraphicsPage.qml experimentalPresets).
V2_PRESETS = (V2_12GB, V2_ON, V2_NATIVE)
