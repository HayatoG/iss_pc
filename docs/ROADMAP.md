# inFAMOUS Second Son PC: roadmap

Port de inFAMOUS Second Son (Sucker Punch, PS4, CUSA00223, base 1.00 + update 1.07, SDK 4.00).
Atualizado em 15/10/2026. O estado de quem joga está no [README](../README.md#status); cada item
aberto vira uma issue com o tempo.

## Onde estamos

**Jogável, em desenvolvimento:** boot, título, intro e gameplay (testado até "Go to Longhouse"),
com som próprio (AudioOut + Ajm, sem arquivo do firmware), saves, HUD correto, sensor de movimento
(real ou simulado), touchpad simulado e partículas da GPU com a leitura precisa (padrão).
20–30 FPS típicos numa RTX 2060, instável. Launcher Qt como padrão (com o Tk de reserva), saves e dados
configuráveis, 12 correções experimentais (BP01–BP12) e o contador de oclusão corrigido (faróis e brilho
de luzes), confirmado no Windows e no Linux.

## Feito

| Item | Detalhe |
|---|---|
| Boot | `mov reg, fs:[0]` em qualquer registrador (TLS); os 15 imports só deste jogo (`runtime_infamous.c`) |
| Sensor de movimento | giroscópio real pela SDL3 e "modo sensor" simulado (Share ou L3+R3; teclado T/G/Y) |
| Analógicos | zona morta de 10% (`ISS_STICK_DEADZONE`, também no launcher) e o último controle usado |
| Touchpad sem touchpad | View/Select (ou Tab) + analógico direito desliza ("Open Door") |
| VRAM | orçamento padrão de 3500 MB (falta de VRAM no carregamento) |
| FPS caindo com o tempo | variantes de shader com a chave tirada antes da tradução: de 60→6 FPS para estável |
| Partículas da GPU (fumaça do dreno, drones) | leitura precisa da GPU por padrão; modo rápido no launcher ("Precisão da GPU") |
| Stencil `test_val`/`op_val` | uma referência Vulkan quando as máscaras permitem |
| Cache de shaders | pasta com a chave do tradutor; o cache antigo é apagado sozinho |
| Launcher Qt | padrão nos pacotes, 8 telas, pt-BR/en-US, Tk de reserva ("classic"), `--selftest` no CI |
| Idioma do jogo | "Automático (idioma do sistema)" por padrão e 6 idiomas (inclui turco e russo, se o dump tiver) |
| Saves e dados | pasta configurável (`ISS_USER_DIR`, arquivo-ponteiro), achar saves de outras instalações, backup antes de cada partida, exportar/importar `.zip` |
| Correções experimentais | BP01–BP12 (shadPS4, SotC, bbport, Mrsuss/0xCydral, CTR) com chave individual, perfil Recomendado e `TLG_BACKPORTS=0`; log com `Config:`, `Backports:` e `Backport hits` |
| Oclusão | modo 3 (contagem zero) por padrão: corrige faróis que piscavam e brilho que atravessava personagens (Windows e Linux) |
| Mouse | câmera, botões e modo touchpad (`ISS_MOUSE_*`, `ISS_KEY_MAP`); falta validar em mais jogadores |
| Guias de controle | imagens Xbox/Switch Pro/8BitDo em pt-BR e en-US |
| Suporte e Atualizações | relatório sem caminhos pessoais; verificação da última release (sem instalar nada) |
| CPUs sem F16C | as conversões de meia precisão são emuladas na partida (#51) |

## Aberto (por prioridade)

| # | Item | Detalhe |
|---|---|---|
| 1 | Desempenho | a thread do jogo espera leituras da GPU (escritas de `memcpy` em páginas modificadas pela GPU); correção: rastrear a memória em pedaços menores que a página. O primeiro passo existe (`ISS_WRITE_MERGE`, ligado pelo launcher, e as leituras adiadas), e o FPS segue instável |
| 2 | Engasgo de shader | 64–118 compilações por janela de 5 s em cenas novas; precisa do cache de pipelines em disco. O BP08 (blob do cache do driver) é experimental e desligado; `ISS_PIPELINE_BINARIES=1` ainda derruba o jogo no driver NVIDIA 595 |
| 3 | Oclusão real | o modo 3 é uma aproximação (contagem zero); falta medir a oclusão de verdade pelo Vulkan (occlusion queries). Risco atual: brilho de luzes mais fraco em algumas cenas (modo 0 volta ao antigo) |
| 4 | Névoa, água e fogo piscando; grade fina de linhas escuras | suspeitas: ballots wave64 em fluxo divergente, texturas BC 1D; os faróis e o brilho de luzes que piscavam já foram corrigidos (oclusão, modo 3) |
| 5 | Brilho azul do dreno de drones | não aparece; ballot wave64, stencil ou readback |
| 6 | RX 580 / Polaris no Windows | crash em `amdvlk64.dll+0xebbbb` (#31, #49, #59, #65, #67, #81); causa em investigação; as BC 1D minúsculas são decodificadas na CPU (`ISS_BC_TINY`) e o BP09 está para teste |
| 7 | GPU de 4 GB de VRAM | a memória de textura estoura (#72, #75); a investigar; por enquanto baixe a resolução. "Precisão da GPU: Rápido" escurece a imagem |
| 8 | Quadro / Maxwell | pode estar abaixo do mínimo (#70); a investigar |
| 9 | Texturas BC 1D | promoção para 2D automática nos drivers que recusam BC 1D, menos a NVIDIA; `ISS_BLOCK1D=0/1` decide. Na NVIDIA fica desligada porque estraga o HUD |
| 10 | Atualizador automático | a tela Atualizações só verifica a última release e abre a página; baixar e instalar sozinho ainda não existe |
| 11 | Mouse | validar com mais jogadores (câmera, botões, modo touchpad) |
| 12 | Windows | validar em mais PCs e placas (hoje o teste é por relatos de jogadores) |
| 13 | `param.sfo` 1.07 na visão do jogo | a visão usa o 1.00 |
| 14 | Idiomas | turco e russo dependem de o dump ter o idioma; ler do `param.sfo`/dados quais idiomas existem |
| 15 | Prefixo `ISS_`/`TLG_` | o jogador usa `ISS_*` e o núcleo ainda lê `TLG_*` por dentro; quando todos os ports usarem o ps4_core, o prefixo comum vira `PSCORE_` |

---

# inFAMOUS Second Son PC: roadmap (English)

Port of inFAMOUS Second Son (Sucker Punch, PS4, CUSA00223, base 1.00 + update 1.07, SDK 4.00).
Updated 15/10/2026. The player-facing status is in the [README](../README.md#english).

**Playable, in development:** boot, title, intro and gameplay (tested up to "Go to Longhouse"), own
audio (no firmware file), saves, correct HUD, real or simulated motion sensor, simulated touchpad and
GPU particles with precise readbacks (default). 20–30 FPS typical on an RTX 2060, unstable.

Done since the last roadmap: the Qt launcher as the default (Tk as a fallback), configurable saves and data,
12 experimental fixes (BP01–BP12), the occlusion counter fix (headlights and light glare; confirmed on Windows and
Linux), mouse camera and touchpad mode, controller guides, the Support and Updates screens, and the F16C emulation.

Open, by priority: (1) performance — the game thread waits on GPU readbacks; (2) shader-compile stutter — needs an
on-disk pipeline cache (BP08 is experimental and off); (3) real occlusion through Vulkan (mode 3 is an approximation);
(4) flickering fog/water/fire and a fine dark grid; (5) the missing blue drone-drain glow; (6) the RX 580/Polaris crash
on Windows; (7) GPUs with 4 GB of VRAM; (8) Quadro/Maxwell; (9) BC 1D textures; (10) an automatic updater (the Updates
screen only checks); (11) validating the mouse; (12) validating Windows on more PCs; (13) the 1.07 `param.sfo` in the
game view; (14) game languages from the game data; (15) the `ISS_`/`TLG_`/`PSCORE_` prefix.
