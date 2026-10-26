#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Prepare and run inFAMOUS Second Son (CUSA00223) through the native loader.

Shared by the command line (run_infamous.sh -> `infamous_run.py [seconds]`) and the launcher
window (infamous_launcher.py), so both start the game the same way:
  1. prepare.py / link_modules.py / content_profile.py build out/boot-linked.bin from the game
     folder (eboot + the game's own libc and Fios2; no console firmware module is needed);
  2. the defaults below become ISS_* environment variables (each one validated in play);
  3. tlg-probe runs with its output in out/logs/run-<date>.log (last_run.log points to it).
"""
import datetime
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import time

import addons
import backports_table
import logtail
import shader_cache
import padsettings
import savemanager
import userdata

# A packaged build (PyInstaller) keeps its folders next to the executable: scripts/, out/, user/.
FROZEN = bool(getattr(sys, 'frozen', False))
PORT_DIR = Path(sys.executable).resolve().parent if FROZEN else Path(__file__).resolve().parent.parent
REPO_DIR = PORT_DIR.parent
OUT_DIR = PORT_DIR / 'out'
BUILTIN_MODS = (PORT_DIR / 'patches' / 'iss_builtin_intro.xml',)
sys.path.insert(0, str(PORT_DIR / 'scripts'))
import mod_patches  # noqa: E402


def user_dir():
    """The folder of saves, tlg.ini, backups and GPU caches: ISS_USER_DIR, else the per-user pointer
    file (survives reinstalling), else <port folder>/user (userdata.py has the rules). Always call
    this instead of caching it: the Qt launcher changes it while running."""
    return userdata.user_dir(PORT_DIR)


def mods_dir():
    """ISS_MODS_DIR, else user_dir()/mods (created empty on first use so the player sees where
    to drop shadPS4/GoldHEN cheat XML files). Not cached: user_dir() can change while running."""
    directory = Path(os.environ.get('ISS_MODS_DIR') or user_dir() / 'mods')
    directory.mkdir(parents=True, exist_ok=True)
    return directory


def mods_config():
    return Path(os.environ.get('ISS_MODS_CONFIG') or user_dir() / 'mods.json')


# Native runtime feature, stored with the existing Mods selections. This key is not
# an XML patch: the runtime allocates a guarded trampoline before guest execution.
CAMERA_MOD_KEY = 'native/camera-speed-1.5x'
CAMERA_PITCH_MOD_KEY = 'native/camera-prevent-pitch-slowdown'


def camera_mod_enabled():
    try:
        config = json.loads(mods_config().read_text())
        return (isinstance(config, dict) and
                CAMERA_MOD_KEY in config.get('enabled', []) and
                CAMERA_MOD_KEY not in config.get('disabled', []))
    except (OSError, ValueError, TypeError):
        return False


def camera_mod_environment():
    scale = '1.5' if camera_mod_enabled() else '1'
    return {'ISS_CAMERA_PAN_SCALE': scale, 'ISS_CAMERA_TILT_SCALE': scale}


def camera_pitch_mod_enabled():
    # Both runtime features follow the single saved camera selection.
    return camera_mod_enabled()


def camera_build_supported(game, app_version):
    if app_version == '01.07':
        return True  # Existing catalogue behavior; runtime still verifies title and code.
    if app_version != '01.00':
        return False
    try:
        from prepare import sfo
        from infamous_patches import read_eboot_elf
        import hashlib
        metadata = sfo((Path(game) / 'sce_sys/param.sfo').read_bytes())
        if metadata.get('TITLE_ID') != 'CUSA00004':
            return False
        elf = read_eboot_elf(game)
        # The verified 1.07 executable, identical across the tested US/EU dumps.
        return elf is not None and hashlib.sha256(elf).hexdigest() == \
            'b3afd173a379f83e1e408e71fdef22b78f93092d332b79decd48a18a08bfc605'
    except (OSError, ValueError):
        return False


def list_mods(settings):
    """[(key, name, note, enabled)] for the Mods tab: every shadPS4/GoldHEN cheat XML found in
    the mods folder for this game's version, with its current enabled state."""
    from infamous_patches import TITLE_IDS
    app_version = mod_patches.game_app_version(settings['game_dir'])
    found = mod_patches.external_patches(
        mods_dir(), app_version, TITLE_IDS, builtin_files=BUILTIN_MODS)
    selected = {key for key, _, _ in mod_patches.external_selection(found, mods_config())}
    rows = [(key, meta.get('Name'), meta.get('Note', ''), key in selected) for key, _, meta in found]
    if camera_build_supported(settings['game_dir'], app_version):
        rows.append((CAMERA_MOD_KEY, 'Camera improvements (1.5x + slowdown fix)',
                     'Raises horizontal and vertical camera speed limits by 50% and prevents '
                     'diagonal slowdown at vertical viewing limits with zoom off. '
                     'Zoomed aiming and vertical limits remain unchanged. '
                     'Requires a verified CUSA00223/CUSA00004 1.07 build. Applies on the next game launch.',
                     camera_mod_enabled()))
    return rows


def set_mod_enabled(key, enabled):
    """Updates mods.json: moves key into enabled/disabled, out of the other list."""
    path = mods_config()
    try:
        data = json.loads(path.read_text()) if path.is_file() else {}
    except (OSError, ValueError):
        data = {}
    on, off = set(data.get('enabled', [])), set(data.get('disabled', []))
    if key == CAMERA_MOD_KEY:
        on.discard(CAMERA_PITCH_MOD_KEY)
        off.discard(CAMERA_PITCH_MOD_KEY)
    (on if enabled else off).add(key)
    (off if enabled else on).discard(key)
    path.write_text(json.dumps({'enabled': sorted(on), 'disabled': sorted(off)}, indent=2))


def __getattr__(name):  # old code that still reads tlg_run.USER_DIR gets the current folder
    if name == 'USER_DIR':
        return user_dir()
    raise AttributeError(name)


LOG_DIR = OUT_DIR / 'logs'
PROBE = OUT_DIR / ('tlg-probe.exe' if os.name == 'nt' else 'tlg-probe')
# TLG_PROBE (ISS_PROBE): another probe build next to it, e.g. tlg-probe-v2 (renderer v2).
if os.environ.get('TLG_PROBE') or os.environ.get('ISS_PROBE'):
    PROBE = OUT_DIR / Path(os.environ.get('TLG_PROBE') or os.environ['ISS_PROBE']).name
# Renderer v2 (port/gpu_v2, the launcher's "Renderer" choice): its probe and its own defaults.
V2_PROBE = OUT_DIR / ('tlg-probe-v2.exe' if os.name == 'nt' else 'tlg-probe-v2')
V2_DEFAULTS = PORT_DIR / 'gpu_v2' / 'defaults.env'
KEEP_LOGS = 30


def renderer_v2(settings):
    """True when the player chose renderer v2 and it is built (else this port's renderer)."""
    item = next(i for i in backports_table.ITEMS if i.key == 'renderer')
    return backports_table.effective(item, experimental_states(settings)) == '2' and V2_PROBE.is_file()


def renderer_v2_env(settings):
    """Its launcher's defaults (gpu_v2/defaults.env), the game folder its shader index reads, and the
    native passes and unified memory as the launcher's switches say (Windows: on and off)."""
    env = {}
    try:
        for line in V2_DEFAULTS.read_text(encoding='utf-8').splitlines():
            key, sep, value = line.strip().partition('=')
            if sep and key and not key.startswith('#'):
                env[key] = value
    except OSError:
        pass
    env['ISS_GAME_DIR'] = settings['game_dir']
    # Native passes and unified memory: the launcher's switches (backports_table.py has the defaults).
    states = experimental_states(settings)
    for key, var in (('native_passes', 'ISS_NATIVE_PASSES'), ('v2_unified_memory', 'ISS_UNIFIED_MEMORY')):
        item = next(i for i in backports_table.ITEMS if i.key == key)
        env[var] = '1' if backports_table.effective(item, states) == '1' else '0'
    return env


# Switches that renderer v2 does not take from the player: defaults.env's value applies (an
# ISS_/TLG_ variable set outside the launcher still wins). ISS_DRAW_PIPE: defaults.env sets 0 and
# the draw pipeline's stability in v2 is not established, so an old tlg.ini adv_draw_pipe=1 (from
# this port's renderer) must not turn it on. backports_table.V2_DEFAULTS_WIN_IDS marks the rows.
V2_DEFAULTS_WIN = ('ISS_DRAW_PIPE',)
# A variable with this value is removed from the game's environment after the defaults and the
# launcher's switches are applied (build_env): the way to run without a defaults.env key.
UNSET = '__unset__'
# ISS_TUNING keys the renderer adds up instead of replacing (bridge.cpp: relaxed_site, up to 8).
TUNING_REPEATABLE = ('relaxed_site',)


def merge_tuning(base, override):
    """ISS_TUNING "name=value,..." of base (defaults.env) with override's items applied per key:
    a name already in base takes override's value in place, a new one is appended, and a
    repeatable one (relaxed_site) or an item without '=' is appended unless already there.
    merge_tuning(base, merge_tuning(base, x)) == merge_tuning(base, x)."""
    items = [item for item in base.split(',') if item]
    for item in (i for i in override.split(',') if i):
        name, sep, _ = item.partition('=')
        at = next((n for n, old in enumerate(items) if old.partition('=')[0] == name), None)
        if not sep or name in TUNING_REPEATABLE or at is None:
            if item not in items:
                items.append(item)
        else:
            items[at] = item
    return ','.join(items)

# Game language: PS4 system language ids (sceSystemServiceParamGetInt LANG). Which of them the
# player's dump has comes from its PlayGo chunks (game_languages); each choice passes its own id.
GAME_LANGUAGES = {'en': 1, 'en_GB': 18, 'pt_BR': 17, 'pt_PT': 7, 'es_LA': 20, 'es': 3, 'fr_CA': 22, 'fr': 2,
                  'de': 4, 'it': 5, 'nl': 6, 'pl': 16, 'tr': 19, 'ru': 8, 'cs': 23, 'hu': 24, 'el': 25, 'ro': 26,
                  'fi': 12, 'sv': 13, 'da': 14, 'no': 15, 'ar': 21, 'ja': 0, 'ko': 9, 'zh_TW': 10, 'zh_CN': 11,
                  'th': 27, 'vi': 28, 'id': 29}
# Offered when the game files cannot tell (no playgo-chunk.dat, as in FTP dumps): the CUSA00223 set.
_DEFAULT_LANGUAGES = ('en', 'pt_BR', 'es_LA', 'fr_CA', 'tr', 'ru')
# The closest choice when the system's language is missing from the game files.
_LANGUAGE_SIBLING = {'en_GB': 'en', 'pt_PT': 'pt_BR', 'pt_BR': 'pt_PT', 'es': 'es_LA', 'es_LA': 'es',
                     'fr': 'fr_CA', 'fr_CA': 'fr', 'zh_TW': 'zh_CN', 'zh_CN': 'zh_TW'}
# Locale language (and region) -> choice; a language not listed here is English.
_LANGUAGE_REGION = {('en', 'gb'): 'en_GB', ('en', 'ie'): 'en_GB', ('en', 'au'): 'en_GB', ('en', 'nz'): 'en_GB',
                    ('pt', 'br'): 'pt_BR', ('pt', ''): 'pt_BR', ('es', 'es'): 'es', ('fr', 'ca'): 'fr_CA',
                    ('zh', 'tw'): 'zh_TW', ('zh', 'hk'): 'zh_TW', ('zh', 'mo'): 'zh_TW', ('zh', 'hant'): 'zh_TW'}
_LANGUAGE_PREFIX = {'en': 'en', 'pt': 'pt_BR', 'es': 'es_LA', 'fr': 'fr', 'de': 'de', 'it': 'it', 'nl': 'nl',
                    'pl': 'pl', 'tr': 'tr', 'ru': 'ru', 'cs': 'cs', 'hu': 'hu', 'el': 'el', 'ro': 'ro', 'fi': 'fi',
                    'sv': 'sv', 'da': 'da', 'no': 'no', 'nb': 'no', 'nn': 'no', 'ar': 'ar', 'ja': 'ja', 'ko': 'ko',
                    'zh': 'zh_CN', 'th': 'th', 'vi': 'vi', 'id': 'id', 'in': 'id'}


def system_game_language(environ=None):
    """The game language that follows the system one: LC_ALL, LC_MESSAGES, LANG, then the Python
    locale and, on Windows, the user's default locale name. The region picks the variant (es_ES is
    es, other Spanish is es_LA; pt_PT is pt_PT, pt_BR and a bare pt are pt_BR; fr_CA, en_GB, zh_TW);
    a language the PS4 has no id for is English."""
    environ = os.environ if environ is None else environ
    code = ''
    for name in ('LC_ALL', 'LC_MESSAGES', 'LANG'):
        if environ.get(name):
            code = environ[name]
            break
    if not code:
        try:
            import locale
            code = locale.getlocale()[0] or ''
        except Exception:
            code = ''
    if not code and os.name == 'nt':
        try:
            import ctypes
            buf = ctypes.create_unicode_buffer(85)
            if ctypes.windll.kernel32.GetUserDefaultLocaleName(buf, 85):
                code = buf.value
        except Exception:
            code = ''
    parts = re.split(r'[_\-.@]', code.strip().lower())
    language = parts[0]
    # Windows names: Portuguese_Brazil, Spanish_Spain... (locale.getlocale on Windows)
    windows = {'english': 'en', 'portuguese': 'pt', 'spanish': 'es', 'french': 'fr', 'german': 'de',
               'italian': 'it', 'dutch': 'nl', 'polish': 'pl', 'turkish': 'tr', 'russian': 'ru', 'czech': 'cs',
               'hungarian': 'hu', 'greek': 'el', 'romanian': 'ro', 'finnish': 'fi', 'swedish': 'sv',
               'danish': 'da', 'norwegian': 'no', 'arabic': 'ar', 'japanese': 'ja', 'korean': 'ko',
               'chinese': 'zh', 'thai': 'th', 'vietnamese': 'vi', 'indonesian': 'id'}
    regions = {'brazil': 'br', 'portugal': 'pt', 'spain': 'es', 'canada': 'ca', 'united kingdom': 'gb',
               'taiwan': 'tw', 'hong kong sar': 'hk'}
    language = windows.get(language, language)
    region = regions.get(parts[1], parts[1]) if len(parts) > 1 else ''
    if language == 'pt' and region == 'pt':
        return 'pt_PT'
    if language == 'pt':
        return 'pt_BR'
    if (language, region) in _LANGUAGE_REGION:
        return _LANGUAGE_REGION[(language, region)]
    if language == 'zh' and len(parts) > 2 and parts[1] == 'hant':
        return 'zh_TW'
    return _LANGUAGE_PREFIX.get(language, 'en')


def playgo_languages(path):
    """PS4 language ids the package has its own PlayGo chunks for, from sce_sys/playgo-chunk.dat
    ("plgo"; chunk count at 10, chunk attribute table offset at 192; each 32-byte attribute has a
    64-bit language mask at 16, bit 63 - id). Chunks for every language (mask 0 or all ones) say
    nothing, so a package without language chunks gives an empty set."""
    data = Path(path).read_bytes()
    if data[:4] != b'plgo' or len(data) < 200:
        raise ValueError('not a PlayGo chunk file')
    count = struct.unpack_from('<H', data, 10)[0]
    table = struct.unpack_from('<I', data, 192)[0]
    if table + count * 32 > len(data):
        raise ValueError('PlayGo chunk table out of the file')
    ids = set()
    for i in range(count):
        mask = struct.unpack_from('<Q', data, table + i * 32 + 16)[0]
        if mask & 0xffffffff and not mask >> 32:  # written big-endian: ids 0-31 are the high bits
            mask = int.from_bytes(mask.to_bytes(8, 'little'), 'big')
        if mask not in (0, (1 << 64) - 1):
            ids |= {63 - bit for bit in range(64) if mask >> bit & 1}
    return ids


def game_languages(game_dir):
    """The GAME_LANGUAGES keys the game files have, in that order, from the PlayGo chunk languages
    of <game>/sce_sys/playgo-chunk.dat; the CUSA00223 set when that cannot tell (no file: FTP dumps
    do not expose it; or no language chunk). param.sfo is no help: its TITLE_xx keys are store titles."""
    ids = set()
    if game_dir:
        try:
            ids = _cached(Path(game_dir) / 'sce_sys' / 'playgo-chunk.dat', 'languages', playgo_languages)
        except (OSError, ValueError, struct.error):
            pass
    return [key for key, code in GAME_LANGUAGES.items() if code in ids] or list(_DEFAULT_LANGUAGES)


def game_language_choices(settings):
    """The game language box of both launchers: 'auto', the languages the game files have and the
    saved choice even when they lack it (never changed behind the player's back)."""
    keys = ['auto'] + game_languages(settings.get('game_dir', ''))
    choice = settings.get('game_language', 'auto')
    return keys + [choice] if choice in GAME_LANGUAGES and choice not in keys else keys


def resolve_game_language(settings, environ=None):
    """settings['game_language'] when it is one of GAME_LANGUAGES; 'auto' (or anything unknown) follows
    the system; when the game files lack that language, its closest variant (es/es_LA, pt_PT/pt_BR...),
    else English, else the first language they have."""
    choice = settings.get('game_language', 'auto')
    if choice in GAME_LANGUAGES:
        return choice
    system, present = system_game_language(environ), game_languages(settings.get('game_dir', ''))
    for key in (system, _LANGUAGE_SIBLING.get(system), 'en', 'en_GB'):
        if key in present:
            return key
    return present[0]


# TLG core defaults (the reasons are in each comment; still to be validated on this game).
DEFAULT_ENV = {
    'ISS_PIPELINE_BINARIES': '0',  # VK_KHR_pipeline_binary crashes on the NVIDIA 595 driver
    'ISS_GC_BUDGET_MB': '3500',    # texture cache budget; 5600 exhausts 6 GB of VRAM in inFAMOUS
    'ISS_UPSCALER': 'off',         # native image: GoW3's temporal upscalers made it shake
    'ISS_JITTER': '0',
    'ISS_OBJECT_MOTION': '0',
    'ISS_FRAME_STATS': '1',        # FPS in the log every 5 s (scripts/watch_session.sh heartbeat)
}

# The game's own modules from sce_module/, linked into the image.
MODULES = ['libc.prx', 'libSceFios2.prx']

DEFAULT_SETTINGS = {
    # Source tree: the merged game view (base 1.00 + update 1.07). Package: the player picks it.
    'game_dir': '' if FROZEN else str(REPO_DIR / 'game' / 'CUSA00223'),
    'game_language': 'auto',       # auto (system language) | a key of GAME_LANGUAGES
    'ui_language': 'auto',         # auto | pt_BR | en_US
    'show_fps': False,
    'display_mode': 'Windowed',    # Windowed | Borderless | Fullscreen (tlg.ini)
    'vsync': True,
    'vram_budget_mb': 3500,  # 5600 runs out of VRAM loading the first level (RTX 2060, 6 GB)
    'vram_auto': True,       # renderer v2: ISS_GC_BUDGET_MB=auto, the texture budget follows the card live
    'stick_deadzone': 10,    # percent; runtime_pad.c ISS_STICK_DEADZONE
    'eager_readback': False,  # ISS_EAGER_READBACK: copy GPU results the game reads back at each fence
    'write_merge': True,     # ISS_WRITE_MERGE: game memcpy/memset into GPU memory without a GPU wait
    'deferred_readback': True,  # ISS_DEFERRED_READBACK: the game thread waits for its own GPU reads
    'precompile_shaders': True,  # launcher: make the shader jobs list before the game starts (precompile.py)
    'close_on_play': True,
    'reopen_launcher': True,  # launcher closed on play: opens again when the game exits  # launcher: close about 2 s after the game starts (an open launcher cost ~30% FPS in CPU-bound scenes)
    'gpu_readbacks': '2',    # 2 precise: GPU particles (smoke to drain) need it; 0 fast skips them
}


def config_path():
    base = Path(os.environ.get('APPDATA') or os.environ.get('XDG_CONFIG_HOME') or Path.home() / '.config')
    return base / 'infamous-launcher' / 'settings.json'


# Bumped when a default changes in a way that must reach players who already saved settings.
# rev 2: eager_readback went off by default (it made smoke and glow flicker in precise mode).
SETTINGS_REV = 2


def load_settings():
    settings = dict(DEFAULT_SETTINGS)
    saved = {}
    try:
        saved = json.loads(config_path().read_text(encoding='utf-8'))
        settings.update(saved)
    except (OSError, ValueError):
        pass
    if saved and saved.get('settings_rev', 1) < 2:
        settings['eager_readback'] = False
    settings['settings_rev'] = SETTINGS_REV
    return settings


def save_settings(settings):
    path = config_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(settings, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


# The supported eboot.bin (update 1.07, US): entry point and memory image size from its ELF
# headers, the same whatever tool decrypted it (base 1.00 is 0x933850 / 85,115,231 bytes).
EBOOT_107 = (0x61dfd0, 133_846_143)
# APP_VER of the update's param.sfo; mods (cheat XML) are chosen by it.
APP_VER_107 = '01.07'

# Messages about the player's game folder (en, pt); the launcher shows them in its language,
# the command line prints both. {detail} names the file.
_COPY_UPDATE = ('Copy every file of update 1.07 (CUSA00223-patch: eboot.bin, sce_module/, sce_sys/ and the rest) '
                'over the base game folder, replacing the files; see "Required game files" in the README.',
                'Copie todos os arquivos do update 1.07 (CUSA00223-patch: eboot.bin, sce_module/, sce_sys/ e o resto) '
                'por cima da pasta do jogo base, substituindo os arquivos; veja "Arquivos do jogo necessÃ¡rios" no README.')
TEXT = {
    'old_module': ('{detail} is not the one from update 1.07 (it comes from the base game 1.00 or another '
                   'version) and the game cannot start with it. ' + _COPY_UPDATE[0],
                   'O {detail} nÃ£o Ã© o do update 1.07 (Ã© do jogo base 1.00 ou de outra versÃ£o) e o jogo nÃ£o '
                   'abre com ele. ' + _COPY_UPDATE[1]),
    'other_eboot': ('eboot.bin is not the one from update 1.07 ({detail}); the port is only tested with '
                    'CUSA00223 (US) 1.07. ' + _COPY_UPDATE[0],
                    'O eboot.bin nÃ£o Ã© o do update 1.07 ({detail}); o port sÃ³ Ã© testado com o CUSA00223 (EUA) '
                    '1.07. ' + _COPY_UPDATE[1]),
    'old_param': ('{detail} is not the one from update 1.07: the game starts, but cheats (mods) made for 1.07 '
                  'are not listed. Copy the 1.07 param.sfo (on the PS4: /system_data/priv/appmeta/CUSA00223/param.sfo) '
                  'into the game folder\'s sce_sys/; see "Required game files" in the README.',
                  'O {detail} nÃ£o Ã© o do update 1.07: o jogo abre, mas os cheats (mods) feitos para o 1.07 nÃ£o '
                  'aparecem. Copie o param.sfo do 1.07 (no PS4: /system_data/priv/appmeta/CUSA00223/param.sfo) '
                  'para o sce_sys/ da pasta do jogo; veja "Arquivos do jogo necessÃ¡rios" no README.'),
    'unreadable': ('{detail} could not be read (damaged, incomplete or still encrypted): dump it again from the PS4.',
                   'NÃ£o deu para ler o {detail} (danificado, incompleto ou ainda criptografado): faÃ§a o dump de novo no PS4.'),
}
# Problems that are reported but do not block Play.
WARNINGS = {'other_eboot', 'old_param'}


def message(key, portuguese=None, **fmt):
    """TEXT[key] in one language, or both (pt, then en) when portuguese is None."""
    en, pt = TEXT[key]
    if portuguese is None:
        return f'{pt.format(**fmt)}\n{en.format(**fmt)}'
    return (pt if portuguese else en).format(**fmt)


class GameError(RuntimeError):
    """A problem with the player's game files (TEXT key + detail); str() holds both languages."""

    def __init__(self, key, detail):
        super().__init__(message(key, detail=detail))
        self.key, self.detail = key, detail


_inspected = {}


def _scripts():
    scripts = str(PORT_DIR / 'scripts')
    if scripts not in sys.path:
        sys.path.insert(0, scripts)


def external_tls_relocations(path):
    """How many TLS relocations of this module need another module's thread-local symbol, which
    link_modules.py cannot link (the libc.prx of base 1.00, SDK 1.50, has 4; the 1.07 one has none)."""
    _scripts()
    import link_modules
    return link_modules.external_tls_relocations(link_modules.module(path))


def eboot_layout(path):
    """(entry point, memory image size) from the eboot's ELF headers."""
    _scripts()
    from prepare import parse_self, read_self
    _, header, ph, _, _ = parse_self(read_self(path))
    return header[4], max(p['vaddr'] + p['memsz'] for p in ph if p['type'] in (1, 0x61000010))


def _cached(path, kind, inspect):
    """inspect(path), remembered while the file keeps its size and time (check_game runs on
    every change in the launcher window)."""
    stat = path.stat()
    key = (str(path), kind, stat.st_size, stat.st_mtime_ns)
    if key not in _inspected:
        _inspected[key] = inspect(path)
    return _inspected[key]


def check_game(game_dir):
    """Problems that keep the game from starting, as (key, detail) pairs; empty means ready.
    Keys in WARNINGS are only reported."""
    game = Path(game_dir)
    problems = []
    eboot = game / 'eboot.bin'
    if not eboot.is_file():
        problems.append(('missing', 'eboot.bin'))
    else:
        try:
            entry, size = _cached(eboot, 'layout', eboot_layout)
            if (entry, size) != EBOOT_107:
                problems.append(('other_eboot', f'entry={entry:#x}, image={size:,} bytes'))
            else:  # the base 1.00 param.sfo beside the 1.07 eboot: no 1.07 mods
                version = mod_patches.game_app_version(game)
                if version not in (None, APP_VER_107):
                    problems.append(('old_param', f'sce_sys/param.sfo (APP_VER {version})'))
        except (OSError, ValueError, KeyError, StopIteration) as error:
            problems.append(('unreadable', f'eboot.bin ({error})'))
    for module in MODULES:
        path = game / 'sce_module' / module
        if not path.is_file():
            problems.append(('missing', f'sce_module/{module}'))
            continue
        try:
            if _cached(path, 'tls', external_tls_relocations):
                problems.append(('old_module', f'sce_module/{module}'))
        except (OSError, ValueError, KeyError, StopIteration) as error:
            problems.append(('unreadable', f'sce_module/{module} ({error})'))
    if not PROBE.is_file():
        problems.append(('not_built', str(PROBE)))
    return problems


def blocking(problems):
    return [problem for problem in problems if problem[0] not in WARNINGS]


def update_ini(path, values):
    """Sets keys in tlg.ini (the in-game menu's file), keeping every other line."""
    lines = path.read_text(encoding='utf-8').splitlines() if path.is_file() else ['# tlg settings']
    seen = set()
    for i, line in enumerate(lines):
        key = line.split('=', 1)[0].strip()
        if key in values:
            lines[i] = f'{key}={values[key]}'
            seen.add(key)
    lines += [f'{k}={v}' for k, v in values.items() if k not in seen]
    path.write_text('\n'.join(lines) + '\n', encoding='utf-8')


# The serial the GPU backend names this game's shader cache after (another region's copy: its
# param.sfo TITLE_ID, read from the game folder).
SHADER_CACHE_SERIALS = ('CUSA00223',)


def shader_cache_serials(settings):
    serial = shader_cache.title_id(settings['game_dir']) if settings.get('game_dir') else None
    return sorted({*SHADER_CACHE_SERIALS, *([serial] if serial else [])})


def gpu_user_dir():
    """Where the GPU backend keeps its caches (shaders, pipelines): the saves folder, on every OS.
    ISS_GPU_USER_DIR/TLG_GPU_USER_DIR move it (the more specific name wins)."""
    return Path(os.environ.get('ISS_GPU_USER_DIR') or os.environ.get('TLG_GPU_USER_DIR') or user_dir())


def shader_cache_size(settings):
    """Bytes of compiled shaders this game has on disk (launcher/shader_cache.py has the paths)."""
    return shader_cache.size(shader_cache_serials(settings), gpu_user_dir())


def clear_shader_cache(settings):
    """Deletes this game's compiled shaders: the next start rebuilds them (longer load, some
    stutter at first). Saves, settings and logs stay. Returns (bytes freed, [(path, error)])."""
    return shader_cache.clear(shader_cache_serials(settings), gpu_user_dir())


def python_command(script):
    """How to run one of scripts/*.py: the packaged executable runs them itself (--script)."""
    if FROZEN:
        return [sys.executable, '--script', str(script)]
    return [sys.executable, str(script)]


def attach_stdio():
    """A windowed Windows executable starts without sys.stdout; inherited pipes still exist."""
    if os.name != 'nt':
        return
    import ctypes
    import msvcrt
    for name, std in (('stdout', -11), ('stderr', -12)):
        if getattr(sys, name) is not None:
            continue
        stream = None
        handle = ctypes.windll.kernel32.GetStdHandle(std)
        if handle and handle != ctypes.c_void_p(-1).value:
            try:
                stream = open(msvcrt.open_osfhandle(handle, os.O_WRONLY), 'w', encoding='utf-8',
                              errors='replace', buffering=1)
            except OSError:
                stream = None
        setattr(sys, name, stream or open(os.devnull, 'w'))


NO_WINDOW = getattr(subprocess, 'CREATE_NO_WINDOW', 0)  # no console flashing for the scripts


def run_script(argv):
    """--script <path> [args]: runs a preparation script inside the packaged executable."""
    import runpy
    attach_stdio()
    script = Path(argv[0])
    sys.argv = [str(script), *argv[1:]]
    sys.path.insert(0, str(script.parent))
    try:
        runpy.run_path(str(script), run_name='__main__')
    except SystemExit as stop:
        return stop.code if isinstance(stop.code, int) else (0 if stop.code is None else 1)
    except Exception as error:  # noqa: BLE001  never PyInstaller's "unhandled exception" dialog
        import traceback
        traceback.print_exc(file=sys.stderr)
        print(f'{script.name}: {error}', file=sys.stderr, flush=True)  # last line: the reason
        return 1
    return 0


EXTERNAL_TLS = re.compile(r'([\w.-]+\.s?prx): unsupported external TLS relocation')


def step_error(name, output):
    """The error of a failed preparation step: GameError for a known problem with the game files,
    else RuntimeError with the step's last line (its full output goes to the log)."""
    old = EXTERNAL_TLS.search(output)
    if old:
        return GameError('old_module', f'sce_module/{old.group(1)}')
    lines = [line for line in output.splitlines() if line.strip()]
    return RuntimeError(f'{name}: {lines[-1] if lines else "failed"}')


def prepare(settings, log=print):
    """Builds out/boot-linked.bin and out/content.bin; returns the module list that was linked."""
    game = settings['game_dir']
    scripts = PORT_DIR / 'scripts'
    modules = list(MODULES)
    steps = [
        [*python_command(scripts / 'prepare.py'), game, '--out', str(OUT_DIR)],
        [*python_command(scripts / 'link_modules.py'), game, '--out', str(OUT_DIR), '--modules', *modules],
        [*python_command(scripts / 'content_profile.py'), game, '--out', str(OUT_DIR)],
        [*python_command(scripts / 'infamous_patches.py'), game, '--out', str(OUT_DIR),
         '--mods-dir', str(mods_dir()), '--mods-config', str(mods_config())],
    ]
    for step in steps:
        result = subprocess.run(step, cwd=str(scripts), capture_output=True, text=True,
                                creationflags=NO_WINDOW)
        if result.returncode:
            name = next(Path(arg).name for arg in step if arg.endswith('.py'))
            output = (result.stderr or '') + (result.stdout or '')
            for line in output.splitlines():
                log(line)
            raise step_error(name, output)
        for line in result.stdout.splitlines():
            log(line)
    isa_table(log)
    return modules


ISA_TABLE = OUT_DIR / 'isa-rewrite.bin'


def probe_env():
    """The environment tlg-probe needs to start: on Linux the libraries packaged with the port
    (SDL3, FFmpeg, ...) live in out/lib, and the frozen launcher's own LD_LIBRARY_PATH (PyInstaller's
    _internal) is replaced by the one it had at start."""
    env = os.environ.copy()
    if os.name != 'nt':
        original = env.pop('LD_LIBRARY_PATH_ORIG', None) if FROZEN else env.get('LD_LIBRARY_PATH')
        bundled = OUT_DIR / 'lib'
        paths = [str(bundled)] if bundled.is_dir() else []
        if original:
            paths.append(original)
        if paths:
            env['LD_LIBRARY_PATH'] = os.pathsep.join(paths)
        else:
            env.pop('LD_LIBRARY_PATH', None)
    return env


def cpu_missing_sets(log=print):
    """Instruction sets `tlg-probe --cpu-features` reports missing ([] when complete); None when the
    probe could not tell (it did not start: the table is then built anyway, see isa_table). A CPU
    that lacks a set nothing can rewrite (AVX and older) stops here (exit 20) with the probe's reason."""
    try:
        result = subprocess.run([str(PROBE), '--cpu-features'], stdin=subprocess.DEVNULL, capture_output=True,
                                text=True, timeout=120, creationflags=NO_WINDOW, env=probe_env())
    except subprocess.SubprocessError as error:
        # Windows players with Intel CPUs met this: the first --cpu-features run went past the
        # timeout (the antivirus scanning the new exe), no table was built and the game stopped on
        # the first SSE4A instruction (exception 0xc000001d). Unknown: build the table anyway.
        log(f'CPU check failed ({error.__class__.__name__}); building the instruction rewrite table anyway')
        return None
    except OSError as error:
        if not PROBE.is_file():
            return []  # no probe yet: check_game reports it
        log(f'CPU check failed ({error}); building the instruction rewrite table anyway')
        return None
    if result.returncode == 20:
        raise RuntimeError(result.stdout.strip() or 'this CPU cannot run the game')
    for line in result.stdout.splitlines():
        if line.startswith('CPU: missing '):
            return [name.strip() for name in line[len('CPU: missing '):].split(',') if name.strip()]
        if line.startswith('CPU: complete'):
            return []
    # Linux players with Intel CPUs met this: the probe could not load its libraries, so the CPU
    # looked complete, no table was built and the game stopped ("no rewrite table matches").
    detail = (result.stderr or result.stdout).strip().splitlines()
    log(f'CPU check failed (exit {result.returncode}): {detail[-1] if detail else "no output"}; '
        'building the instruction rewrite table anyway')
    return None


def red_zone_wanted(env=None):
    """Windows red-zone protection (scripts/red_zone.py, iss_pc #29/#41): on by default on Windows,
    ISS_RED_ZONE_REWRITE=0 turns it off; on Linux only ISS_RED_ZONE_REWRITE=1 (tests) turns it on."""
    env = os.environ if env is None else env
    policy = env.get('ISS_RED_ZONE_REWRITE') or env.get('TLG_RED_ZONE_REWRITE') or ''
    return policy != '0' if os.name == 'nt' else policy == '1'


def isa_table(log=print):
    """ps4_core ISA rewrite (upstream gow3_pc #129): on a CPU without MOVBE, BMI1, LZCNT, F16C or
    SSE4A (SSE4A: every Intel CPU; F16C: Sandy Bridge, #51) the probe replaces those instructions in
    the image, from the table scripts/isa_rewrite.py writes (iced-x86). ISS_ISA_REWRITE=1 (every set)
    or a list such as ISS_ISA_REWRITE=sse4a or =f16c builds it on any CPU (testing), =0 never. The same table carries the
    red-zone protection (red_zone_wanted), so on Windows it is always built; the script keeps a
    table that already matches the image. A failed table is reported and the probe then explains
    what is missing."""
    policy = os.environ.get('ISS_ISA_REWRITE') or os.environ.get('TLG_ISA_REWRITE') or ''
    red_zone = red_zone_wanted()
    if not red_zone:
        ISA_TABLE.unlink(missing_ok=True)  # always matches the boot-linked.bin just written
    missing = cpu_missing_sets(log)
    # None (unknown) builds it too: the probe only rewrites the sets this CPU really lacks.
    if not ((policy and policy != '0') or (policy != '0' and (missing is None or missing)) or red_zone):
        return
    scripts = PORT_DIR / 'scripts'
    result = subprocess.run([*python_command(scripts / 'isa_rewrite.py'), str(OUT_DIR / 'boot-linked.bin'),
                             '--out', str(OUT_DIR), *(['--red-zone'] if red_zone else [])], cwd=str(scripts),
                            capture_output=True, text=True, creationflags=NO_WINDOW)
    if result.returncode:
        ISA_TABLE.unlink(missing_ok=True)
    for line in (result.stdout + result.stderr).splitlines():
        log(line)


def window_icon(settings):
    """The game's own icon0.png converted to BMP (SDL's built-in format) in out/; None if absent."""
    source = Path(settings['game_dir']) / 'sce_sys' / 'icon0.png'
    target = OUT_DIR / 'window_icon.bmp'
    try:
        # Stamped with the source's time: a different dump (or a copy keeping old times) redoes it.
        if source.is_file() and (not target.is_file() or target.stat().st_mtime != source.stat().st_mtime):
            from PIL import Image  # optional: without Pillow the window keeps the default icon
            OUT_DIR.mkdir(parents=True, exist_ok=True)
            Image.open(source).convert('RGB').resize((128, 128)).save(target)
            os.utime(target, (source.stat().st_atime, source.stat().st_mtime))
        return target if target.is_file() else None
    except Exception:
        return None


def pad_glyphs():
    """ISS_GLYPHS for the in-game menu (xbox or nintendo button names) from the first controller
    `tlg-probe --gamepad` finds; None for PlayStation pads, none attached or when it fails.
    ISS_GLYPHS in the environment (playstation, xbox, nintendo) skips the question."""
    if os.environ.get('ISS_GLYPHS'):
        return None
    try:
        out = subprocess.run([str(PROBE), '--gamepad'], stdin=subprocess.DEVNULL, capture_output=True,
                             text=True, timeout=10, creationflags=NO_WINDOW, env=probe_env()).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    for line in out.splitlines():
        if line.startswith('Gamepad: type='):
            kind = line.split()[1][len('type='):]
            return 'xbox' if kind.startswith('xbox') else 'nintendo' if kind.startswith(('switch', 'joycon')) else None
    return None


def read_ini(path):
    """The keys of tlg.ini (name -> text); {} when it does not exist."""
    values = {}
    try:
        lines = Path(path).read_text(encoding='utf-8').splitlines()
    except OSError:
        return values
    for line in lines:
        if '=' in line and not line.lstrip().startswith('#'):
            key, value = line.split('=', 1)
            values[key.strip()] = value.strip()
    return values


def experimental_states(settings):
    """The player's choices of the launcher's "Experimental fixes" window (tlg.ini + settings)."""
    return backports_table.read_states(read_ini(user_dir() / 'tlg.ini'), settings)


def save_experimental(settings, states):
    """Writes the choices of the "Experimental fixes" window: tlg.ini keys (the game's own file)
    and the switches kept in settings.json. Shared by the Tk and Qt launchers."""
    user_dir().mkdir(parents=True, exist_ok=True)
    backports_table.write_ini(user_dir() / 'tlg.ini', backports_table.ini_updates(states))
    for item in backports_table.ITEMS:
        if item.store == 'launcher':  # already handled by build_env (settings.json)
            value = backports_table.effective(item, states)
            settings[item.key] = value != '0'
    save_settings(settings)


def build_env(settings, extra=None):
    env = dict(os.environ)
    for key, value in DEFAULT_ENV.items():
        env.setdefault(key, value)  # explicit ISS_* in the environment always win
    # Renderer v2 computes the texture budget itself (what the card leaves after everything else);
    # this port's renderer always takes the number.
    budget = 'auto' if settings.get('vram_auto', True) and renderer_v2(settings) else str(int(settings['vram_budget_mb']))
    env['ISS_GC_BUDGET_MB'] = os.environ.get('ISS_GC_BUDGET_MB', budget)
    env['ISS_READBACKS'] = os.environ.get('ISS_READBACKS', str(settings.get('gpu_readbacks', '2')))
    env['ISS_EAGER_READBACK'] = os.environ.get('ISS_EAGER_READBACK', '1' if settings.get('eager_readback', False) else '0')
    env['ISS_WRITE_MERGE'] = os.environ.get('ISS_WRITE_MERGE', '1' if settings.get('write_merge', True) else '0')
    env['ISS_DEFERRED_READBACK'] = os.environ.get('ISS_DEFERRED_READBACK', '1' if settings.get('deferred_readback', True) else '0')
    env['ISS_STICK_DEADZONE'] = os.environ.get('ISS_STICK_DEADZONE', f"{int(settings.get('stick_deadzone', 10)) / 100:.2f}")
    for name, value in padsettings.environment(settings).items():   # mouse touchpad mode, TLG_KEY_MAP
        env.setdefault(name, value)
    env['ISS_LANGUAGE'] = os.environ.get('ISS_LANGUAGE', str(GAME_LANGUAGES[resolve_game_language(settings)]))
    if settings.get('ui_language', 'auto') != 'auto':
        env['ISS_UI_LANG'] = settings['ui_language']
    env['ISS_CONFIG'] = str(user_dir() / 'tlg.ini')
    dlc = addons.labels(user_dir() / 'dlc')
    if dlc:
        env.setdefault('ISS_ADDCONT_LABELS', ','.join(dlc))
    # Window name, app id (matches infamous-pc.desktop for Alt+Tab and the dock) and icon.
    env.setdefault('ISS_APP_NAME', 'inFAMOUS Second Son')
    env.setdefault('ISS_APP_ID', 'infamous-pc')
    icon = window_icon(settings)
    if icon:
        env.setdefault('ISS_WINDOW_ICON', str(icon))
    glyphs = pad_glyphs()
    if glyphs:
        env.setdefault('ISS_GLYPHS', glyphs)
    restart = f'"{sys.executable}" --play' if FROZEN else f'"{sys.executable}" "{Path(__file__).resolve()}"'
    env.setdefault('ISS_RESTART_COMMAND', restart)
    # Linux package: the libraries built with the port (SDL3, FFmpeg, ...) are in out/lib.
    if os.name != 'nt':
        libraries = probe_env().get('LD_LIBRARY_PATH')
        if libraries:
            env['LD_LIBRARY_PATH'] = libraries
        else:
            env.pop('LD_LIBRARY_PATH', None)
    # Shaders and pipelines beside the saves (the GPU reads it when it starts; before, Linux used
    # shadPS4's ~/.local/share/shadPS4 and Windows <saves>/user).
    env['TLG_GPU_USER_DIR'] = str(gpu_user_dir())
    # Renderer v2: its defaults (gpu_v2/defaults.env) first, so that the switches below that the
    # player changed win over them, except the V2_DEFAULTS_WIN keys. A variable set outside the
    # launcher wins over both, except ISS_TUNING: that one is merged per key with the defaults'
    # (merge_tuning), so a variant's "x=1" keeps scout=1 etc.
    v2 = renderer_v2(settings)
    if v2:
        for key, value in renderer_v2_env(settings).items():
            if key == 'ISS_TUNING' and os.environ.get(key) and os.environ[key] != UNSET:
                env[key] = merge_tuning(value, os.environ[key])
            elif key not in os.environ:
                env[key] = value
    # Experimental fixes (BPnn) and A/B switches the player changed: ISS_<name>=0/1 (the probe
    # copies them to TLG_). Untouched ones add nothing; ISS_/TLG_ set outside the launcher win.
    switches = backports_table.environment(experimental_states(settings), os.environ)
    if v2:
        for key in V2_DEFAULTS_WIN:
            switches.pop(key, None)
    env.update(switches)
    if extra:
        env.update(extra)
    # The Mods checkbox is authoritative, including when an older session script
    # leaves scale variables in the launcher's inherited environment.
    env.update(camera_mod_environment())
    env['ISS_CAMERA_PREVENT_PITCH_SLOWDOWN'] = '1' if camera_pitch_mod_enabled() else '0'
    # ISS_X=__unset__ (bench.ps1 variants, shells): the game runs without ISS_X, even when
    # defaults.env sets it (an empty value would leave the default in place), and without its
    # TLG_X twin unless that one is set outside the launcher.
    for key in [k for k, v in env.items() if v == UNSET]:
        env.pop(key)
        if key.startswith(('ISS_', 'TLG_')):
            twin = ('TLG_' if key.startswith('ISS_') else 'ISS_') + key[4:]
            if twin not in os.environ or os.environ[twin] == UNSET:
                env.pop(twin, None)
    return env


def new_log():
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log = LOG_DIR / f'run-{datetime.datetime.now():%Y%m%d-%H%M%S}.log'
    latest = OUT_DIR / 'last_run.log'
    try:
        if latest.is_symlink() or latest.exists():
            latest.unlink()
        latest.symlink_to(log)
    except OSError:
        pass
    for old in sorted(LOG_DIR.glob('run-*.log'), reverse=True)[KEEP_LOGS:]:
        old.unlink(missing_ok=True)
    return log


def desktop_env():
    """Environment for the programs the launcher opens for the player (browser, file manager).
    The frozen launcher runs with PyInstaller's LD_LIBRARY_PATH (its _internal folder); a browser
    started with it loads the launcher's libraries instead of the system's and dies silently
    (Linux package: the YouTube, Discord and sponsor links did nothing)."""
    env = os.environ.copy()
    if os.name != 'nt' and FROZEN:
        original = env.pop('LD_LIBRARY_PATH_ORIG', None)
        if original:
            env['LD_LIBRARY_PATH'] = original
        else:
            env.pop('LD_LIBRARY_PATH', None)
    return env


def open_external(target):
    """Opens a URL or a local path with the system's handler, detached from the launcher.
    Returns True when a handler was started."""
    target = str(target)
    try:
        if os.name == 'nt':
            os.startfile(target)  # noqa: S606 (the player's own link or folder)
            return True
        opener = 'open' if sys.platform == 'darwin' else 'xdg-open'
        subprocess.Popen([opener, target], env=desktop_env(), stdin=subprocess.DEVNULL,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, start_new_session=True)
        return True
    except OSError:
        try:
            import webbrowser
            return bool(webbrowser.open(target))
        except Exception:
            return False


def detach_options():
    """Popen options that make the game an independent process: closing the launcher (or its
    terminal) does not reach it. Linux: its own session. Windows: its own process group and no
    console at all (stdout/stderr go to the log file, so nothing needs one)."""
    if os.name == 'nt':
        return {'creationflags': subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS}
    return {'start_new_session': True}


def wait_for_exit(pid):
    """Blocks until process pid ends (no window, no Qt: a closed launcher's watcher)."""
    if os.name == 'nt':
        import ctypes
        kernel = ctypes.windll.kernel32
        handle = kernel.OpenProcess(0x00100000, False, int(pid))   # SYNCHRONIZE
        if handle:
            kernel.WaitForSingleObject(handle, 0xFFFFFFFF)
            kernel.CloseHandle(handle)
        return
    while True:
        try:
            os.kill(int(pid), 0)
        except ProcessLookupError:
            return
        except PermissionError:
            pass
        try:   # a zombie (exited, not yet reaped) counts as ended
            if Path(f'/proc/{int(pid)}/stat').read_text().split(') ', 1)[1][:1] == 'Z':
                return
        except (OSError, IndexError):
            pass
        time.sleep(1.0)


def reopen_after(pid):
    """Starts a detached watcher that opens the launcher again once the game (pid) exits."""
    if FROZEN:
        cmd = [sys.executable, '--reopen-after', str(pid)]
    else:
        cmd = [sys.executable, str(Path(__file__).resolve().parent / 'infamous_launcher_qt.py'),
               '--reopen-after', str(pid)]
    try:
        subprocess.Popen(cmd, env=desktop_env(), stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, **detach_options())
        return True
    except OSError:
        return False


def raise_open_file_limit():
    """Linux: lifts the open-file limit to its hard maximum; the game inherits it. A desktop session
    starts programs with a soft limit of 1024 (systemd DefaultLimitNOFILESoft), and renderer v2
    goes past it while it preloads the pipelines: SDL then fails to create the window's buffer
    file ("Too many open files") and the game crashes before its window opens."""
    if os.name == 'nt':
        return
    try:
        import resource
        soft, hard = resource.getrlimit(resource.RLIMIT_NOFILE)
        if hard != resource.RLIM_INFINITY and soft < hard:
            resource.setrlimit(resource.RLIMIT_NOFILE, (hard, hard))
    except (ImportError, OSError, ValueError):
        pass


def launch(settings, seconds=0, extra_env=None):
    """Writes the in-game settings, starts the game; returns (process, log path).
    The game writes stdout and stderr straight into the log file (no pipe, nobody reads it while
    the game runs), and runs detached from the launcher; the caller may follow the file with
    logtail.LogTail and wait() for the exit code."""
    data = user_dir()
    data.mkdir(parents=True, exist_ok=True)
    update_ini(data / 'tlg.ini', {
        'display_mode': str(settings['display_mode']).lower(),  # the game's keys: windowed|borderless|fullscreen
        'vsync': int(bool(settings['vsync'])),
        'show_fps': int(bool(settings['show_fps'])),
    })
    log = new_log()
    note = savemanager.backup_before_play(data)   # saves copied aside before every start (and to the extra folder)
    if note:
        print(note, flush=True)
    probe = V2_PROBE if renderer_v2(settings) else PROBE
    if probe == V2_PROBE:
        print('Renderer v2 (port/gpu_v2)', flush=True)
    cmd = [str(probe), str(OUT_DIR / 'boot-linked.bin'), '--app0', settings['game_dir'],
           '--user', str(data), '--content-profile', str(OUT_DIR / 'content.bin'),
           '--patches', str(OUT_DIR / 'patches.bin'), '--timeout', str(int(seconds))]
    if ISA_TABLE.is_file():
        cmd += ['--isa-rewrite', str(ISA_TABLE)]
    raise_open_file_limit()
    env = build_env(settings, extra_env)
    given = os.environ.get('ISS_TUNING')
    if given and env.get('ISS_TUNING') != given:   # merged per key with defaults.env (build_env)
        print(f'ISS_TUNING={env.get("ISS_TUNING", "")} (defaults.env merged with ISS_TUNING={given})', flush=True)
    with open(log, 'wb') as out:   # the child keeps its own copy of the descriptor
        process = subprocess.Popen(cmd, cwd=str(data), env=env,
                                   stdin=subprocess.DEVNULL, stdout=out, stderr=subprocess.STDOUT,
                                   **detach_options())
    return process, log


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    attach_stdio()
    seconds = int(argv[0]) if argv and argv[0].isdigit() else 0
    settings = load_settings()
    if os.environ.get('ISS_GAME_DIR'):
        settings['game_dir'] = os.environ['ISS_GAME_DIR']
    if argv[:1] == ['--clear-shader-cache']:
        freed, failures = clear_shader_cache(settings)
        print(f'Cache de shaders: {shader_cache.human_size(freed)} liberados', flush=True)
        for path, error in failures:
            print(f'NÃ£o apagado (em uso? feche o jogo): {path}: {error}', file=sys.stderr)
        return 1 if failures else 0
    problems = check_game(settings['game_dir'])
    for kind, detail in problems:
        if kind in TEXT:
            print(message(kind, detail=detail), file=sys.stderr)
        else:
            print(f'{"Compile primeiro / Build first" if kind == "not_built" else "Faltando / Missing"}: {detail}',
                  file=sys.stderr)
    if blocking(problems):
        return 1
    try:
        prepare(settings, log=lambda line: None)
        # Import here: precompile itself imports this module. The direct --play path
        # must use the same first-start job generation as the Qt/Tk interfaces.
        import precompile
        outcome = precompile.prepare_jobs(settings)
        print(f'Precompile jobs: {outcome}', flush=True)
        if outcome == 'cancelled':
            return 1
    except RuntimeError as error:  # GameError included: a message, never a traceback
        print(error, file=sys.stderr)
        return 1
    process, log = launch(settings, seconds)
    print(f'Rodando (log: {log})', flush=True)
    tail = logtail.LogTail(log, max_lines=1)
    try:
        while process.poll() is None:      # echo the log file to the terminal, once a second
            for line in tail.poll():
                sys.stdout.write(line + '\n')
            sys.stdout.flush()
            time.sleep(1)
    except KeyboardInterrupt:              # the game is detached: Ctrl+C in the terminal stops it here
        process.terminate()
    for line in tail.poll(final=True):
        sys.stdout.write(line + '\n')
    sys.stdout.flush()
    return process.wait()


if __name__ == '__main__':
    sys.exit(main())
