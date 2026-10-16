# SPDX-License-Identifier: GPL-2.0-or-later
"""The launcher's "Advanced" screen: the environment-only tuning parameters of the GPU backend and
the runtime that the Fixes screen does not cover (threads, readbacks, synchronisation, pipeline
build, image workarounds, diagnostics).

Same state model as backports_table.py (this module only holds the rows): a choice is kept in
tlg.ini only when it differs from the default (key `adv_<name>`), is given to the game as
ISS_<name>=<value> (the probe copies ISS_X to TLG_X) and an ISS_/TLG_ variable already in the
environment wins. A parameter the player never touched changes nothing. Default None = the code
decides (per driver, per CPU); the control then shows "Automatic".

tests/test_backports_table.py checks that every TLG_* name here exists in the sources.
Catalog and evidence: CATALOGO_PARAMETROS_ISS.md.
"""

# area ids -> (title en, title pt); the order is the order on the screen
AREAS = (
    ('sync', 'Synchronisation and timing', 'Sincronização e tempo'),
    ('threads', 'Threads and queues', 'Threads e filas'),
    ('read', 'GPU readbacks and memory', 'Leituras da GPU e memória'),
    ('compile', 'Shaders and pipelines', 'Shaders e pipelines'),
    ('image', 'Image workarounds', 'Contornos de imagem'),
    ('diag', 'Diagnostics (logs)', 'Diagnóstico (logs)'),
)

# (area, name, type, default, (name en, pt), (what en, pt), extra)
# type: 'bool' | 'int' | 'choice'. extra: lo/hi/unit for int, choices for choice, risky=True.
_ROWS = (
    ('sync', 'FULL_BARRIERS', 'bool', '0', ('Full barriers everywhere', 'Barreiras totais em tudo'),
     ('Puts a full stop between every GPU step, to tell whether a glitch is a missing synchronisation. '
      'Large FPS loss (roughly 20-50 %); for tests only.',
      'Coloca uma parada total entre cada etapa da placa de vídeo, para descobrir se um defeito é falta de '
      'sincronização. Perde bastante FPS (algo entre 20 e 50 %); só para testes.'), {'risky': True}),
    ('sync', 'GPU_TIMESTAMP', 'choice', 'host', ('GPU clock', 'Relógio da placa de vídeo'),
     ('What time the game reads from the GPU. "Fixed steps" advances by a constant step instead of the PC clock: '
      'a test for distant smoke that flickers.',
      'Que hora o jogo lê da placa de vídeo. "Passos fixos" anda um passo constante em vez do relógio do PC: '
      'um teste para a fumaça distante que pisca.'),
     {'choices': (('host', 'PC clock', 'Relógio do PC'), ('fixed', 'Fixed steps', 'Passos fixos')), 'risky': True}),
    ('sync', 'GPU_TIMESTAMP_STEP_US', 'int', '20', ('Fixed clock step', 'Passo do relógio fixo'),
     ('Step of the fixed GPU clock, in microseconds. Only used with "Fixed steps".',
      'Passo do relógio fixo da placa de vídeo, em microssegundos. Só vale com "Passos fixos".'),
     {'lo': 1, 'hi': 1000, 'unit': 'µs'}),
    ('sync', 'ASYNC_FENCES', 'bool', '0', ('Signal GPU completion early', 'Avisar o fim da GPU mais cedo'),
     ('Tells the game the GPU finished before the memory copies end. Measured slower (about 5 %).',
      'Avisa ao jogo que a GPU terminou antes de as cópias de memória acabarem. Medido mais lento (cerca de 5 %).'),
     {'risky': True}),
    ('sync', 'PRESENT_THREAD', 'bool', '1', ('Present on its own thread', 'Apresentar numa thread própria'),
     ('Shows the finished image from a separate thread. Turn off only when the window misbehaves.',
      'Mostra a imagem pronta a partir de uma thread separada. Desligue só se a janela se comportar mal.'), {}),
    ('sync', 'VK_RECORD_THREAD', 'bool', '1', ('Record GPU commands on their own thread', 'Gravar comandos da GPU numa thread própria'),
     ('A helper thread writes down the GPU orders. Off is slower; useful to rule out a thread race.',
      'Uma thread ajudante anota os pedidos à placa de vídeo. Desligado fica mais lento; serve para descartar uma corrida entre threads.'), {}),
    ('threads', 'DRAW_PIPE', 'bool', None, ('Draw pipeline', 'Linha de montagem de desenhos'),
     ('Prepares draws on one thread and records them on another (about 18 % more FPS). Automatic: on with 8 or more CPU threads.',
      'Prepara os desenhos numa thread e grava noutra (cerca de 18 % a mais de FPS). Automático: ligado com 8 ou mais threads de CPU.'), {}),
    ('threads', 'PREP_WORKERS', 'int', None, ('Draw preparation workers', 'Threads de preparação de desenhos'),
     ('How many threads prepare draws in parallel. 0 turns the look-ahead off (about 10 % FPS lost). Automatic: half the CPU threads.',
      'Quantas threads preparam desenhos em paralelo. 0 desliga a antecipação (perde cerca de 10 % de FPS). Automático: metade das threads da CPU.'),
     {'lo': 0, 'hi': 16}),
    ('threads', 'COPY_THREADS', 'int', None, ('Memory copy threads', 'Threads de cópia de memória'),
     ('Threads that copy guest memory for the GPU. Automatic: a quarter of the CPU threads.',
      'Threads que copiam a memória do jogo para a GPU. Automático: um quarto das threads da CPU.'), {'lo': 0, 'hi': 16}),
    ('threads', 'SUBMIT_DRAWS', 'int', '1024', ('Draws per batch', 'Desenhos por lote'),
     ('How many draws pile up before the batch goes to the GPU. 0 sends only at the end of each game submission.',
      'Quantos desenhos se acumulam antes de o lote ir à GPU. 0 envia só no fim de cada submissão do jogo.'),
     {'lo': 0, 'hi': 4096}),
    ('threads', 'PIPE_SPIN_US', 'int', None, ('Draw thread spin time', 'Tempo de espera ativa da thread de desenhos'),
     ('How long the recording thread keeps watching before it sleeps. More reacts faster and uses more CPU. Automatic: 50 to 200.',
      'Quanto tempo a thread de gravação fica de olho antes de dormir. Mais reage mais rápido e gasta mais CPU. Automático: 50 a 200.'),
     {'lo': 0, 'hi': 1000, 'unit': 'µs'}),
    ('threads', 'PIPE_VERIFY', 'int', '0', ('Verify prepared draws every N', 'Conferir desenhos preparados a cada N'),
     ('Every N draws, compares the prepared state with the real one to find preparation mistakes. 0 = off. Costs some FPS.',
      'A cada N desenhos, compara o estado preparado com o real para achar erros de preparação. 0 = desligado. Custa um pouco de FPS.'),
     {'lo': 0, 'hi': 1000}),
    ('read', 'EAGER_READBACK', 'bool', '1', ('Read ahead of the game', 'Ler antes de o jogo pedir'),
     ('Copies the memory areas the game usually reads back to RAM ahead of time. Off waits for the GPU on every read.',
      'Copia antes as áreas de memória que o jogo costuma ler de volta. Desligado espera a GPU em cada leitura.'), {}),
    ('read', 'WRITE_MERGE', 'bool', '1', ('Merge game writes', 'Juntar escritas do jogo'),
     ('Writes the game makes into memory the GPU also touches go straight through, without waiting.',
      'Escritas do jogo em memória que a GPU também mexe passam direto, sem esperar.'), {}),
    ('read', 'FAST_READ_FAULT', 'bool', '1', ('Fast read faults', 'Resposta rápida a leituras'),
     ('Answers memory reads of the game quickly, without a round trip to the GPU thread. Off loses a lot of FPS.',
      'Responde rápido às leituras de memória do jogo, sem ir até a thread da GPU. Desligado perde muito FPS.'), {}),
    ('read', 'READBACK_LINEAR', 'bool', '0', ('Read back linear images', 'Ler de volta imagens lineares'),
     ('Also sends plain row-by-row images back to the CPU. Off for this game.',
      'Devolve à CPU também imagens simples, linha por linha. Desligado neste jogo.'), {'risky': True}),
    ('read', 'DOWNLOAD_WINDOW_KB', 'int', '512', ('Download window', 'Janela de download'),
     ('Size of the memory chunk read back at once around the address the game read.',
      'Tamanho do pedaço de memória lido de uma vez ao redor do endereço que o jogo leu.'),
     {'lo': 64, 'hi': 8192, 'unit': 'KiB'}),
    ('read', 'FAULT_WINDOW', 'int', '256', ('Unprotect window', 'Janela de desproteção'),
     ('How much memory is released from protection after a game write. A larger window means fewer interruptions.',
      'Quanta memória é liberada da proteção depois de uma escrita do jogo. Janela maior significa menos interrupções.'),
     {'lo': 4, 'hi': 1024, 'unit': 'KiB'}),
    ('read', 'COPY_MERGE_KB', 'int', '64', ('Copy merge distance', 'Distância para juntar cópias'),
     ('Largest gap across which several small buffer copies are merged into one.',
      'Maior distância em que várias cópias pequenas de buffer são juntadas numa só.'),
     {'lo': 1, 'hi': 65536, 'unit': 'KiB'}),
    ('read', 'STAGING_KEEP_MB', 'int', '512', ('Transfer memory kept', 'Memória de transferência guardada'),
     ('How much temporary transfer memory is kept around. Lower it only if RAM is short.',
      'Quanta memória temporária de transferência fica guardada. Reduza só se faltar RAM.'),
     {'lo': 64, 'hi': 2048, 'unit': 'MB'}),
    ('read', 'STREAM_MEMORY', 'choice', 'cached', ('Where small copies stage', 'Onde as cópias pequenas ficam'),
     ('Where 128 MB of scratch memory for small copies lives: cached RAM, uncached RAM or video memory.',
      'Onde ficam 128 MB de memória temporária para cópias pequenas: RAM com cache, sem cache ou memória de vídeo.'),
     {'choices': (('cached', 'Cached RAM', 'RAM com cache'), ('host', 'Uncached RAM', 'RAM sem cache'),
                  ('device', 'Video memory', 'Memória de vídeo'))}),
    ('compile', 'ASYNC_WAIT_MS', 'int', '0', ('Wait for a compiling pipeline (per draw)', 'Esperar um pipeline que está compilando (por draw)'),
     ('A draw whose pipeline is still compiling in the background is skipped (missing ground or effects). With a value above 0 the '
      'game first waits for the compile up to that many milliseconds. 0 = never wait (the default).',
      'Um draw cujo pipeline ainda compila em segundo plano é pulado (chão ou efeitos faltando). Com um valor acima de 0 o jogo '
      'espera a compilação por até essa quantidade de milissegundos. 0 = nunca esperar (o padrão).'),
     {'lo': 0, 'hi': 100, 'unit': 'ms'}),
    ('compile', 'ASYNC_WAIT_FRAME_MS', 'int', '8', ('Wait limit per frame', 'Limite de espera por quadro'),
     ('The most time one frame may spend waiting for compiling pipelines. Only used when the per-draw wait above is on.',
      'O máximo de tempo que um quadro pode gastar esperando pipelines em compilação. Só vale com a espera por draw ligada.'),
     {'lo': 0, 'hi': 100, 'unit': 'ms'}),
    ('compile', 'DRIVER_CACHE_SAVE_S', 'int', '60', ('Driver cache save interval', 'Intervalo para salvar o cache do driver'),
     ('How often the graphics driver cache is written to disk. A crash loses at most this much of what the driver learned.',
      'De quanto em quanto tempo o cache do driver de vídeo é gravado em disco. Um crash perde no máximo esse tempo do que o driver aprendeu.'),
     {'lo': 10, 'hi': 3600, 'unit': 's'}),
    ('compile', 'PARALLEL_WARMUP', 'bool', '0', ('Warm up pipelines on all cores', 'Aquecer pipelines em todos os núcleos'),
     ('Loads the saved pipelines at start on many cores. Faster start, short CPU peak.',
      'Carrega os pipelines salvos no início usando vários núcleos. Início mais rápido, com um pico curto de CPU.'), {}),
    ('compile', 'COMPUTE_ASYNC_BUILD', 'bool', '1', ('Build compute pipelines in the background', 'Montar pipelines de computação em segundo plano'),
     ('Builds GPU compute pipelines in the background. Off freezes the game for 0.6 to 3.8 s on the first use.',
      'Monta os pipelines de computação da GPU em segundo plano. Desligado congela o jogo de 0,6 a 3,8 s no primeiro uso.'), {}),
    ('compile', 'SQUARE_PASS', 'bool', '1', ('Second pass for min/max blends', 'Segunda passada em misturas mín/máx'),
     ('Draws twice for some colour blends (MIN/MAX) to get the right result. Off gains a little FPS and some effects look wrong.',
      'Desenha duas vezes em certas misturas de cor (MIN/MAX) para dar o resultado certo. Desligado ganha um pouco de FPS e alguns efeitos saem errados.'),
     {'risky': True}),
    ('image', 'INTERP_INT_FIX', 'bool', None, ('Integer interpolation fix', 'Correção de interpolação inteira'),
     ('Fix needed only on old NVIDIA cards (Pascal). Automatic decides by the GPU.',
      'Correção necessária só em placas NVIDIA antigas (Pascal). O automático decide pela placa.'), {'risky': True}),
    ('image', '1D_NO_STORAGE', 'bool', '0', ('1D images without storage use', 'Imagens 1D sem uso de armazenamento'),
     ('AMD workaround test: creates 1D images without the storage usage flag.',
      'Teste de contorno para AMD: cria imagens 1D sem o uso de armazenamento.'), {'risky': True}),
    ('image', '1D_NO_MUTABLE', 'bool', '0', ('1D images without mutable format', 'Imagens 1D sem formato mutável'),
     ('AMD workaround test: creates 1D images without the mutable-format flag.',
      'Teste de contorno para AMD: cria imagens 1D sem o formato mutável.'), {'risky': True}),
    ('diag', 'REGION_TRACE', 'bool', '0', ('Trace regions and pipelines', 'Rastrear regiões e pipelines'),
     ('Writes region_trace.tsv in the GPU user folder: every .xpps package the game opens and the first request of each '
      'pipeline. For studying which pipelines each area needs.',
      'Grava region_trace.tsv na pasta do usuário da GPU: cada pacote .xpps que o jogo abre e o primeiro pedido de cada '
      'pipeline. Serve para estudar quais pipelines cada área precisa.'), {}),
    ('diag', 'OCCLUSION_LOG', 'bool', '0', ('Log occlusion queries', 'Registrar consultas de oclusão'),
     ('Writes the first visibility-query events to the log. For investigating lights and smoke.',
      'Escreve no log os primeiros eventos de consulta de visibilidade. Para investigar luzes e fumaça.'), {}),
    ('diag', 'IMG_TRACE', 'bool', None, ('Trace 1D image operations', 'Rastrear operações em imagens 1D'),
     ('Logs every touch of a 1D image. Automatic: on for AMD.',
      'Registra cada toque numa imagem 1D. Automático: ligado em AMD.'), {}),
    ('diag', 'BC1D_LOG', 'bool', '0', ('Log unsupported BC 1D images', 'Registrar imagens BC 1D não suportadas'),
     ('One log line per image created although the driver says its format is unsupported.',
      'Uma linha no log por imagem criada mesmo com o driver dizendo que o formato não é suportado.'), {}),
    ('diag', 'WAVE_STATS', 'bool', '0', ('Log wave statistics', 'Registrar estatísticas de ondas'),
     ('Logs the subgroup width used by the GPU. May change the driver cache.',
      'Registra a largura de subgrupo usada pela GPU. Pode mudar o cache do driver.'), {'risky': True}),
)


def build(make):
    """The Item rows (group 'adv'); `make` is backports_table._make."""
    items = []
    for area, name, vtype, default, label, what, extra in _ROWS:
        items.append(make(
            'TLG_' + name, 'adv', 'adv_' + name.lower(), 'TLG_' + name, vtype, default, label, what,
            choices=extra.get('choices', ()), lo=extra.get('lo', 0), hi=extra.get('hi', 0),
            unit=extra.get('unit', ''), pass_env=True, risky=bool(extra.get('risky')), tab=area))
    return items
