# SPDX-License-Identifier: GPL-2.0-or-later
"""Every switch the launcher's "Experimental fixes" window offers, in three groups:
  MENU         the options of the in-game menu (Insert) that the launcher has no control for yet;
  BACKPORTS    the experimental fixes BP01-BP12 (gpu/shadps4/common/backports.h);
  DIAGNOSTICS  the older environment-only A/B switches.

Single source: tests/test_backports_table.py compares the BPnn rows with backports.h (id, env,
default, shader, live), checks that every TLG_* name of the diagnostic switches exists in the
sources and the menu keys in the settings code, so this table cannot get old.

State model: every item has a default (what the game uses when nothing is set). The player's
choice is kept only when it differs from the default, as a key of tlg.ini (`bp01=0`,
`overlay_corner=3`; the same keys and formats the game's own menu writes), and is removed when
it goes back to the default. The BPnn and A/B switches reach the game as ISS_<name>=0/1 (the
probe copies ISS_X to TLG_X, like the rest of the launcher); the menu keys are read from tlg.ini
by the game itself. A switch the player never touched changes nothing.
All values are strings, as in the ini: '0'/'1' for on/off.
"""
import os
from collections import namedtuple
from pathlib import Path

_WINDOWS = os.name == 'nt'

# group: 'menu' | 'bp' | 'ab'.  vtype: 'bool' | 'choice' | 'int' | 'float'.
# default: '0'/'1' (bool) or the value text; None = automatic (the GPU backend decides per driver).
# store: 'ini' (tlg.ini key) or 'launcher' (settings.json, already used by infamous_run.build_env).
# pass_env: give ISS_<suffix> to the game; False when the game reads the ini key itself (the
#   in-game menu writes it too, and an environment variable would override the menu).
# choices: ((value, en, pt), ...) for 'choice'; lo/hi/unit for 'int'.
Item = namedtuple('Item', 'id group key env vtype default choices lo hi unit shader live store '
                          'pass_env risky broken name what source tab')
Item.__new__.__defaults__ = ()


def _make(id, group, key, env, vtype, default, name, what, *, choices=(), lo=0, hi=0, unit='',
          shader=False, live=False, store='ini', pass_env=False, risky=False, broken=False,
          source='', tab=''):
    return Item(id, group, key, env, vtype, default, choices, lo, hi, unit, shader, live, store,
                pass_env, risky, broken, name, what, source, tab)


def _bp(n, default, shader, live, name, what, source, risky=False):
    return _make(f'BP{n:02d}', 'bp', f'bp{n:02d}', f'TLG_BP{n:02d}', 'bool', '1' if default else '0',
                 name, what, shader=shader, live=live, pass_env=True, risky=risky, source=source)


def _ab(key, env, default, shader, live, name, what, store='ini', pass_env=True, broken=False):
    return _make(env, 'ab', key, env, 'bool', None if default is None else '1' if default else '0',
                 name, what, shader=shader, live=live, store=store, pass_env=pass_env, broken=broken)


def _ab_choice(key, env, default, choices, name, what, **kw):
    kw.setdefault('pass_env', True)
    return _make(env, 'ab', key, env, 'choice', default, name, what, choices=choices, **kw)


def _menu(tab, key, vtype, default, name, what, **kw):
    return _make('menu:' + key, 'menu', key, '', vtype, default, name, what, live=True, tab=tab, **kw)



# The in-game menu (Insert). Display mode, VSync and the FPS overlay's on/off switch are in the
# launcher's own Video box and not repeated here. Every menu option takes effect on the next frame.
_CORNERS = (('0', 'Top left', 'Superior esquerdo'), ('1', 'Top right', 'Superior direito'),
            ('2', 'Bottom left', 'Inferior esquerdo'), ('3', 'Bottom right', 'Inferior direito'))
MENU = [
    _menu('Image', 'fsr1', 'bool', '0', ('Upscale with FSR 1', 'Ampliar com FSR 1'),
          ('Enlarges the game image when it is smaller than the window (spatial filter, no shimmer).',
           'Amplia a imagem do jogo quando ela é menor que a janela (filtro espacial, sem tremor).')),
    _menu('Image', 'rcas', 'bool', '1', ('Sharpening (RCAS)', 'Nitidez (RCAS)'),
          ('Sharpens the final image. Needs FSR 1 when the game image is smaller than the window.',
           'Deixa a imagem final mais nítida. Precisa do FSR 1 quando a imagem do jogo é menor que a janela.')),
    _menu('Image', 'rcas_strength', 'int', '60', ('Sharpening strength', 'Intensidade da nitidez'),
          ('How strong the sharpening is.', 'Quão forte é a nitidez.'), lo=0, hi=100, unit='%'),
    # Draw distance (gpu/shim/tlg_view.h): the in-game menu's sliders; the same ranges.
    _menu('View', 'lod_scale', 'float', '1', ('LOD distance', 'Distância do LOD'),
          ('How far objects keep their detailed models (every LOD band). Above 1x costs CPU and GPU; '
           'the game still lowers it on its own in heavy scenes.',
           'Até onde os objetos mantêm os modelos detalhados (todas as faixas de LOD). Acima de 1x pesa na CPU e '
           'na GPU; o jogo ainda reduz sozinho em cenas pesadas.'), lo=0.25, hi=4.0, unit='x'),
    _menu('View', 'region_lod', 'float', '1', ("Regions' LOD_ADJUST", 'LOD_ADJUST das regiões'),
          ("How much the map regions' own LOD changes count: 1 as in the game, 0 ignored.",
           'Quanto contam as mudanças de LOD das regiões do mapa: 1 como no jogo, 0 ignoradas.'), lo=0.0, hi=1.0),
    _menu('View', 'region_fade', 'float', '1', ("Regions' FADE_DISTANCE", 'FADE_DISTANCE das regiões'),
          ("Distance over which a region's lighting, haze and LOD settings blend in (multiplier).",
           'Distância em que a luz, a névoa e o LOD de uma região se misturam ao entrar nela (multiplicador).'),
          lo=0.25, hi=4.0, unit='x'),
    _menu('View', 'near_clip', 'float', '10', ('Near clip (NEAR_CLIP)', 'Plano próximo (NEAR_CLIP)'),
          ('Default near plane of the cameras (the game uses 10). Lower shows objects closer to the camera, '
           'higher reduces depth flicker far away.',
           'Plano próximo padrão das câmeras (o jogo usa 10). Menor mostra objetos mais perto da câmera, '
           'maior reduz o tremido de profundidade ao longe.'), lo=1.0, hi=100.0),
    _menu('View', 'far_clip', 'float', '10000000', ('Far clip (FAR_CLIP)', 'Plano distante (FAR_CLIP)'),
          ('Default far plane of the cameras (the game uses 10000000, unlimited). Lower cuts the view.',
           'Plano distante padrão das câmeras (o jogo usa 10000000, sem limite). Menor corta a visão.'),
          lo=1000.0, hi=10000000.0),
    _menu('View', 'mip_lod_bias', 'float', '0', ('Texture LOD bias (MipLodBias)', 'Bias de LOD das texturas (MipLodBias)'),
          ('Negative: sharper textures at a distance (more shimmer, a little more GPU). Positive: blurrier.',
           'Negativo: texturas mais nítidas ao longe (mais cintilação, um pouco mais de GPU). Positivo: mais borradas.'),
          lo=-3.0, hi=3.0),
    _menu('Performance', 'fps_limit', 'choice', '0', ('Frame rate limit', 'Limite de FPS'),
          ('Caps the presented frames. The game runs at 30 FPS (as on PS4).',
           'Limita os quadros apresentados. O jogo roda a 30 FPS (como no PS4).'),
          choices=(('24', '24', '24'), ('30', '30', '30'), ('60', '60', '60'), ('120', '120', '120'), ('240', '240', '240'),
                   ('0', 'Unlimited', 'Sem limite'))),
    _menu('Performance', 'frames_queued', 'choice', '1', ('Frames queued', 'Quadros na fila'),
          ('More frames queued: steadier FPS, slower response to input.',
           'Mais quadros na fila: FPS mais estável, resposta mais lenta.'),
          choices=(('1', '1 (lowest latency)', '1 (menor atraso)'), ('2', '2', '2'),
                   ('0', 'Unlimited', 'Sem limite'))),
    _menu('Controls', 'sensor_aim', 'int', '100', ('Graffiti aiming speed', 'Velocidade da mira do grafite'),
          ('How fast the stick moves the graffiti cursor in sensor mode (100% = default).',
           'Quão rápido o analógico move o cursor do grafite no modo sensor (100% = padrão).'),
          lo=25, hi=200, unit='%'),
    _menu('Overlay', 'overlay_fps', 'bool', '1', ('Overlay: FPS', 'Overlay: FPS'),
          ('Shows the frame rate in the performance overlay.', 'Mostra os quadros por segundo no overlay.')),
    _menu('Overlay', 'overlay_frametime', 'bool', '1', ('Overlay: frame time', 'Overlay: tempo de quadro'),
          ('Shows the time of each frame.', 'Mostra o tempo de cada quadro.')),
    _menu('Overlay', 'overlay_upscaler', 'bool', '0', ('Overlay: upscaler', 'Overlay: upscaler'),
          ('Shows the active upscaler.', 'Mostra o upscaler ativo.')),
    _menu('Overlay', 'overlay_gpu', 'bool', '1', ('Overlay: GPU', 'Overlay: GPU'),
          ('Shows the GPU name and use.', 'Mostra o nome e o uso da GPU.')),
    _menu('Overlay', 'overlay_cpu', 'bool', '0', ('Overlay: CPU', 'Overlay: CPU'),
          ('Shows the game\'s CPU use.', 'Mostra o uso de CPU do jogo.')),
    _menu('Overlay', 'overlay_ram', 'bool', '0', ('Overlay: RAM', 'Overlay: RAM'),
          ('Shows RAM used by the game and total.', 'Mostra a RAM usada pelo jogo e a total.')),
    _menu('Overlay', 'overlay_vram', 'bool', '1', ('Overlay: VRAM', 'Overlay: VRAM'),
          ('Shows video memory used and available.', 'Mostra a memória de vídeo usada e disponível.')),
    _menu('Overlay', 'overlay_scale', 'int', '100', ('Overlay font size', 'Tamanho da fonte do overlay'),
          ('Size of the overlay text.', 'Tamanho do texto do overlay.'), lo=50, hi=300, unit='%'),
    _menu('Overlay', 'overlay_corner', 'choice', '1', ('Overlay corner', 'Canto do overlay'),
          ('Where the overlay sits.', 'Onde o overlay fica.'), choices=_CORNERS),
    _menu('Overlay', 'overlay_opacity', 'int', '50', ('Overlay background opacity', 'Opacidade do fundo do overlay'),
          ('0% is no background.', '0% é sem fundo.'), lo=0, hi=100, unit='%'),
    _menu('Overlay', 'overlay_layout', 'choice', '0', ('Overlay layout', 'Disposição do overlay'),
          ('Items side by side or stacked.', 'Itens lado a lado ou empilhados.'),
          choices=(('0', 'One line', 'Uma linha'), ('1', 'One item per line', 'Um item por linha'))),
]

BACKPORTS = [
    _bp(1, True, False, True, ('Free nested command buffers', 'Liberar buffers de comando aninhados'),
        ('Frees the nested indirect command buffers of each frame; stops a slow memory leak.',
         'Libera os buffers de comando indiretos aninhados de cada quadro; evita um vazamento lento de memória.'),
        'shadPS4 #5258'),
    _bp(2, True, False, True, ('Faster texture lookup', 'Busca de texturas mais rápida'),
        ('Looks a texture up by its first memory page only; less CPU time finding images.',
         'Procura a textura só pela primeira página de memória; menos CPU gasta achando imagens.'),
        'shadPS4 145932bf (#5240)'),
    _bp(3, True, True, False, ('Keep EXEC after empty branch', 'Manter o EXEC após desvio vazio'),
        ('Fixes shaders that lost lanes after a divergent branch that did nothing (wrong or missing pixels).',
         'Corrige shaders que perdiam lanes depois de um desvio divergente que não fazia nada (pixels errados ou faltando).'),
        'shadPS4 c34ca21f'),
    _bp(4, True, True, False, ('Reciprocal/log without clamp', 'Recíproco/log sem clamp'),
        ('The RCP, RSQ and LOG shader instructions no longer clamp their result, as the PS4 GPU does.',
         'As instruções RCP, RSQ e LOG dos shaders deixam de limitar o resultado, como a GPU do PS4.'),
        'shadPS4 1c2b8f97 (#5346)'),
    _bp(5, True, True, False, ('Saturate keeps NaN', 'Saturate mantém NaN'),
        ('Saturate keeps NaN values like the PS4 GPU; may fix black or flashing pixels.',
         'O saturate mantém valores NaN como a GPU do PS4; pode corrigir pixels pretos ou piscando.'),
        'shadPS4 #5257'),
    _bp(6, True, True, False, ('Maximal reconvergence', 'Reconvergência máxima'),
        ('Asks the driver for strict thread reconvergence in compute shaders; may fix flicker, may fail on some drivers.',
         'Pede ao driver reconvergência estrita de threads nos shaders de compute; pode corrigir flicker, pode falhar em alguns drivers.'),
        'Shadow of the Colossus port 8d84cf7', True),
    _bp(7, False, False, True, ('Async compile never skips draws', 'Compilação assíncrona sem pular draws'),
        ('While a pipeline compiles in the background, full-screen and indirect draws are no longer skipped: '
         'fewer missing effects, more hitches.',
         'Enquanto um pipeline compila em segundo plano, draws de tela cheia e indiretos não são mais pulados: '
         'menos efeitos faltando, mais engasgos.'),
        'bbport 10baf9d', True),
    _bp(8, True, False, False, ('Driver pipeline cache', 'Cache de pipelines do driver'),
        ('Saves the driver\'s own pipeline cache to disk: pipelines are created at once on the next start, so no draw is skipped while one compiles. On by default since 2026-10-16; adds files and depends on the driver.',
         'Salva o cache de pipelines do próprio driver em disco: na próxima abertura as pipelines são criadas na hora e nenhum draw é pulado enquanto uma compila. Ligado por padrão desde 16/10/2026; cria arquivos e depende do driver.'),
        'Mrsuss b972798, 0xCydral bdb9769', True),
    _bp(9, False, True, False, ('Every 1D image as 2D', 'Toda imagem 1D como 2D'),
        ('Every 1D texture becomes a 2D image of height 1; a workaround for drivers that mishandle 1D.',
         'Toda textura 1D vira uma imagem 2D de altura 1; contorno para drivers que tratam mal o 1D.'),
        'CTR GetLostIn d26bcb2', True),
    _bp(10, True, False, True, ('Vertex/index buffer barriers', 'Barreiras de buffers de vértice/índice'),
        ('Adds memory barriers for vertex, index and indirect buffers; fixes rare corrupted geometry.',
         'Adiciona barreiras de memória para buffers de vértice, índice e indiretos; corrige geometria corrompida rara.'),
        'shadPS4 #5262'),
    _bp(11, True, True, False, ('First instance counted once', 'First instance contada uma vez'),
        ('The first instance is counted once in indirect draws; fixes wrong per-instance data.',
         'A first instance é contada uma vez nos draws indiretos; corrige dados por instância errados.'),
        'shadPS4 #5348'),
    _bp(12, False, True, False, ('Early fragment tests', 'Teste antecipado de fragmento'),
        ('Turns on early fragment tests when the game asks for depth/stencil before the pixel shader; '
         'fixes wrong overdraw and, in some scenes, missing or doubled effects.',
         'Liga o teste antecipado de fragmento quando o jogo pede profundidade/stencil antes do pixel shader; '
         'corrige sobreposição errada e, em algumas cenas, efeitos faltando ou duplicados.'),
        'shadPS4 #5264 (7d28d279)'),
]

# The older A/B switches (env names checked against the sources by the test). Defaults are the
# ones the game uses in this port; the LDS family is on for inFAMOUS only (recompiler_toggles.h).
DIAGNOSTICS = [
    _ab('wqm', 'TLG_WQM', True, True, False, ('Whole quad mode', 'Modo quad inteiro'),
        ('S_WQM_B64 grows the lane mask to whole quads (it used to do nothing).',
         'S_WQM_B64 estende a máscara de lanes a quads inteiros (antes não fazia nada).')),
    _ab('lds_small', 'TLG_LDS_SMALL', True, True, False, ('LDS barriers, small groups', 'Barreiras LDS, grupos pequenos'),
        ('Shared-memory barriers in compute groups under 64 threads (GPU smoke).',
         'Barreiras de memória compartilhada em grupos de compute com menos de 64 threads (fumaça da GPU).')),
    _ab('lds_multi', 'TLG_LDS_MULTI', True, True, False, ('LDS barriers, large groups', 'Barreiras LDS, grupos grandes'),
        ('Shared-memory barriers in compute groups over 64 threads.',
         'Barreiras de memória compartilhada em grupos de compute com mais de 64 threads.')),
    _ab('lds_readlane', 'TLG_LDS_READLANE', True, True, False, ('LDS: constant ReadLane is uniform', 'LDS: ReadLane constante é uniforme'),
        ('A ReadLane of a constant lane counts as uniform when placing LDS barriers.',
         'Um ReadLane de lane constante conta como uniforme ao posicionar as barreiras LDS.')),
    _ab('lane_divergence', 'TLG_LANE_DIVERGENCE', True, True, False, ('Lane-varying branches', 'Desvios que variam por lane'),
        ('Branches that depend on lane id, v_mbcnt or atomic results count as divergent, so no barrier lands inside them.',
         'Desvios que dependem do id da lane, do v_mbcnt ou de resultado de atomic contam como divergentes, para nenhuma barreira cair neles.')),
    _ab('rfl_all', 'TLG_RFL_ALL', False, True, False, ('ReadFirstLane in every wide shader', 'ReadFirstLane em todo shader largo'),
        ('Experiment: lowers v_readfirstlane in every wide compute shader (two extra barriers each).',
         'Experimento: reescreve o v_readfirstlane em todo shader de compute largo (duas barreiras a mais cada).')),
    _ab('dynamic_stride', 'TLG_DYNAMIC_STRIDE', True, True, False, ('Compute buffer strides at run time', 'Stride dos buffers de compute em tempo de execução'),
        ('Compute shaders read each buffer\'s stride from the GPU descriptor instead of compiling it in, so one compiled shader serves every stride. Stops the GPU smoke from flickering (#7, #50).',
         'Os shaders de compute leem o stride de cada buffer do descritor da GPU em vez de compilá-lo, então um shader compilado serve para todos os strides. Acaba com a fumaça da GPU piscando (#7, #50).')),
    _ab('block1d', 'TLG_BLOCK1D', None, True, False, ('BC 1D textures as 2D', 'Texturas BC 1D como 2D'),
        ('Block-compressed 1D textures as 2D images. Automatic: on for drivers that refuse them, except NVIDIA.',
         'Texturas 1D comprimidas como imagens 2D. Automático: ligado nos drivers que as recusam, exceto NVIDIA.')),
    _ab('bc_tiny', 'TLG_BC_TINY', None, True, False, ('Tiny BC textures on the CPU', 'Texturas BC minúsculas na CPU'),
        ('Decodes tiny block-compressed textures on the CPU. Automatic: on for AMD\'s proprietary driver only.',
         'Decodifica na CPU as texturas comprimidas minúsculas. Automático: ligado só no driver proprietário da AMD.')),
    _ab('no_msaa', 'TLG_NO_MSAA', None, True, False, ('Disable multisampling', 'Desligar multisampling'),
        ('Single-sampled images instead of the guest\'s MSAA. Automatic: on for AMD and Intel on Windows.',
         'Imagens sem multisampling no lugar do MSAA do jogo. Automático: ligado em AMD e Intel no Windows.')),
    _ab('async_shaders', 'TLG_ASYNC_SHADERS', False, False, True, ('Asynchronous shader compile', 'Compilação assíncrona de shaders'),
        ('Compiles pipelines off the GPU thread: fewer hitches, but a draw whose pipeline is not ready yet is skipped and the object flashes for a frame. Off by default since 2026-10-16. Also in the in-game menu.',
         'Compila pipelines fora da thread da GPU: menos engasgos, mas um draw cuja pipeline ainda não está pronta é pulado e o objeto pisca por um quadro. Desligada por padrão desde 16/10/2026. Também no menu do jogo.'),
        pass_env=False),
    _ab('deferred_readback', 'TLG_DEFERRED_READBACK', True, False, False, ('Deferred GPU readbacks', 'Leituras da GPU adiadas'),
        ('Only the thread that reads GPU results waits, not the whole GPU. Needs "Precise" GPU accuracy.',
         'Só a thread que lê resultados da GPU espera, não a GPU inteira. Precisa da precisão "Preciso".'),
        store='launcher', pass_env=False),
    _ab('stale_readback', 'TLG_STALE_READBACK', False, False, False, ('Stale readbacks', 'Leituras da GPU com um quadro de atraso'),
        ('CPU reads see the last published GPU version instead of waiting (needs deferred readbacks).',
         'A CPU lê a última versão publicada pela GPU em vez de esperar (precisa das leituras adiadas).'),
        pass_env=False),
    _ab('background_warmup', 'TLG_BACKGROUND_WARMUP', True, False, False, ('Load pipelines while playing', 'Carregar pipelines durante o jogo'),
        ('The cached pipelines load in the background (a corner note shows progress); off blocks until all are loaded.',
         'Os pipelines em cache carregam em segundo plano (uma nota no canto mostra o progresso); desligado espera carregar tudo.')),
    _ab('drain_sleep', 'TLG_DRAIN_SLEEP', True, False, False, ('GPU command thread sleeps while waiting', 'Thread de comandos da GPU dorme ao esperar'),
        ('While it waits for draw recording the GPU command thread sleeps instead of spinning: one core less busy, no FPS difference measured (16 threads: 30.8-32.9 either way). Off restores the spin for comparisons.',
         'Enquanto espera a gravação dos draws, a thread de comandos da GPU dorme em vez de girar: um núcleo a menos ocupado, sem diferença de FPS medida (16 threads: 30,8-32,9 nos dois modos). Desligado volta ao giro, para comparar.')),
    _ab('pipeline_binaries', 'TLG_PIPELINE_BINARIES', False, False, False, ('Pipeline binaries (VK_KHR_pipeline_binary)', 'Pipeline binaries (VK_KHR_pipeline_binary)'),
        ('Still crashes the game on the NVIDIA 595 driver; off by default in this port.',
         'Ainda derruba o jogo no driver NVIDIA 595; desligado por padrão neste port.'),
        broken=True),
    # Windows default: renderer v2 with its native passes (RTX 3060 + Ryzen 5 5500 / 4600G, 2026-10-21:
    # 11-20 FPS in gameplay with the previous v2 defaults, ~40-45 FPS with native passes and
    # unified memory off: the v2_unified_memory switch below, whose value replaces
    # gpu_v2/defaults.env's ISS_UNIFIED_MEMORY=1 in launcher runs). Linux keeps this port's
    # renderer: native passes crash NVIDIA's Linux driver.
    _ab_choice('renderer', 'TLG_RENDERER', '2' if _WINDOWS else '1',
               (('1', '1 · This port\'s renderer', '1 · Renderer deste port'),
                ('2', '2 · Renderer v2 (experimental)', '2 · Renderer v2 (experimental)')),
               ('Renderer', 'Renderer'),
               ('Renderer v2 (port/gpu_v2): another renderer with this port\'s interface, its own fixes and '
                'its own shader cache (the first start compiles the shaders again). The default on Windows. '
                'Needs the v2 build (TLG_RENDERER_V2=1 build.sh); without it the game starts with this '
                'port\'s renderer.',
                'Renderer v2 (port/gpu_v2): outro renderer com a interface deste port, as próprias correções e '
                'o próprio cache de shaders (a primeira partida compila os shaders de novo). O padrão no '
                'Windows. Precisa do build v2 (TLG_RENDERER_V2=1 build.sh); sem ele o jogo abre com o '
                'renderer deste port.'),
               source='launcher (infamous_run.py)', pass_env=False),
    _ab('native_passes', 'TLG_NATIVE_PASSES', _WINDOWS, False, False,
        ('Native passes (renderer v2 only)', 'Native passes (só no renderer v2)'),
        ('Renderer v2 records the game\'s draws straight into Vulkan when the game submits them. On by '
         'default on Windows; off on Linux, where NVIDIA\'s driver can drop frames or crash with them. '
         'Has no effect on this port\'s renderer.',
         'O renderer v2 grava os desenhos do jogo direto no Vulkan quando o jogo os envia. Ligado por '
         'padrão no Windows; desligado no Linux, onde o driver NVIDIA pode derrubar quadros ou travar. '
         'Não tem efeito no renderer deste port.'),
        pass_env=False),
    # Windows default off: with it on, the game's fences wait for the GPU ~28 times a frame (11-20 FPS
    # in gameplay, RTX 3060 + Ryzen 5 5500, 2026-10-21). Castlesea's release has it on but its heap
    # range never matches there, so it measures without it.
    _ab('v2_unified_memory', 'ISS_UNIFIED_MEMORY', not _WINDOWS, False, False,
        ('Unified memory (renderer v2 only)', 'Memória unificada (só no renderer v2)'),
        ('Renderer v2 shares the game\'s per-frame heap with the GPU, as on the PS4, so the game waits for '
         'the GPU to finish before it reuses that memory. Off by default on Windows, where it held the game '
         'at 11-20 FPS; on on Linux. Has no effect on this port\'s renderer.',
         'O renderer v2 compartilha com a GPU o heap de dados por quadro do jogo, como no PS4, e o jogo '
         'espera a GPU terminar antes de reusar essa memória. Desligado por padrão no Windows, onde segurava '
         'o jogo em 11-20 FPS; ligado no Linux. Não tem efeito no renderer deste port.'),
        pass_env=False),
    _ab_choice('occlusion_mode', 'TLG_OCCLUSION_MODE', '4',
               (('0', '0 · As before (fake counter, glare may show through)', '0 · Como antes (contador falso, o brilho pode atravessar)'),
                ('1', '1 · Lights count as hidden', '1 · Luzes contam como tapadas'),
                ('2', '2 · Lights count as fully visible', '2 · Luzes contam como totalmente visíveis'),
                ('3', '3 · Zero counts (previous default)', '3 · Contagem zero (padrão anterior)'),
                ('4', '4 · Real occlusion, counted by the GPU (recommended)', '4 · Oclusão real, contada pela GPU (recomendado)')),
               ('Occlusion query (diagnostic)', 'Consulta de oclusão (diagnóstico)'),
               ('Test for the headlight glow that shows through characters and flickers: the counter the game '
                'reads to know how much of a light is visible is a fake one. 1 = nothing visible, 2 = everything '
                'visible, 3 = zero counts, 4 = real occlusion counted by the GPU (the default); 0 is the old behaviour. '
                'Send the log and a screenshot of each mode.',
                'Teste do brilho dos faróis que atravessa personagens e pisca: o contador que o jogo lê para saber '
                'quanto de uma luz está visível é falso. 1 = nada visível, 2 = tudo visível, 3 = contagem zero; '
                '4 = oclusão real contada pela GPU (o padrão); 0 é o comportamento antigo. '
                'Mande o log e uma captura de cada modo.'),
               source='diagnóstico (liverpool.cpp ZPASS_DONE)'),
    _ab('verbose_log', 'TLG_VERBOSE_LOG', False, False, False, ('Detailed log (for bug reports)', 'Log detalhado (para relatórios de problema)'),
        ('Turns on every safe trace switch, the GPU log at debug level, timestamps and thread ids on GPU lines, and extra '
         'messages for files, gamepads, semaphores, audio and boot steps. The log gets very large and the game may run a '
         'little slower, so turn it on only to send the log with a problem report.',
         'Liga todas as opções de rastreio seguras, o log da GPU em nível debug, horário e id da thread nas linhas da GPU e '
         'mensagens extras sobre arquivos, controles, semáforos, áudio e etapas da inicialização. O log fica muito grande e o '
         'jogo pode ficar um pouco mais lento; ligue só para enviar o log junto de um relatório de problema.')),
]

import advanced_table  # noqa: E402

ADVANCED = advanced_table.build(_make)

# Rows whose variable nothing in renderer v2 (port/gpu_v2) or the shared runtime (port/src) reads:
# they only change this port's renderer, and their names say so. tests/test_backports_table.py
# checks this list against the sources. (The launcher's own GPU accuracy / read-ahead / write
# merge settings are marked the same way in infamous_launcher.py TEXT.)
NO_EFFECT_V2 = frozenset([f'BP{n:02d}' for n in range(1, 13)] + [
    'TLG_WQM', 'TLG_LDS_SMALL', 'TLG_LDS_MULTI', 'TLG_LDS_READLANE', 'TLG_LANE_DIVERGENCE', 'TLG_RFL_ALL',
    'TLG_BLOCK1D', 'TLG_BC_TINY', 'TLG_NO_MSAA', 'TLG_ASYNC_SHADERS', 'TLG_DEFERRED_READBACK',
    'TLG_STALE_READBACK', 'TLG_BACKGROUND_WARMUP', 'TLG_DRAIN_SLEEP', 'TLG_PIPELINE_BINARIES',
    'TLG_OCCLUSION_MODE',
    'TLG_FULL_BARRIERS', 'TLG_GPU_TIMESTAMP', 'TLG_GPU_TIMESTAMP_STEP_US', 'TLG_PRESENT_THREAD',
    'TLG_VK_RECORD_THREAD', 'TLG_PREP_WORKERS', 'TLG_SUBMIT_DRAWS', 'TLG_PIPE_SPIN_US', 'TLG_PIPE_VERIFY',
    'TLG_EAGER_READBACK', 'TLG_PARTIAL_HEIGHT_VIEWS', 'TLG_BACKING_INVALIDATE', 'TLG_FAST_READ_FAULT',
    'TLG_READBACK_LINEAR', 'TLG_DOWNLOAD_WINDOW_KB', 'TLG_COPY_MERGE_KB', 'TLG_STAGING_KEEP_MB',
    'TLG_STREAM_MEMORY', 'TLG_VARIANT_REUSE', 'TLG_HEAP_WARM', 'TLG_INDIRECT_CHECK', 'TLG_ASYNC_WAIT_MS',
    'TLG_ASYNC_WAIT_FRAME_MS', 'TLG_DRIVER_CACHE_SAVE_S', 'TLG_PARALLEL_WARMUP', 'TLG_COMPUTE_ASYNC_BUILD',
    'TLG_SQUARE_PASS', 'TLG_INTERP_INT_FIX', 'TLG_1D_NO_STORAGE', 'TLG_1D_NO_MUTABLE', 'TLG_EXPLICIT_RANGE',
    'TLG_BARRIER_SPLIT', 'TLG_BARRIER_MERGE', 'TLG_PERF_DIAG', 'TLG_IMG_TRACE', 'TLG_BC1D_LOG',
    'TLG_WAVE_STATS'])
NO_EFFECT_V2_NOTE = (' (no effect in renderer v2)', ' (sem efeito no renderer v2)')


def _note_no_effect_v2(items):
    return [item._replace(name=(item.name[0] + NO_EFFECT_V2_NOTE[0], item.name[1] + NO_EFFECT_V2_NOTE[1]))
            if item.id in NO_EFFECT_V2 else item for item in items]


BACKPORTS = _note_no_effect_v2(BACKPORTS)
DIAGNOSTICS = _note_no_effect_v2(DIAGNOSTICS)
ADVANCED = _note_no_effect_v2(ADVANCED)
ITEMS = MENU + BACKPORTS + DIAGNOSTICS + ADVANCED
PREFIX = 'ISS_'


def _from_launcher(item, settings):
    value = settings.get(item.key)
    return item.default if value is None else '1' if value else '0'


def read_states(ini, settings=None):
    """{item.id: value text or None}: the player's choice, None = the default applies.
    A value equal to the default is also None, so only real changes are kept."""
    settings = settings or {}
    states = {}
    for item in ITEMS:
        value = _from_launcher(item, settings) if item.store == 'launcher' else ini.get(item.key)
        states[item.id] = normalize(item, value)
    return states


def normalize(item, value):
    """The value if valid and different from the default, else None."""
    if value is None:
        return None
    value = str(value).strip()
    if item.vtype == 'bool':
        valid = value in ('0', '1')
    elif item.vtype == 'choice':
        valid = value in [c[0] for c in item.choices]
    elif item.vtype == 'float':
        try:
            number = float(value)
            valid = item.lo <= number <= item.hi
        except ValueError:
            valid = False
        if valid:
            if abs(number - float(item.default)) <= 1e-6 * max(1.0, abs(number)):
                return None
            value = f'{number:g}' if abs(number) < 1e6 else str(int(round(number)))
    else:
        try:
            valid = item.lo <= int(value) <= item.hi
        except ValueError:
            valid = False
        value = str(int(value)) if valid else value
    return value if valid and value != item.default else None


# Slider steps of the 'float' items (launchers).
FLOAT_STEPS = {'lod_scale': 0.05, 'region_lod': 0.05, 'region_fade': 0.05, 'near_clip': 0.5,
               'far_clip': 1000.0, 'mip_lod_bias': 0.125}


def effective(item, states):
    """The value the game will use; None for an automatic switch left on automatic."""
    value = states.get(item.id)
    return item.default if value is None else value


def ini_updates(states):
    """tlg.ini keys to write: the value for a change, None to remove the key (default again)."""
    return {item.key: states.get(item.id) for item in ITEMS if item.store == 'ini'}


def environment(states, environ=None):
    """ISS_<name>=0/1 for the BPnn and A/B switches the player changed; one already in the
    environment (ISS_ or TLG_) wins. Untouched switches add nothing, so the defaults hold."""
    environ = {} if environ is None else environ
    env = {}
    for item in ITEMS:
        value = states.get(item.id)
        if value is None or not item.pass_env:
            continue
        suffix = item.env[len('TLG_'):]
        if PREFIX + suffix in environ or item.env in environ:
            continue
        env[PREFIX + suffix] = value
    return env


def previous_release(states):
    """Every BPnn off, as the release before the backports (explicit, so the list shows it)."""
    states = dict(states)
    for item in BACKPORTS:
        states[item.id] = '0' if item.default == '1' else None
    return states


def defaults(states, group=None):
    """Forget every choice (of one group, or all)."""
    return {k: (None if group is None or _group(k) == group else v) for k, v in states.items()}


def is_recommended(item):
    """Rows that are part of the Recommended profile (the table's defaults): the fixes and
    switches that are on by default. Recommended = no choice stored; nothing here changes a default."""
    return item.group in ('bp', 'ab') and item.vtype == 'bool' and item.default == '1'


def changes(states):
    """How many fixes/switches (BPnn and A/B) differ from the Recommended profile. The game menu
    options (overlay corner, limits...) are preferences and do not count."""
    return sum(1 for item in BACKPORTS + DIAGNOSTICS if states.get(item.id) is not None)


def advanced_changes(states):
    """How many Advanced parameters the player changed."""
    return sum(1 for item in ADVANCED if states.get(item.id) is not None)


def notice_pending(settings):
    """The one-time notice about the new fixes has not been shown yet."""
    return not settings.get('backports_notice_seen')


def mark_notice_seen(settings):
    settings['backports_notice_seen'] = True


def _group(item_id):
    return next(item.group for item in ITEMS if item.id == item_id)


def write_ini(path, updates):
    """Sets or removes keys in tlg.ini, keeping every other line (the game's own keys included)."""
    path = Path(path)
    try:
        lines = path.read_text(encoding='utf-8').splitlines()
    except OSError:
        lines = ['# tlg settings']
    out, seen = [], set()
    for line in lines:
        key = line.split('=', 1)[0].strip() if '=' in line and not line.lstrip().startswith('#') else None
        if key in updates:
            seen.add(key)
            if updates[key] is not None:
                out.append(f'{key}={updates[key]}')
        else:
            out.append(line)
    out += [f'{k}={v}' for k, v in updates.items() if v is not None and k not in seen]
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('\n'.join(out) + '\n', encoding='utf-8')
