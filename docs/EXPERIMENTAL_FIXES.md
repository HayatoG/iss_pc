# Correções experimentais e opções do menu no launcher

No launcher novo (Qt) é a tela **Correções**; no launcher Tk de reserva (**inFAMOUS Second Son (classic)**), o botão **Correções experimentais…** abre uma janela com três abas. Os dois leem e gravam as mesmas chaves. Tudo é salvo no `tlg.ini` (pasta `user/` do port) e vale na **próxima partida**; os itens marcados "no jogo" também podem ser mudados durante a partida, no menu Insert. Se você não mexer em nada, valem os padrões do port. Só o que difere do padrão é gravado: voltar ao padrão apaga a chave.

- **Menu do jogo (Insert):** as opções do menu do jogo que não estão no resto do launcher (imagem, desempenho, controle e overlay). Modo de exibição, VSync e o botão do overlay de FPS continuam no quadro Vídeo.
- **Correções experimentais (BPnn):** as 12 correções portadas de outros ports de PS4 e do shadPS4 (`port/gpu/shadps4/common/backports.h`). As de padrão desligado (BP06 a BP09) são experimentais. Em cada uma, o launcher mostra a origem; as que mudam shader avisam que a primeira partida depois de mudar recompila os shaders (pode piscar ou ficar verde por um tempo).
- **Chaves de diagnóstico / A-B:** chaves que só existiam como variável de ambiente, para descobrir qual mudança causa um problema. Inclui o **modo de oclusão** (`ISS_OCCLUSION_MODE`, padrão 3; veja abaixo), que também tem um seletor na tela inicial. `TLG_PIPELINE_BINARIES` ainda derruba o jogo no driver NVIDIA 595.

Botões: **Voltar ao recomendado** (apaga todas as escolhas; o perfil Recomendado são os padrões da tabela, com o selo "recomendado" nas linhas que o compõem; a janela mostra "Perfil: Recomendado" ou "Perfil: personalizado (N mudanças)") e **Como a release anterior** (grava `bp01=0` ... para todo BPnn que vem ligado; é o mesmo que iniciar com `TLG_BACKPORTS=0`, mas as caixas mostram o estado e dá para religar uma a uma).

Variáveis: o launcher passa `ISS_<nome>=0/1` ao jogo (o `tlg-probe` copia para `TLG_<nome>`; um `ISS_`/`TLG_` já definido fora do launcher vence). As opções do menu do jogo são lidas do `tlg.ini` pelo próprio jogo.

## Correções BPnn

| Item | Chave (tlg.ini) | Variável | Padrão | Vale | Origem |
|---|---|---|---|---|---|
| BP01 Liberar buffers de comando aninhados | `bp01` | ISS_/TLG_BP01 | ligado | no jogo (Insert) | shadPS4 #5258 |
| BP02 Busca de texturas mais rápida | `bp02` | ISS_/TLG_BP02 | ligado | no jogo (Insert) | shadPS4 145932bf (#5240) |
| BP03 Manter o EXEC após desvio vazio | `bp03` | ISS_/TLG_BP03 | ligado | reinicia o jogo, recompila shaders | shadPS4 c34ca21f |
| BP04 Recíproco/log sem clamp | `bp04` | ISS_/TLG_BP04 | ligado | reinicia o jogo, recompila shaders | shadPS4 1c2b8f97 (#5346) |
| BP05 Saturate mantém NaN | `bp05` | ISS_/TLG_BP05 | ligado | reinicia o jogo, recompila shaders | shadPS4 #5257 |
| BP06 Reconvergência máxima | `bp06` | ISS_/TLG_BP06 | desligado | reinicia o jogo, recompila shaders, experimental | Shadow of the Colossus port 8d84cf7 |
| BP07 Compilação assíncrona sem pular draws | `bp07` | ISS_/TLG_BP07 | desligado | no jogo (Insert), experimental | bbport 10baf9d |
| BP08 Cache de pipelines do driver | `bp08` | ISS_/TLG_BP08 | desligado | reinicia o jogo, experimental | Mrsuss b972798, 0xCydral bdb9769 |
| BP09 Toda imagem 1D como 2D | `bp09` | ISS_/TLG_BP09 | desligado | reinicia o jogo, recompila shaders, experimental | CTR GetLostIn d26bcb2 |
| BP10 Barreiras de buffers de vértice/índice | `bp10` | ISS_/TLG_BP10 | ligado | no jogo (Insert) | shadPS4 #5262 |
| BP11 First instance contada uma vez | `bp11` | ISS_/TLG_BP11 | ligado | reinicia o jogo, recompila shaders | shadPS4 #5348 |
| BP12 Correção do shadPS4 #5264 | `bp12` | ISS_/TLG_BP12 | ligado | no jogo (Insert) | shadPS4 #5264 |

## Chaves de diagnóstico / A-B

| Item | Chave (tlg.ini) | Variável | Padrão | Vale | Origem |
|---|---|---|---|---|---|
|  Modo quad inteiro | `wqm` | ISS_/TLG_WQM | ligado | reinicia o jogo, recompila shaders |  |
|  Barreiras LDS, grupos pequenos | `lds_small` | ISS_/TLG_LDS_SMALL | ligado | reinicia o jogo, recompila shaders |  |
|  Barreiras LDS, grupos grandes | `lds_multi` | ISS_/TLG_LDS_MULTI | ligado | reinicia o jogo, recompila shaders |  |
|  LDS: ReadLane constante é uniforme | `lds_readlane` | ISS_/TLG_LDS_READLANE | ligado | reinicia o jogo, recompila shaders |  |
|  Desvios que variam por lane | `lane_divergence` | ISS_/TLG_LANE_DIVERGENCE | ligado | reinicia o jogo, recompila shaders |  |
|  ReadFirstLane em todo shader largo | `rfl_all` | ISS_/TLG_RFL_ALL | desligado | reinicia o jogo, recompila shaders |  |
|  Texturas BC 1D como 2D | `block1d` | ISS_/TLG_BLOCK1D | automático | reinicia o jogo, recompila shaders |  |
|  Texturas BC minúsculas na CPU | `bc_tiny` | ISS_/TLG_BC_TINY | automático | reinicia o jogo, recompila shaders |  |
|  Desligar multisampling | `no_msaa` | ISS_/TLG_NO_MSAA | automático | reinicia o jogo, recompila shaders |  |
|  Consulta de oclusão (0 a 3) | `occlusion_mode` | ISS_/TLG_OCCLUSION_MODE | 3 | reinicia o jogo | diagnóstico (`liverpool.cpp`, `ZPASS_DONE`) |
|  Compilação assíncrona de shaders | `async_shaders` | (lido do tlg.ini) | ligado | no jogo (Insert) |  |
|  Leituras da GPU adiadas | `deferred_readback` | (lido do tlg.ini) | ligado | reinicia o jogo |  |
|  Leituras da GPU com um quadro de atraso | `stale_readback` | (lido do tlg.ini) | ligado | reinicia o jogo |  |
|  Carregar pipelines durante o jogo | `background_warmup` | ISS_/TLG_BACKGROUND_WARMUP | ligado | reinicia o jogo |  |
|  Pipeline binaries (VK_KHR_pipeline_binary) | `pipeline_binaries` | ISS_/TLG_PIPELINE_BINARIES | desligado | reinicia o jogo |  |

**Modo de oclusão (`occlusion_mode`):** o contador `ZPASS_DONE` que o jogo lê para saber quanto de uma luz está visível era um número falso e crescente, e isso fazia os faróis piscarem e o brilho atravessar personagens. `3` (padrão, contagem zero) corrigiu isso, confirmado no Windows e no Linux; `0` é o comportamento antigo; `1` (luzes tapadas) e `2` (luzes totalmente visíveis) são testes. Risco: o brilho das luzes pode ficar mais fraco com o `3`; nesse caso use `0`. `TLG_OCCLUSION_LOG=1` registra os eventos no log.

`TLG_DEFERRED_READBACK` usa a opção que o launcher já tinha (settings.json), `TLG_ASYNC_SHADERS` e `TLG_STALE_READBACK` são lidas do `tlg.ini` pelo jogo (o menu Insert escreve as mesmas chaves).

## Opções do menu do jogo

| Item | Chave (tlg.ini) | Variável | Padrão | Vale | Origem |
|---|---|---|---|---|---|
|  Ampliar com FSR 1 | `fsr1` | (lido do tlg.ini) | desligado | no jogo (Insert) |  |
|  Nitidez (RCAS) | `rcas` | (lido do tlg.ini) | ligado | no jogo (Insert) |  |
|  Intensidade da nitidez | `rcas_strength` | (lido do tlg.ini) | 75 | no jogo (Insert) |  |
|  Limite de FPS | `fps_limit` | (lido do tlg.ini) | 0 | no jogo (Insert) |  |
|  Quadros na fila | `frames_queued` | (lido do tlg.ini) | 1 | no jogo (Insert) |  |
|  Velocidade da mira do grafite | `sensor_aim` | (lido do tlg.ini) | 100 | no jogo (Insert) |  |
|  Overlay: FPS | `overlay_fps` | (lido do tlg.ini) | ligado | no jogo (Insert) |  |
|  Overlay: tempo de quadro | `overlay_frametime` | (lido do tlg.ini) | ligado | no jogo (Insert) |  |
|  Overlay: upscaler | `overlay_upscaler` | (lido do tlg.ini) | desligado | no jogo (Insert) |  |
|  Overlay: GPU | `overlay_gpu` | (lido do tlg.ini) | ligado | no jogo (Insert) |  |
|  Overlay: CPU | `overlay_cpu` | (lido do tlg.ini) | desligado | no jogo (Insert) |  |
|  Overlay: RAM | `overlay_ram` | (lido do tlg.ini) | desligado | no jogo (Insert) |  |
|  Overlay: VRAM | `overlay_vram` | (lido do tlg.ini) | ligado | no jogo (Insert) |  |
|  Tamanho da fonte do overlay | `overlay_scale` | (lido do tlg.ini) | 100 | no jogo (Insert) |  |
|  Canto do overlay | `overlay_corner` | (lido do tlg.ini) | 1 | no jogo (Insert) |  |
|  Opacidade do fundo do overlay | `overlay_opacity` | (lido do tlg.ini) | 50 | no jogo (Insert) |  |
|  Disposição do overlay | `overlay_layout` | (lido do tlg.ini) | 0 | no jogo (Insert) |  |

Na primeira abertura do launcher depois de atualizar, uma mensagem única explica que as correções novas já vêm ativadas e como voltar ("Como a release anterior"); a chave `backports_notice_seen` no settings do launcher impede que ela se repita.

## No log

Todo log (`out/logs/run-*.log`) começa com `Build:` e um bloco `Config:` com as chaves do `tlg.ini` e as variáveis `TLG_`/`ISS_` definidas (sem valores que pareçam caminhos), seguido das linhas `Backports:` (uma por BPnn, com o estado, mais o modo de oclusão); com as estatísticas de quadros aparece `Backport hits (total): BPnn n, ...`, que conta quantas vezes cada correção realmente rodou (só as que se mexeram). Isso diz, num relato, quais correções estavam ligadas.

## Origem das correções

BP01–BP05, BP10–BP12: [shadPS4](https://github.com/shadps4-emu/shadPS4) (números de PR/commits na tabela). BP06: port nativo de [Shadow of the Colossus](https://github.com/AjncSystem/ShadowOfTheColossus-PC). BP07: bbport ([bloodborne_pc](https://github.com/deadinside28/bloodborne_pc)). BP08: [Mrsuss60/bloodborne_pc_windows_port](https://github.com/Mrsuss60/bloodborne_pc_windows_port) e [0xCydral/bloodborne_pc](https://github.com/0xCydral/bloodborne_pc). BP09: [T3CHNOLOG1C/CTR](https://github.com/T3CHNOLOG1C/CTR). Detalhes em `port/gpu/VENDOR.txt` e nas mensagens dos commits `Backport BPnn`.

---

# Experimental fixes and game menu options in the launcher

In the new (Qt) launcher it is the **Fixes** screen; in the fallback Tk launcher (**inFAMOUS Second Son (classic)**), the **Experimental fixes…** button opens a window with three tabs. Both read and write the same keys. Everything is saved in `tlg.ini` (the port's `user/` folder) and applies on the **next start**; items tagged "in game" can also be changed while playing, in the Insert menu. If you touch nothing, the port's defaults hold. Only what differs from the default is written: going back to the default removes the key.

- **Game menu (Insert):** the in-game menu options the rest of the launcher does not have (image, performance, controls, overlay). Display mode, VSync and the FPS overlay switch stay in the Video box.
- **Experimental fixes (BPnn):** the 12 fixes ported from other PS4 ports and from shadPS4 (`port/gpu/shadps4/common/backports.h`). Those off by default (BP06 to BP09) are experimental. Each shows its origin; the ones that change shaders warn that the first game after changing recompiles the shaders (it may flicker or look green for a while).
- **Diagnostic / A-B switches:** switches that were environment variables only, to find which change causes a problem. It includes the **occlusion mode** (`ISS_OCCLUSION_MODE`, default 3; see below), which also has a selector on the home screen. `TLG_PIPELINE_BINARIES` still crashes the game on the NVIDIA 595 driver.

Buttons: **Back to recommended** (clears every choice; the Recommended profile is the table defaults, rows in it carry a "recommended" badge; the window shows "Profile: Recommended" or "Profile: custom (N changes)") and **Like the previous release** (writes `bp01=0` ... for every BPnn that is on by default; same as starting with `TLG_BACKPORTS=0`, but the boxes show the state and you can turn single ones back on).

Variables: the launcher gives the game `ISS_<name>=0/1` (`tlg-probe` copies it to `TLG_<name>`; an `ISS_`/`TLG_` already set outside the launcher wins). The game menu options are read from `tlg.ini` by the game itself.

## BPnn fixes

| Item | tlg.ini key | Variable | Default | Applies | Source |
|---|---|---|---|---|---|
| BP01 Free nested command buffers | `bp01` | ISS_/TLG_BP01 | on | in game (Insert) | shadPS4 #5258 |
| BP02 Faster texture lookup | `bp02` | ISS_/TLG_BP02 | on | in game (Insert) | shadPS4 145932bf (#5240) |
| BP03 Keep EXEC after empty branch | `bp03` | ISS_/TLG_BP03 | on | restarts the game, recompiles shaders | shadPS4 c34ca21f |
| BP04 Reciprocal/log without clamp | `bp04` | ISS_/TLG_BP04 | on | restarts the game, recompiles shaders | shadPS4 1c2b8f97 (#5346) |
| BP05 Saturate keeps NaN | `bp05` | ISS_/TLG_BP05 | on | restarts the game, recompiles shaders | shadPS4 #5257 |
| BP06 Maximal reconvergence | `bp06` | ISS_/TLG_BP06 | off | restarts the game, recompiles shaders, experimental | Shadow of the Colossus port 8d84cf7 |
| BP07 Async compile never skips draws | `bp07` | ISS_/TLG_BP07 | off | in game (Insert), experimental | bbport 10baf9d |
| BP08 Driver pipeline cache | `bp08` | ISS_/TLG_BP08 | off | restarts the game, experimental | Mrsuss b972798, 0xCydral bdb9769 |
| BP09 Every 1D image as 2D | `bp09` | ISS_/TLG_BP09 | off | restarts the game, recompiles shaders, experimental | CTR GetLostIn d26bcb2 |
| BP10 Vertex/index buffer barriers | `bp10` | ISS_/TLG_BP10 | on | in game (Insert) | shadPS4 #5262 |
| BP11 First instance counted once | `bp11` | ISS_/TLG_BP11 | on | restarts the game, recompiles shaders | shadPS4 #5348 |
| BP12 shadPS4 fix #5264 | `bp12` | ISS_/TLG_BP12 | on | in game (Insert) | shadPS4 #5264 |

## Diagnostic / A-B switches

| Item | tlg.ini key | Variable | Default | Applies | Source |
|---|---|---|---|---|---|
|  Whole quad mode | `wqm` | ISS_/TLG_WQM | on | restarts the game, recompiles shaders |  |
|  LDS barriers, small groups | `lds_small` | ISS_/TLG_LDS_SMALL | on | restarts the game, recompiles shaders |  |
|  LDS barriers, large groups | `lds_multi` | ISS_/TLG_LDS_MULTI | on | restarts the game, recompiles shaders |  |
|  LDS: constant ReadLane is uniform | `lds_readlane` | ISS_/TLG_LDS_READLANE | on | restarts the game, recompiles shaders |  |
|  Lane-varying branches | `lane_divergence` | ISS_/TLG_LANE_DIVERGENCE | on | restarts the game, recompiles shaders |  |
|  ReadFirstLane in every wide shader | `rfl_all` | ISS_/TLG_RFL_ALL | off | restarts the game, recompiles shaders |  |
|  BC 1D textures as 2D | `block1d` | ISS_/TLG_BLOCK1D | automatic | restarts the game, recompiles shaders |  |
|  Tiny BC textures on the CPU | `bc_tiny` | ISS_/TLG_BC_TINY | automatic | restarts the game, recompiles shaders |  |
|  Disable multisampling | `no_msaa` | ISS_/TLG_NO_MSAA | automatic | restarts the game, recompiles shaders |  |
|  Occlusion query (0 to 3) | `occlusion_mode` | ISS_/TLG_OCCLUSION_MODE | 3 | restarts the game | diagnostic (`liverpool.cpp`, `ZPASS_DONE`) |
|  Asynchronous shader compile | `async_shaders` | (read from tlg.ini) | on | in game (Insert) |  |
|  Deferred GPU readbacks | `deferred_readback` | (read from tlg.ini) | on | restarts the game |  |
|  Stale readbacks | `stale_readback` | (read from tlg.ini) | on | restarts the game |  |
|  Load pipelines while playing | `background_warmup` | ISS_/TLG_BACKGROUND_WARMUP | on | restarts the game |  |
|  Pipeline binaries (VK_KHR_pipeline_binary) | `pipeline_binaries` | ISS_/TLG_PIPELINE_BINARIES | off | restarts the game |  |

**Occlusion mode (`occlusion_mode`):** the `ZPASS_DONE` counter the game reads to know how much of a light is visible was a fake, ever-growing number, which made headlights flicker and glare show through characters. `3` (default, zero counts) fixed it, confirmed on Windows and on Linux; `0` is the old behavior; `1` (lights hidden) and `2` (lights fully visible) are tests. Risk: the glare of lights may look weaker with `3`; if so, use `0`. `TLG_OCCLUSION_LOG=1` logs the events in the log.

`TLG_DEFERRED_READBACK` uses the launcher's existing option (settings.json); `TLG_ASYNC_SHADERS` and `TLG_STALE_READBACK` are read from `tlg.ini` by the game (the Insert menu writes the same keys).

## Game menu options

| Item | tlg.ini key | Variable | Default | Applies | Source |
|---|---|---|---|---|---|
|  Upscale with FSR 1 | `fsr1` | (read from tlg.ini) | off | in game (Insert) |  |
|  Sharpening (RCAS) | `rcas` | (read from tlg.ini) | on | in game (Insert) |  |
|  Sharpening strength | `rcas_strength` | (read from tlg.ini) | 75 | in game (Insert) |  |
|  Frame rate limit | `fps_limit` | (read from tlg.ini) | 0 | in game (Insert) |  |
|  Frames queued | `frames_queued` | (read from tlg.ini) | 1 | in game (Insert) |  |
|  Graffiti aiming speed | `sensor_aim` | (read from tlg.ini) | 100 | in game (Insert) |  |
|  Overlay: FPS | `overlay_fps` | (read from tlg.ini) | on | in game (Insert) |  |
|  Overlay: frame time | `overlay_frametime` | (read from tlg.ini) | on | in game (Insert) |  |
|  Overlay: upscaler | `overlay_upscaler` | (read from tlg.ini) | off | in game (Insert) |  |
|  Overlay: GPU | `overlay_gpu` | (read from tlg.ini) | on | in game (Insert) |  |
|  Overlay: CPU | `overlay_cpu` | (read from tlg.ini) | off | in game (Insert) |  |
|  Overlay: RAM | `overlay_ram` | (read from tlg.ini) | off | in game (Insert) |  |
|  Overlay: VRAM | `overlay_vram` | (read from tlg.ini) | on | in game (Insert) |  |
|  Overlay font size | `overlay_scale` | (read from tlg.ini) | 100 | in game (Insert) |  |
|  Overlay corner | `overlay_corner` | (read from tlg.ini) | 1 | in game (Insert) |  |
|  Overlay background opacity | `overlay_opacity` | (read from tlg.ini) | 50 | in game (Insert) |  |
|  Overlay layout | `overlay_layout` | (read from tlg.ini) | 0 | in game (Insert) |  |

The table is generated from `port/launcher/backports_table.py`; `port/tests/test_backports_table.py` keeps it in step with the sources.

On the first launcher start after updating, a one-time message explains that the new fixes are already on and how to go back ("Like the previous release"); the `backports_notice_seen` key in the launcher settings keeps it from repeating.

## In the log

Every log (`out/logs/run-*.log`) starts with `Build:` and a `Config:` block with the `tlg.ini` keys and the `TLG_`/`ISS_` variables that are set (no values that look like paths), followed by the `Backports:` lines (one per BPnn, with its state, plus the occlusion mode); with the frame statistics comes `Backport hits (total): BPnn n, ...`, counting how many times each fix actually ran (only the ones that moved). In a report that says which fixes were on.

## Source of the fixes

BP01–BP05, BP10–BP12: [shadPS4](https://github.com/shadps4-emu/shadPS4) (PR numbers/commits in the table). BP06: the native [Shadow of the Colossus](https://github.com/AjncSystem/ShadowOfTheColossus-PC) port. BP07: bbport ([bloodborne_pc](https://github.com/deadinside28/bloodborne_pc)). BP08: [Mrsuss60/bloodborne_pc_windows_port](https://github.com/Mrsuss60/bloodborne_pc_windows_port) and [0xCydral/bloodborne_pc](https://github.com/0xCydral/bloodborne_pc). BP09: [T3CHNOLOG1C/CTR](https://github.com/T3CHNOLOG1C/CTR). Details in `port/gpu/VENDOR.txt` and in the `Backport BPnn` commit messages.

