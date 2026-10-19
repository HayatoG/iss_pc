# DLC unlocker metadata / Metadados de DLC

## English

The base game and update 1.07 still come from the player's own dump. This helper
does not download content, decrypt packages, mount filesystems, or install game
updates. It reads only an unencrypted `PARAM_SFO` entry or an extracted `param.sfo`.

**Runtime support is a separate change:** AppContent unlocker support is proposed
in [BeastBoyyy's PR #249](https://github.com/HayatoG/iss_pc/pull/249). This helper is
a companion to that work. Importing metadata into a build without that support
does **not** enable DLC. It must not be presented as a replacement for #249.

### Inspect before importing

From a source checkout, using Python 3:

```powershell
python port/scripts/import_addcont.py --game "C:\Games\CUSA00004" "C:\OwnedDLC"
```

The JSON report shows the game's title ID, each add-on's content ID, name and
16-character entitlement label. This command writes nothing. Supply one or more
PKGs, extracted `param.sfo` files, or folders containing them. All inputs must be
category `ac` and match the base game's title ID; CUSA00004 and CUSA00223 are not
interchangeable. Conflicting duplicates, malformed metadata and encrypted SFO
entries are rejected. The PKG reader locates entry **ID `0x1000`**, rather than
assuming that `param.sfo` is always the ninth entry.

### Import the validated metadata

Repeat the same command with `--install`:

```powershell
python port/scripts/import_addcont.py --game "C:\Games\CUSA00004" --install "C:\OwnedDLC"
```

For a packaged build that includes this helper, use its own Python runtime:

```powershell
& ".\inFAMOUS Second Son.exe" --script scripts/import_addcont.py --game "C:\Games\CUSA00004" --install "C:\OwnedDLC"
```

It creates only this layout, beside the game folder:

```text
CUSA00004/
  sce_sys/param.sfo
CUSA00004-DLC/
  EP9000-CUSA00004_00-UNLOCKCOLEJACKET/
    sce_sys/param.sfo
  ...
```

The folder name must match the actual game directory: a game at `dump/uroot`
uses `dump/uroot-DLC`. In builds with #249, the ordinary launcher discovers this
sibling folder. `ISS_ADDCONT_DIR` is an alternative for an existing custom DLC
directory. No special Play shortcut is required. Identical metadata is left
untouched on repeat imports; differing existing metadata is never overwritten.

### Verify what the port received

After preparing/launching a build with #249, inspect
`out/content-profile.json`: `title_id` must match the game and `installed_addons`
must contain the imported entitlement labels. The preparation log reports
`installed add-ons without data=N`. The runtime's AppContent initialization log
reports the same count. `mounted_addons: []` is expected for entitlement-only
unlockers: there is no payload to mount.

For the inspected CUSA00004 seven-add-on set, examples include
`UNLOCKCOLEJACKET`, `UNLOCKCOLEMISSIO` and the vest unlockers. Seven is an example,
not a required count: the report must match the player's own add-ons. Verify the
actual jacket/vest options and Cole's Legacy missions in gameplay after their
normal progression requirements. An entitlement report alone does not prove
that a mission was played or that every reward is available immediately.

This importer copies **metadata only**. DLC with additional asset data requires
separate runtime/filesystem support; an empty PKG extraction result is not proof
that every DLC package is an entitlement-only unlocker.

## Português

Use apenas o jogo, a atualização 1.07 e os DLCs do seu próprio dump. A ferramenta
lê metadados não criptografados; não baixa conteúdo, não descriptografa PKGs e não
monta sistemas de arquivos. O suporte do runtime é o trabalho de **BeastBoyyy no
PR #249**. Importar os metadados sem esse suporte não libera os DLCs.

Primeiro execute o comando sem `--install` para conferir o relatório. Depois
adicione `--install` para criar `<pasta-do-jogo>-DLC/<CONTENT_ID>/sce_sys/param.sfo`.
O `TITLE_ID` deve ser o mesmo do jogo; não misture CUSA00004 e CUSA00223. Metadados
existentes diferentes, entradas criptografadas e arquivos inválidos são recusados.

Com #249, o launcher normal encontra a pasta vizinha; `ISS_ADDCONT_DIR` permite
usar uma pasta própria. Confira `installed_addons` em `out/content-profile.json`
e a contagem no log de AppContent. `mounted_addons: []` é normal para unlockers sem
dados. Confirme também os itens e missões no jogo, respeitando a progressão normal.
Pacotes com arquivos adicionais precisam de suporte próprio; esta ferramenta
importa somente `param.sfo`.
