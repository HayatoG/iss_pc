# xpps: índice dos pacotes `.xpps`

Etapa 1 da compilação de shaders na primeira execução. Lê os pacotes KCAP (`.xpps`) do jogo do próprio jogador e grava num SQLite local tudo o que as etapas seguintes precisam: shaders, cabeçalhos Gnm, registros de pass, fetch shaders, efeitos, instâncias de material, objetos textura e malhas.

Só Python com a biblioteca padrão (sem numpy), igual no Windows e no Linux. **Nada do jogo entra no git nem sai da máquina:** o código só descreve a estrutura, e os testes geram KCAP sintético.

*English summary at the end.*

## Como rodar

A partir de `port/tools`:

```sh
python -m xpps.index build <pasta com .xpps> <saida.sqlite> [--jobs N] [--full] [--verify-footers] [--nice]
python -m xpps.index stats <saida.sqlite>
python -m xpps.index sha   <saida.sqlite>
```

- `build` imprime no stdout uma linha `PROGRESS index <n> <total>` por pacote (reaproveitado ou lido), para o launcher. O resto vai para o stderr.
- `--jobs`: processos em paralelo, cada um com um pacote por vez (padrão: até 4).
- **Incremental:** um pacote só é lido de novo quando muda a impressão digital (tamanho + mtime + SHA-256 dos primeiros e últimos 64 KiB). Pacotes que sumiram saem do índice. `--full` ignora o índice anterior.
- O índice é gravado em `<saida>.tmp` e renomeado no fim: se der erro no meio, o anterior continua valendo.
- `--verify-footers` também procura cada `OrbShdr` pela assinatura e confere a regra do início. Isso lê os pacotes inteiros, então só serve para conferência.
- `sha` dá o hash do conteúdo, que não depende dos ids dos arquivos, da hora do build nem do layout do SQLite. Duas varreduras dos mesmos arquivos dão o mesmo valor (também fica em `meta.content_sha`).

Testes: `cd port && python3 -m unittest discover -s tests -p 'test_xpps_index.py'`. O teste com o jogo real só roda se existir `game/CUSA00223/art/cache` (ou `ISS_GAME`).

## Como a leitura funciona

A varredura segue as **tabelas de ligação** (`KNLI`) do pacote em vez de adivinhar ponteiros: um campo só é tratado como ponteiro quando o loader o reloca. Por isso ela lê só as estruturas de CPU e o começo dos shaders, e não os 22 GB de texturas e vértices.

| Módulo | O que lê |
|---|---|
| `kcap.py` | cabeçalho (0x98 B), seções, itens (`tipo << 16 \| alinhamento`) e BSS (itens que passam do fim dos dados) |
| `knli.py` | todas as tabelas `KNLI` (inclusive várias por pacote); relocações por tipo (0 ponteiro de CPU, 2 V#, 3 ponteiro de GPU, 4 endereço de GPU em registrador/T#); `CID` (local → classe); `XLNK` (imports: hash de 16 B → slot); tabela de exports (hash → offset) |
| `gnm.py` | rodapé `OrbShdr`; **início do código = rodapé − 8·(code[1]+1) com code[0] = 0xBEEB03FF**; cabeçalhos VS/PS/CS; InputUsageSlots (VS+0x28, PS+0x3C, CS+0x2C; 4 B `{usageType, apiSlot, startRegister, flags}`); `m_shaderSize`; tamanho do fetch até o `s_setpc_b64` |
| `scan.py` | as entidades abaixo, com os critérios descritos no próprio arquivo |
| `index.py` | SQLite, incremental, resolução dos imports, hash do conteúdo, CLI |

**Base única do registro de pass (0xB8 B):** +0x00 hash de classe, +0x20 início do objeto (entrada CID), +0x28 id do pass, +0x30 fetch, +0x38 VS, +0x40 PS, +0x48/+0x50 IRO do VS/PS (0x138 B), +0x58 lista de bytes, +0x68/+0x78/+0x88/+0x98 pares `{estágio, slot}`, +0xA8 lista de u32.

## Formato do índice (esquema 2)

Offsets relativos aos dados do pacote (`doff` do KCAP). Valores u64 (hashes, classes) ficam como int64 com sinal; hashes de 16 B e estruturas cruas ficam como BLOB.

| Tabela | Conteúdo |
|---|---|
| `schema_version` | versão do esquema (2: as mesmas tabelas do 1, mas com as tabelas de ligação divididas lidas; um índice 1 é refeito inteiro) |
| `meta` | raiz, contagens, tempo, `content_sha`, resultado da resolução dos imports |
| `files` | `id, path` (com `/`), `size, mtime_ns, fp`, cabeçalho KCAP e estatísticas da varredura (JSON) |
| `shader_occ` | uma linha por cabeçalho Gnm: `hdr_off, code_off, size` (código + rodapé), `stage, hash, footer_off` |
| `shader_hdr` | cabeçalho de cada ocorrência: `common`, `regs` (+0x10 até os slots), `slots`, `semantics` |
| `shaders` | uma linha por hash (primeira ocorrência na ordem dos caminhos), `n_occ`, `n_variants`, `n_hdr_variants` |
| `passes` | `class_hash, cid_class, pass_id, vs, ps, vs_hdr, ps_hdr, fetch_off, fetch_size, iro_vs, iro_ps, list58, list68, list78, smp_pairs` (+0x88), `tex_pairs` (+0x98), `ids_a8` |
| `effects` | `passes_off, n_passes, nmat_tex` (+0x2C), `slot_map` (+0x68), listas +0x90/+0xA0/+0xB0, `smp_desc` (+0xC0), `tex_desc` (+0xD0), `matmap` (+0xE0), `matsmp` (+0xF0) |
| `matinst` | `kind` (`local`, `import`, `unlinked`), `name_hash` (+0x18), efeito (`effect_file/off` ou `effect_import`), `block_off`, `cb_vsharp`, `n_tex`, `tex_hashes`, `tex_list` |
| `textures` | `cid_class, hash` (+0x38), `tsharp` (32 B em +0x18), `fmtword` (+0x40) |
| `meshes` | `matinst_off` (ou `matinst_import`), `n_vsharps`, `vsharps` |
| `imports`, `exports` | tabelas de ligação entre pacotes |
| `footer_check` | só com `--verify-footers`: rodapés achados pela assinatura e quantos têm exatamente um início |

## O que foi medido (CUSA00223 1.07, 1.587 pacotes, 16/10/2026)

| Item | Medido | Esperado / referência |
|---|---|---|
| Shaders únicos | 10.374 (1.986 PS, 339 VS, 8.049 CS) | 10.374 (1.986 / 339 / 8.049) |
| Inícios de código | 314.869; `--verify-footers`: 314.869 rodapés, todos com exatamente um início | 314.869 |
| Registros de pass (VS e PS) | 138.900 (nenhum só com VS) | 138.900 |
| Pares (VS, PS) distintos | 2.195; os 879 pares dos 1.381 `.key` gráficos do cache de referência estão todos no índice | 2.195; 879/879 |
| Cabeçalhos com mais de uma versão por hash | 0 | 0 |
| Efeitos | 22.518 = todos os itens de tipo 0xB (o efeito fica no item + 0x10) | 22.520 (não confirmado) |
| Instâncias de material | 33.125 locais + 65.320 importadas (64.351 resolvem para um efeito, com o mesmo número de texturas); nenhuma sem ligação desde a correção do `knli.py` (16/10) | 141.180 = 33.110 + 108.070 (não confirmado) |
| Objetos textura | 119.582, com 9.177 hashes de nome | 180.936 (errado); assinatura 120.522 |
| Malhas | 210.244, todas com efeito resolvido (80.339 local + 129.905 importado) | 178.505 (não confirmado) |
| Tempo | 44 s com o cache de páginas frio (SSD SATA), 14 s quente, 8 s sem mudanças | ≤ 120 s frio |
| Determinismo | duas varreduras completas e uma incremental: mesmo `content_sha` | igual |
| Recursos | 4 processos, ~400 MB de RSS somados; índice de 296 MiB | RAM ≤ 1,5 GB |

Por que os números não confirmados diferem:
- **Objetos textura:** a assinatura antiga (byte 0xFF em T#+0x2b, tipo de imagem) acha 940 objetos a mais. Nenhum deles tem relocação nos 0x48 bytes do objeto, e 933 estão dentro de itens de memória de GPU (dados de textura e vértices), então são falsos positivos. O índice exige a relocação do endereço do T#.
- **Instâncias de material:** até 16/10 o leitor perdia as tabelas de ligação divididas em vários itens (ver a correção na seção das etapas 2-3) e 12.209 instâncias apareciam como `unlinked`. Hoje 969 imports resolvem para algo que não é efeito e ficam com o efeito vazio. O 108.070 antigo não pôde ser refeito.
- **Malhas:** 210.244 é o total com V# relocados; a contagem antiga só considerava as cadeias com efeito resolvido e procurava os imports só na primeira tabela `KNLI`.
- **Efeitos:** a contagem estrutural bate com os 22.518 itens de tipo 0xB; os 2 a mais da conta antiga não foram achados.

## Etapas 2 e 3: jobs de pré-compilação (v2, sem captura)

`jobs.py` gera, só a partir dos arquivos do jogo do próprio jogador (os `.xpps` e o `eboot.bin`), o arquivo de jobs que o pré-compilador do port lê (`ISSPJOB1`, formato em `vk_precompile.h` e `jobfile.py`). Nada é distribuído e nenhuma captura é lida. `jobs_eval.py` só mede o gerador contra capturas de desenvolvimento (`TLG_PRECOMPILE_CAPTURE`).

### Interface para o launcher (estável)

```sh
cd port/tools
python3 -m xpps.jobs build <pasta dos .xpps> <saida.jobs> [--index <indice.sqlite>] [--jobs N] [--nice] [--compute-wide]
```

- **Fases:** primeiro o índice (incremental: refaz só os pacotes que mudaram, uns 8 s quando nada mudou) e depois os jobs.
- **Índice:** por padrão fica em `<saida sem extensão>.index.sqlite`, ao lado da saída.
- **stdout:** só linhas `PROGRESS index <n> <total>` (n = pacotes prontos) e depois `PROGRESS jobs <n> <total>` (n = grupos de pass e shaders de compute processados). O `total` é fixo dentro de cada fase, e cada fase termina com `n == total`.
- **stderr:** log livre; a última linha é um JSON com as estatísticas.
- **Código de saída:** 0 = sucesso; 1 = erro (pasta sem `.xpps`, erro de leitura etc.); 2 = uso inválido. A saída é gravada em `<saida>.tmp` e renomeada no fim, então um erro deixa a saída anterior intacta.
- **`eboot.bin`:** é procurado na pasta dos `.xpps` e em até 4 pastas acima (`<jogo>/CUSA00223/art/cache` → `<jogo>/CUSA00223/eboot.bin`). Sem ele o gerador ainda roda, mas sem as classes nem os nomes dos recursos do motor, e a cobertura cai.
- `--jobs` = processos da fase de índice (padrão: até 4); `--compute-wide` = mais registros de compute.

A mesma interface está em `~/decomp/iss_ref/agent_status/jobs_cli.txt`. `--eboot <arquivo>` (opcional) aponta o `eboot.bin` quando ele não está acima da pasta dos `.xpps`.

| Módulo | O que faz |
|---|---|
| `jobfile.py` | lê e grava o formato `ISSPJOB1` |
| `signature.py` | `signature(registro)`: só o que muda a `GraphicsPipelineKey` e a `StageSpecialization` (cada campo com o arquivo:linha do port) |
| `eboot.py` | lê o `eboot.bin` do jogador: classe CID → id de tipo, hashes guardados como imediatos no código |
| `engine_tables.py` | regras e tabelas do motor (só números), cada uma com a função e a linha do `all.c` |
| `gcn_mem.py` | acessos a buffer no código GCN (stride e escrita de um V#) |
| `jobs.py` | o gerador e a interface `build` |
| `jobs_eval.py` | `rules`, `eval` (e `infer`, só para a v1) |

Testes (dados sintéticos: KCAP, ELF e cabeçalhos inventados): `cd port && python3 -m unittest discover -s tests -p 'test_xpps_*.py'`. Medição de desenvolvimento: `python -m xpps.jobs_eval eval <captura> <indice.sqlite> [--compute-wide]`. A forma de desenvolvimento (`python -m xpps.jobs <indice> <saida> [--tables tabelas.json]`) continua para comparar com a v1.

### O que vem de onde

Toda tabela de `engine_tables.py` traz a função e a linha do `all.c` (decompilação 1.07) de onde saiu, marcada `EBOOT` (fato do código), `HYPOTHESIS` (coerente com o código, não provado) ou `MEASURED` (convenção do formato dos pacotes medida nos dados). **No git só há números e enums:** formatos, strides, máscaras, ids de tipo e endereços de código. Os hashes de classe e de nome de recurso não entram: `eboot.py` os lê no `eboot.bin` do jogador, nos endereços das instruções que os carregam (`movabs`, ou `movabs` + `lea`).

| O quê | Fonte (FATO) | No gerador |
|---|---|---|
| Classe do material pass | hash CID do pacote → tabela ordenada do motor → tabela de fábrica (va 0xd04e28 + (id−0x3ef)·0x80) → vtable; +0x28 = setup, +0x30 = draw (FUN_014484d0 l.958113) | `eboot.Eboot.type_of_class`; id de tipo por grupo de pass |
| Conjuntos de render target | FUN_012ffeb0 (l.680686) com as flags de cada alvo; formato FUN_013008e0 (l.677970); máscara padrão l.680967; G-buffer l.351036/351046; HDR l.351274/351276; sombras l.351304 e l.943294; principal l.420415 | `RT_SETS` |
| Conjunto de cada classe | buckets por classe (FUN_011fecc0 l.434165 e vizinhas) e o que cada bucket liga (FUN_011901f0…FUN_01191100) | `class_sets` (G-buffer por MRT; parte é HIPÓTESE) |
| Blend e máscara por classe | virtuais +0x28: 0x3a1 FUN_011a3f60/FUN_011a4080 (l.358981, flags da lista +0xa8 do pass, FUN_01447370 l.957901), 0x3a6 l.359969, 0x396 l.915921, 0x39d/0x39e/0x39f l.437305/436200/435949, luzes FUN_0119b930 l.355764, 0x3c0/0x3c1 tabela va 0x1ae34c0, 0x3f9 l.892144, 0x3fe l.893135, 0x3de l.393268 | `class_blends`, `flags_of` |
| Profundidade | formato pelo conjunto (Z32F, S8 quando a flag 0x4000000) | `RT_SETS` |
| Raster e VS | zclip das sombras (FUN_01439ab0 l.943535), plano de usuário dos reflexos (FUN_013bfb90 l.841000), step rate (1,1) só por 0x3fe/0x3c1 (l.893924, l.1256315) e nunca zerado | variantes em `model_states` |
| Buffers do motor por nome | o shader declara o hash no descritor do efeito (+0xd0); FUN_01445ae0 acha o slot; globais FUN_0145dba0 l.973630-973690, ligados por FUN_013f93e0 l.892737 (nulos sem dados de instância); por objeto FUN_011a1770 l.358168, FUN_0143d480 l.946295; luzes FUN_01446190 l.955339 | `ENGINE_BUFFERS` (endereço do hash → stride, formato, mtype, pode ser nulo) |
| Texturas do motor por nome | FUN_014179e0 l.915719-915770 (volumes de neblina: 2D array BC6, 2D array BC1 sRGB, 1D array BC1 sRGB); FUN_0105bab0 (cubo BC6) | `ENGINE_TEXTURES` |
| Demais texturas do motor | tipo pelo byte +8 do descritor (MEASURED: convenção do formato do efeito; não há conversão no eboot) | `DESC_TYPE_TO_IMAGE` |
| Fetch | V# da malha ligada ao efeito; entrada de 1 componente além das streams da malha = stream de índice de instância (formato 0x4402 de FUN_0145f590 l.972017, HIPÓTESE quanto à tabela de vértices); senão float de 32 bits com o número de componentes da entrada | `INSTANCE_STREAM_WITNESS`, `vertex_witness_by_size` |
| Compute | partículas: FUN_013f47e0 l.886031 + FUN_013fab70 l.891700 (slots 4,0 4,1 4,2 4,5 4,6 4,7 0,1 0,2 0,3 0,4 0,11; ver `COMPUTE_SLOTS`) | `compute_re` |

**Correção no índice (FATO):** o carregador percorre a cadeia de pedaços de todo item de ligação, qualquer que seja a etiqueta inicial (FUN_0130fc00 → FUN_0130a310, l.695800/692853). Em 54 pacotes (blocos do mundo e `core_*`) a tabela vem dividida em um item só com `KNLI` e outros que começam com `KNLX`, `" DIC"`, `PYTO` ou `PLPS`, e o `knli.py` pulava estes. Com a correção, as 12.209 instâncias "ligadas pelo jogo em tempo de execução" são imports comuns (resolvem para efeitos em `core_common` e `core_seaport`), todos os 138.900 registros de pass têm classe, e as 210.244 malhas têm efeito. O XLNK é `{hash de 16 B, u32 local, i32 soma}`.

### Medido (16/10/2026, sem nenhuma tabela inferida)

Duas capturas de desenvolvimento só como gabarito: `capture.bin` (rota de 150 s em Seattle: 613 pipelines gráficos e 413 de compute distintos) e `play_capture.bin` (outra sessão: 148 e 30). Cobertura = assinaturas da captura achadas entre as geradas; custo = registros gerados para os pares/programas da captura ÷ necessários. A v1 é medida com validação cruzada de 5 dobras (as tabelas inferidas nunca viram o par avaliado); a v2 não tem nada inferido, então não há dobras.

| | v1 `capture` | **v2 `capture`** | v1 `play` | **v2 `play`** |
|---|---|---|---|---|
| Gráficos: cobertura | 85,0% (521/613) | **92,0% (564/613)** | 75,0% (111/148) | **92,6% (137/148)** |
| Gráficos: custo | 4,43× | **4,33×** | 5,81× | **6,06×** |
| Compute: cobertura | 83,1% (343/413) | **88,1% (364/413)** | 90,0% (27/30) | **100% (30/30)** |
| Compute: custo | 1,18× | **1,18×** | 1,17× | **1,17×** |

Faltas por parte (uma assinatura pode faltar em mais de uma):

| Parte | v1 `capture` | v2 `capture` | v1 `play` | v2 `play` |
|---|---|---|---|---|
| `rt` | 47 | 20 | 12 | 2 |
| `fetch` | 26 | 13 | 8 | 0 |
| `ps_res` | 22 | 11 | 11 | 5 |
| `vs_res` | 22 | 3 | 14 | 2 |
| `depth` | 10 | 12 | 3 | 4 |
| `raster` | 5 | 7 | 3 | 4 |
| `vs_rt` | 5 | 6 | 5 | 4 |
| `cs_res` | 70 | 49 | 3 | 0 |

- `--compute-wide` (`capture`): compute 92,3% com 1,94×.
- **Sem o `eboot.bin`:** gráficos 7,5% (sem classes nem nomes de recursos); compute igual.
- **Jogo inteiro:** 22.634 registros (9.148 gráficos + 13.486 de compute), 99 MB, 47 s com o índice já feito (o índice frio leva mais 30 a 45 s) e 368 MB de RSS.

**O que ainda falta (`capture`):**
- `rt` 20: passes do motor cujo código de desenho não foi lido (0x3d8, 0x3f4/0x3f5/0x3f7, 0x3fb/0x3fd/0x3ff, 0x3e6, 0x3b4) e blends das classes 0x3c0/0x3c1 escolhidos por um recurso de dados (+0x4c);
- `fetch` 13: malhas cuja classe numérica do primeiro atributo não está entre as 12 variantes de material geradas;
- `depth`/`raster`/`vs_rt` (~25 no total, sobrepostos): estado herdado de draws anteriores (zclip e plano de usuário fora das sombras, depth test dos passes do motor);
- `ps_res`/`vs_res` 14: texturas do motor que não são o tipo do descritor (dois cubos escolhidos por dados) e os slots 12-15 do VS das partículas;
- `cs_res` 49: os 13 CS do motor (17 especializações, imagens e samplers ligados por código) e o stride de s[0:3] das partículas (16/4: `--compute-wide`).

**Limites.**
- Cada captura cobre uma rota de jogo; o desperdício real só aparece com o jogo inteiro.
- A assinatura é um modelo em Python do código C++; o teste final é no jogo.
- Hipóteses ainda abertas estão marcadas em `engine_tables.py`: o bucket de algumas classes, o estado dos passes do motor (pass id 255) e a tabela de vértices da stream de instância.

### v1 (com tabelas inferidas da captura, só para comparar)

`python -m xpps.jobs_eval infer` grava as tabelas INFERIDAS (`tables.json`, fora do git) e `eval --tables tables.json --cv 5` mede com validação cruzada. A v1 usava perfis de estado e de sharps do motor tirados de outros pares da mesma captura.

### Testemunhas observadas (renderer v2)

Os buffers do motor que o modelo não conhece saem com o fallback (stride 16), e a pré-compilação monta permutações de vertex shader que o jogo nunca pede: no renderer v2, 1.370 dos 1.599 pipelines compilados jogando (rota de combate, cache vazio) eram uma permutação nova de um vertex shader que já estava nos jobs.

- `ISS_RECORD_WITNESSES=1` (renderer v2, `gpu_v2/gpu/witness_record.inl`): cada pipeline gráfico compilado jogando grava uma linha em `<user>/cache/<serial>-witnesses.jsonl` com as dwords do flat de onde os sharps do VS e do PS são lidos e os V# de cada atributo do fetch. Os endereços viram os placeholders de `words_of` e o tamanho dos buffers vira ligado/desligado: nada de código nem de endereço.
- `python -m xpps.witnesses merge <jsonl...> -o xpps/data/CUSA00223-witnesses.json` confere isso e junta as linhas por par de shaders. A tabela vai no git e no pacote (`cp -r tools/xpps`), e entra no hash do gerador (`launcher/precompile.py`), então os jobs são refeitos quando ela muda.
- `jobs.py` (produto, sem `--tables`): para cada par da tabela, uma cópia de cada registro do par por variante observada, com aquelas dwords e V#; a assinatura descarta as que já existem.

**Medido (25/10/2026, RTX 4070, mesma rota, cache vazio, tabela de uma sessão):** +3.351 registros gráficos (14.670 pipelines pré-compilados em 36,6 s contra 11.368 em 23,6 s); jogando, ~470 pipelines realmente novos contra ~1.780, 115 módulos de shader contra 316 e 0,8 s de espera do gravador por pipelines contra 2,9 s. O que sobra é sobretudo par VS+PS que a tabela ainda não viu: mais sessões gravadas (outras rotas, outras missões) cobrem mais.

---

## English summary

`xpps` builds a local SQLite index of the game's KCAP packages for the first-run shader precompiler (stage 1). It follows the packages' relocation tables instead of guessing pointers, so it only reads CPU structures and shader heads. Stdlib only.

- Run from `port/tools`: `python -m xpps.index build <dir> <out.sqlite> [--jobs N]`; progress lines `PROGRESS index <n> <total>` on stdout. Incremental by fingerprint (size + mtime + SHA-256 of the first/last 64 KiB); `sha` prints a content hash that is stable across builds.
- Code start rule: start = footer − 8·(code[1]+1), code[0] = 0xBEEB03FF (the footer's length field is not the code size).
- Measured on the game: 10,374 unique shaders, 314,869 code starts, 138,900 pass records, 2,195 VS/PS pairs (879/879 of the reference cache), 22,518 effects, 119,582 texture objects, 210,244 meshes; 44 s cold, 14 s warm.
- Tests: `python3 -m unittest discover -s tests -p 'test_xpps_index.py'` (synthetic KCAP; the real-game test is skipped without the game folder).
- Stages 2-3 (`jobs.py`, `eboot.py`, `engine_tables.py`, `jobs_eval.py`, `signature.py`): job builder v2 from the player's own files only (the `.xpps` and `eboot.bin`), no capture. Launcher interface: `python -m xpps.jobs build <xpps dir> <out.jobs> [--index <sqlite>]` prints `PROGRESS index|jobs <n> <total>` and exits 0 on success. The engine model (render target sets, per-class blend/mask, engine buffers and textures by descriptor name hash, instance stream) comes from the 1.07 executable, every value with its all.c line; class and name hashes are read from the player's eboot at code addresses, so git only holds numbers. Measured with no inferred table: 92.0% of the graphics pipelines of a 150 s capture (4.3 records per needed one) and 92.6% of another session (6.1x); compute 88.1% and 100% (1.2x). Whole game: 22,634 records in 47 s. The link-table reader now follows tables split across several items (12,209 material instances were wrongly "unlinked").
- Observed witnesses (`witnesses.py`): renderer v2 with `ISS_RECORD_WITNESSES=1` writes, per graphics pipeline compiled in play, the flat dwords the VS/PS sharps are loaded from and the fetch attributes' V#, addresses replaced by placeholders; `python -m xpps.witnesses merge` builds `data/<serial>-witnesses.json`, part of the generator hash, and `jobs.py` adds one copy of a pair's records per observed variant. Same route, empty cache: ~470 pipelines new in play instead of ~1,780, 115 shader modules instead of 316.
