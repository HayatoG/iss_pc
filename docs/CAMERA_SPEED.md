# Optional camera speed mod / Mod opcional de velocidade da câmera

## English

In the updated launcher, open **Mods**, check **Camera speed 1.5x**, and start the
game once. Uncheck it before the next launch to restore normal speed. It defaults
to off and is stored in the existing `mods.json`, for both Windows and Linux x86_64.
The checkbox controls both axes and overrides inherited camera scale variables.

Troubleshooting: one Windows tester reported that the mod took effect on the second
launch. If it appears inactive after enabling it, close the game and launch it again.
This is a reported workaround, not an intended two-launch requirement. If it still
does not work, check the startup log for the enabled message or a signature/version
rejection; the cause of the first-launch report has not been established.

The native runtime raises the effective horizontal and vertical angular-speed
parameters after zoom interpolation and mode preferences. It does not amplify
stick axes. Both turn directions share the same axis scale. Adaptive input gain,
acceleration, pitch limits, collision and scripted-camera behavior remain in the game;
this is not a removal of the observed diagonal slowdown or a guarantee of 50% more
angular movement in every situation.

Only CUSA00223, APP_VER 01.07, with four original-instruction signatures is accepted.
The researched ELF SHA256 is
`b3afd173a379f83e1e408e71fdef22b78f93092d332b79decd48a18a08bfc605`.
At ELF VA `0x14d943`, the displaced 34-byte block stores the horizontal parameter at
`rsp+0x104` and computes/stores the vertical parameter at `rsp+0x108`. The block at
`0x14a810` consumes them through `r12+4` and `r12+8`. A live Windows trace confirmed
1.4 → 2.1 horizontally and 0.75 → 1.125 vertically; acceleration stayed 21.
The hook replays the displaced instructions, scales those parameters, and returns
to `0x14d965`. It does not assume that the metadata offsets directly identify a
live camera object. Addresses are relative to the runtime's guest image.

Installation is before guest execution and after ISA/red-zone rewrites. Four byte
signatures and executable-segment bounds fail closed. The trampoline uses existing
platform allocation/protection helpers and requires AVX, as do the original instructions.
The game ELF on disk is not edited. No debugger or extra player dependency is added.
The enabled startup log begins `Camera speed: horizontal 1.5x, vertical 1.5x`;
a version/signature mismatch logs that camera speed is disabled.

### Builds and downloads

The existing **Build** workflow packages both runtimes and the launcher with its
dependencies. On this PR's Actions run, download `infamous-windows-x86_64` for the
Windows ZIP or `infamous-linux-x86_64` for the Linux `.tar.gz` (Actions wraps downloads
in a ZIP). They contain no game files. Fork PR builds may require maintainer approval.
These are CI artifacts; merging a PR does not publish a release.

For local builds use the platform dependency lists in `.github/workflows/build.yml`,
`port/packaging/linux/build_deps.sh`, `port/shell.nix`, and the existing build scripts:

```sh
cd port
TLG_RENDERER_V2=1 bash build.sh
# Linux:
bash packaging/linux/package.sh
# Windows, MSYS2 CLANG64 with Windows Python configured as WINPYTHON:
bash packaging/windows/package.sh
```

The portable `camera-hook-test` executes generated code against a synthetic fixture,
so CI needs no proprietary ELF. An optional first argument audits a locally owned ELF.
The launcher test covers default off, persistence, independent mod selections and
the actual startup environment. Local Windows hook execution and frozen launcher
checks passed during development. Linux compilation and gameplay must be verified
by CI and a Linux tester; a successful CI build is not a gameplay test. Test both
directions, vertical movement, zoom, both renderers and scripted cameras before release.

## Português (Brasil)

No launcher atualizado, abra **Mods**, marque **Camera speed 1.5x** e inicie o jogo.
Desmarque antes da próxima inicialização para restaurar a velocidade normal. A opção
vem desativada e é salva no `mods.json`, tanto no Windows quanto no Linux x86_64.

Solução de problemas: um jogador no Windows relatou que o mod funcionou na segunda
inicialização. Se ele parecer inativo após ser marcado, feche o jogo e abra novamente.
Esse é um contorno relatado, não uma exigência normal de duas inicializações. Se o
problema persistir, consulte o log para verificar a ativação ou uma rejeição por versão
ou assinatura; a causa da falha na primeira tentativa ainda não foi identificada.

O mod aumenta em 50% os parâmetros de velocidade angular horizontal e vertical,
sem multiplicar os eixos do controle. O ganho adaptativo, a aceleração e os limites
verticais continuam ativos; a redução de velocidade observada na diagonal permanece.
O resultado não garante 50% mais movimento em todas as situações.

Somente CUSA00223, atualização 01.07, com as assinaturas originais verificadas é aceito.
O runtime instala o hook antes de executar o jogo, sem editar o ELF no disco.
Uma incompatibilidade de versão ou assinatura desativa o recurso e aparece no log.
Não é necessário depurador nem uma dependência adicional para o jogador.

O workflow **Build** existente gera os pacotes e suas dependências para as duas
plataformas. Nos artefatos da execução deste PR, baixe `infamous-windows-x86_64`
(ZIP do Windows) ou `infamous-linux-x86_64` (`.tar.gz` do Linux, dentro do ZIP do
Actions). Nenhum arquivo do jogo é incluído. A publicação de uma release continua manual.

Os testes executam o código gerado sem precisar do ELF proprietário e verificam
a seleção do mod e as variáveis usadas ao iniciar o jogo. Os testes locais do Windows
passaram durante o desenvolvimento. A compilação e a execução no Linux ainda precisam
de validação pelo CI e por um jogador no Linux. Antes da publicação, teste os dois
sentidos, o movimento vertical, o zoom, os dois renderizadores e as câmeras de cenas.
