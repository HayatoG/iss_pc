# Renderer v3: experimentos com o núcleo do axylpup (branch `feat/renderer-v3-axylpup`)

Branch de experimentação, aberta em 28/10/2026 a partir da beta-rc1 (`3722737`). Nada daqui vai para a
`developer` sem PR e sem autorização do usuário. O objetivo é chegar perto do desempenho que os testadores do
`Liondarkz/issn_pc` relatam, sem perder o que já temos (upscaler temporal, resolução nativa, governador,
roda de poderes, troféus, mira direta).

Base da análise: `ANALISE_ISSN_GPU.md` e `ANALISE_ISSN_RESTO.md` (pasta `decomp/`, 27/10/2026).
Referências locais, só leitura: `decomp/ref_issn_pc` (issn_pc) e `decomp/ref_axylpup_shadps4`
(`axylpup/shadPS4`, tag `second-son-v1.1.1`, commit `57c9f746bbc3b91966b216435a14447c13a89da3`).

## O que se sabe e o que não se sabe

- **Fato (declarado por eles):** trocar o núcleo antigo pelo do axylpup levou a cidade de ~38 para 76–83 FPS, e a
  v1.1 chega a 90–105 FPS parado na cidade, com o AA do jogo, num Ryzen 7 3700X + RX 9060 XT. Com FSR 4 o próprio
  testador mediu 43 FPS.
- **Não sabido:** quanto disso é arquitetura e quanto é hardware (RDNA4, Windows). Ninguém mediu na nossa máquina
  de teste (RTX 2060). **Nenhuma decisão de trocar o núcleo sai antes da fase 0.**
- O ganho vem de arquitetura, não de LTO/PGO nem de extensões Vulkan: CP dividido (frente/fundo), parser magro e
  tradução nativa em workers, memos de pipeline/textura, rastreador de memória sem contenção, fila de readback.
  No nosso v2 as chaves equivalentes (`ISS_CP_SPLIT`, `ISS_DRAW_PIPE`, `ISS_ASYNC_COMPUTE`) vêm desligadas.

## Fases

Cada fase termina com medição e só a seguinte começa se a anterior justificar.

### Fase 0: medir (1 dia, precisa do jogo)
- Mesma rota gravada (`tools/route_run.py`), vsync desligado, contador de FPS ligado, mesma máquina, um jogo por vez
  sob a trava `MAQUINA.lock`.
- Comparar: nosso v2 (padrão), nosso v2 com as chaves da fase 1, e o núcleo do axylpup **compilado por nós** a partir
  do código-fonte (não rodar o `.zip` de terceiros).
- Saída: tabela de FPS médio, 1% low e custo de CPU/GPU por quadro. Isso decide a fase 4.

### Fase 1: ligar e medir o que o v2 já tem (sem código novo)
- `ISS_CP_SPLIT`, `ISS_DRAW_PIPE`, `ISS_ASYNC_COMPUTE`, `ISS_FAST_*`, `ISS_STAGE_MEMO`, `ISS_RENDER_REUSE`.
- Registrar o que quebra (NVIDIA 595 derruba o driver em alguns caminhos nativos) antes de mexer no código.

### Fase 2: peças pequenas do axylpup (cada uma um commit, com o hash de origem)
- P2 modo de desempenho: 7 sítios do eboot 1.07 + tampa de LOD (`view_hook.h` já tem os mesmos offsets).
- P6 lista de luzes por tile acima de 3200x1800 (evita device lost em 4K).
- P4 compute na frente (`ComputeUrgent`), um `VkPipelineCache` por thread, relatório de engasgo.
- P3 `PARTICLE_LATE_READ` + descarte da arena de partículas (patch de código do jogo; validar visualmente).

### Fase 3: upscaler com as entradas do próprio jogo (P1)
- Trocar os vetores de câmera calculados por matrizes pelos vetores de movimento, profundidade (Z reverso) e fase de
  jitter do jogo. Mantém o nosso gancho, o marcador e o backend FSR 3.1/DLSS.
- Usar aprendizagem por draw, nunca endereço fixo copiado (o layout de memória deles é outro).

### Fase 4: núcleo `gpu_v3` (só se a fase 0 mostrar diferença grande que as fases 1–3 não fecham)
1. Importar o núcleo do axylpup em `port/gpu_v3/shadps4` (cópia vendorizada, com `VENDOR.txt` e SPDX originais).
2. Reaproveitar o nosso shim (`port/gpu/shim`) e o adaptador `tlggpu.h`.
3. Resolver a base de memória do guest (eles usam `0x200000000`; o nosso runtime usa `0x1068000000` para o heap C):
   tradução de endereços ou mudança no runtime.
4. Reescrever as partes só de Windows (`VirtualProtect`, SRWLOCK, `HeapCompact`) para o Linux.
5. Reaplicar o upscaler temporal, a resolução nativa e o overlay por cima.
6. Testar em NVIDIA e AMD, Windows e Linux. Só então virar opção do launcher (v3 ao lado do v2, v2 continua padrão).

## O que não portar
Pular draws enquanto compila como padrão; distribuir a semente de shaders (decisão do usuário pendente, e metadados
derivados do jogo); os 28 MB de FSR 4 no git (proveniência dos pesos não verificada: se entrar, baixado pelo
jogador); DLSS 5; hashes e endereços fixos sem aprendizagem; o `dlss_bridge` deles (duplicata do nosso).

## Regras do porte
- **Licença e crédito:** GPL-2.0-or-later. Manter o `SPDX-FileCopyrightText` do axylpup nos arquivos copiados,
  `VENDOR.txt` com `axylpup/shadPS4 second-son-v1.1.1 @ 57c9f74` e `Liondarkz/issn_pc @ <hash>`, e o hash de origem na
  mensagem de cada commit. O README e as notas de release creditam o axylpup.
- **Nada da Sony no git.** Qualquer arquivo gerado do jogo fica fora do repositório.
- **Um jogo por vez**, com a trava; o agente nunca abre o jogo sem a permissão do usuário.
- Cada fase tem sua medição registrada em `docs/` e no ai-memory antes de a próxima começar.

## Estado (28/10/2026)

- **Etapa 1 (feita):** núcleo do issn_pc importado em `port/gpu_v3`, com `issn`→`tlg` renomeado.
- **Etapa 2 (feita, só compila e liga):** `cmake -S port/gpu_v3 -B port/out/gpu_v3 -G Ninja` + `ninja -C port/out/gpu_v3 tlg-probe`
  gera `tlg-probe.exe` (12 MB), com o nosso `port/src`. Nunca foi executado.
  - `third_party` vem de `port/gpu/third_party` (só `stb` é novo, domínio público).
  - Stubs de fornecedor em `shim/tlg_vendor_stubs.cpp`: Streamline DLSS, DLSS 5 e FSR 4 não são compilados
    (nada de SDK da NVIDIA nem pesos sem proveniência no repositório).
  - `shim/tlg_runtime_glue.cpp`: respostas neutras para o que o nosso runtime espera do shim e o núcleo importado não
    tem: diálogos de save (`begin` devolve 0 = sem janela), karma/troféus (nada guardado), roda de poderes, governador de
    qualidade (`tlg_view_*`), mouse capturado, `input_settings`.
- **Etapa 3a (feita):** `TlgGpuConfig` ganhou `eboot_base`, `game_dir` e `app_ver` (o runtime os preenche; v1/v2 ignoram);
  com `TLG_GPU_V3` o `probe.c` chama `tlggpu_apply_game_patches()` e `tlggpu_mark_game_main_thread()`; `runtime_memory.c`
  reserva `0x200000000-0x800000000` para o guest (a janela reservada do sistema fica dentro; `TLG_GUEST_LOW=0` desliga).
  v3 e v2 compilam. **Ainda não foi executado.**
- **Etapa 3b (a fazer):** trocar cada stub de `tlg_runtime_glue.cpp` pela implementação real de `port/gpu/shim`;
  fazer o runtime passar `eboot_base`, `game_dir`, `app_ver` e chamar `apply_game_patches`/`mark_game_main_thread`
  (campos novos do `TlgGpuConfig` do núcleo importado, que hoje o `tlggpu.h` do runtime não tem: **por isso o
  executável ainda não deve ser aberto**); resolver a base de memória do guest (`0x200000000`).
- **Etapa 4 (a fazer):** primeira execução e medição (fase 0), com a permissão do usuário e a trava.
