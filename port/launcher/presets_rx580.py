# SPDX-License-Identifier: GPL-2.0-or-later
"""Opt-in RX 580 / Polaris preset: the AMD preset on renderer v2 with its native passes, capped at 30 FPS.

Measured on an RX 580 2048SP + Xeon E5-2650 v4 (Windows, AMD driver 2.0.279, 2026-10-23): renderer v2
with native passes ~30 FPS in gameplay against ~18-27 with renderer v2 alone and ~11-18 with this port's
renderer. AMD's Windows driver on Polaris lacks VK_EXT_vertex_input_dynamic_state; the native passes record
with the pipelines' baked vertex formats there. Barrier merging (#232/#278) stays on for this port's
renderer. Frame generation (menu:frame_gen) is left to the player.
RX 580 work by Aleff Freitas (github.com/Sraleff).
"""
from presets_amd import AMD

RX580 = {
    'id': 'rx580',
    'exported': '2026-10-23',
    'names': {'en': 'RX 580 / POLARIS (EXPERIMENTAL)', 'pt': 'RX 580 / POLARIS (EXPERIMENTAL)'},
    'what': {
        'en': 'Recommended AMD settings on renderer v2 with native passes, capped at 30 FPS: about 30 FPS in gameplay '
              'on an RX 580. Frame generation (Frame rate section) can show it as 60. Restart the game after applying.',
        'pt': 'Configuração recomendada para AMD no renderer v2 com native passes, limitada a 30 FPS: cerca de 30 FPS no '
              'gameplay numa RX 580. A geração de quadros (seção Taxa de quadros) pode mostrar 60. Reinicie o jogo após aplicar.',
    },
    'settings': dict(AMD['settings'], vsync=True),
    'items': dict(AMD['items'], TLG_RENDERER='2', TLG_NATIVE_PASSES='1', **{'menu:fps_limit': '30'},
                  TLG_BARRIER_MERGE='1', TLG_EXPLICIT_RANGE='1', TLG_BARRIER_SPLIT='0'),
}
