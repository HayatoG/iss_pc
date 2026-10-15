# SPDX-License-Identifier: GPL-2.0-or-later
"""Launcher logic that does not need Qt: translations, status chips, news, last save, fix groups.

Everything here is a thin layer over the modules the Tk launcher already uses
(infamous_run, backports_table, shader_cache, tlg_vulkan); it never changes how the game starts.
"""
import json
import locale
import os
import re
import subprocess
import sys
from pathlib import Path
from struct import unpack_from

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent))
import backports_table  # noqa: E402
import infamous_run as tlg_run  # noqa: E402
import savemanager  # noqa: E402
import userdata  # noqa: E402
import shader_cache  # noqa: E402

# ---------------------------------------------------------------------------------------- i18n
LANGS = ('pt_BR', 'en_US')
# Names of the game languages (same as GAME_LANGUAGE_NAMES of the Tk launcher, which needs tkinter to import).
GAME_LANGUAGE_NAMES = {'en': 'English', 'pt_BR': 'Português (Brasil)', 'es_LA': 'Español (Latinoamérica)',
                       'fr_CA': 'Français (Canada)', 'tr': 'Türkçe', 'ru': 'Русский'}


def load_strings(path=HERE / 'i18n' / 'strings.json'):
    return json.loads(Path(path).read_text(encoding='utf-8'))


def tk_text():
    """The Tk launcher's (en, pt) texts, reused when a key exists there. Importing it needs
    tkinter installed but no display; without it only the Qt strings are available."""
    try:
        import infamous_launcher
        return dict(infamous_launcher.TEXT)
    except Exception:  # tkinter missing in a slim Python
        return dict(tlg_run.TEXT)


def system_is_portuguese():
    for name in ('LC_ALL', 'LC_MESSAGES', 'LANG'):
        value = os.environ.get(name)
        if value:
            return value.lower().startswith('pt')
    try:
        return (locale.getlocale()[0] or '').lower().startswith('pt')
    except ValueError:
        return False


def is_portuguese(settings):
    choice = settings.get('ui_language', 'auto')
    return system_is_portuguese() if choice == 'auto' else choice == 'pt_BR'


class Translator:
    """t(key): Qt strings first (i18n/strings.json), then the Tk launcher's TEXT table."""

    def __init__(self, settings, strings=None, tk=None):
        self.settings = settings
        self.strings = load_strings() if strings is None else strings
        self.tk = tk_text() if tk is None else tk

    def pt(self):
        return is_portuguese(self.settings)

    def __call__(self, key, **fmt):
        if key in self.strings:
            entry = self.strings[key]
            text = entry['pt'] if self.pt() else entry['en']
        elif key in self.tk:
            en, pt = self.tk[key]
            text = pt if self.pt() else en
        else:
            return key
        try:
            return text.format(**fmt)
        except (KeyError, IndexError):
            return text

    def pair(self, pair):
        """An (en, pt) tuple from backports_table."""
        return pair[1] if self.pt() else pair[0]


# ------------------------------------------------------------------------------------ fix groups
GROUP_ORDER = ('light', 'perf', 'tex', 'exp')
_GROUP_OF = {
    'BP03': 'light', 'BP04': 'light', 'BP05': 'light', 'BP11': 'light', 'BP12': 'light',
    'TLG_WQM': 'light', 'TLG_LDS_SMALL': 'light', 'TLG_LDS_MULTI': 'light',
    'TLG_LDS_READLANE': 'light', 'TLG_LANE_DIVERGENCE': 'light', 'TLG_OCCLUSION_MODE': 'light',
    'BP01': 'perf', 'BP02': 'perf', 'BP07': 'perf', 'BP08': 'perf',
    'TLG_ASYNC_SHADERS': 'perf', 'TLG_DEFERRED_READBACK': 'perf', 'TLG_STALE_READBACK': 'perf',
    'TLG_BACKGROUND_WARMUP': 'perf',
    'BP09': 'tex', 'BP10': 'tex', 'TLG_BLOCK1D': 'tex', 'TLG_BC_TINY': 'tex', 'TLG_NO_MSAA': 'tex',
    'BP06': 'exp', 'TLG_RFL_ALL': 'exp', 'TLG_PIPELINE_BINARIES': 'exp',
}


def fix_items():
    """BPnn and A/B rows (the in-game menu options are not part of this screen)."""
    return [i for i in backports_table.ITEMS if i.group in ('bp', 'ab')]


def group_of(item):
    """Fixes the table marks risky (off by default, for testing) or broken go to Experimental."""
    if item.risky or item.broken:
        return 'exp'
    return _GROUP_OF.get(item.id, 'exp')


# -------------------------------------------------------------------------------- status chips
def parse_vulkaninfo(text):
    """(GPU name, 'major.minor' Vulkan) of the best device in `vulkaninfo --summary` output.
    Prefers a discrete GPU, then an integrated one, never a software rasteriser first."""
    devices, current = [], {}
    for line in text.splitlines():
        m = re.match(r'\s*(\w+)\s*=\s*(.+?)\s*$', line)
        if not m:
            continue
        key, value = m.groups()
        if key == 'apiVersion':
            current['api'] = value
        elif key == 'deviceType':
            current['type'] = value
        elif key == 'deviceName':
            current['name'] = value
            devices.append(current)
            current = {}
        elif key == 'GPU0' or key.startswith('GPU'):
            current = {}
    if not devices:
        return None
    rank = {'PHYSICAL_DEVICE_TYPE_DISCRETE_GPU': 0, 'PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU': 1}
    devices.sort(key=lambda d: rank.get(d.get('type', ''), 3))
    best = devices[0]
    version = re.match(r'(\d+\.\d+)', best.get('api', ''))
    return best.get('name', '?'), version.group(1) if version else ''


def detect_gpu(timeout=6):
    """(name, vulkan) from vulkaninfo, with the same Vulkan driver selection the game gets
    (tlg_vulkan.configure); None when vulkaninfo is missing or fails."""
    env = dict(os.environ)
    try:
        import tlg_vulkan
        tlg_vulkan.configure(env)
    except Exception:  # no host driver found: the plain loader environment still works
        pass
    try:
        out = subprocess.run([os.environ.get('TLG_VULKANINFO', 'vulkaninfo'), '--summary'],
                             stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=timeout,
                             env=env, creationflags=getattr(subprocess, 'CREATE_NO_WINDOW', 0)).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    return parse_vulkaninfo(out)


_PAD_WORDS = (('8BitDo', '8BitDo'), ('DualSense', 'DualSense'), ('Wireless Controller', 'DualShock 4'),
              ('DualShock', 'DualShock 4'), ('Xbox', 'Xbox'), ('XInput', 'Xbox'), ('Microsoft', 'Xbox'),
              ('Pro Controller', 'Switch Pro'), ('Switch', 'Switch Pro'), ('Nintendo', 'Switch Pro'),
              ('Controller', None), ('Gamepad', None))


def _pad_from_name(name):
    low = name.lower()
    if 'touchpad' in low or 'motion sensors' in low or 'keyboard' in low or 'mouse' in low:
        return None
    for word, label in _PAD_WORDS:
        if word.lower() in low:
            return label or name
    return None


def _probe_pad_name():
    """Controller name from `tlg-probe --gamepad` (SDL3, works on Windows too); None when it is
    missing, fails or finds nothing."""
    try:
        out = subprocess.run([str(tlg_run.PROBE), '--gamepad'], stdin=subprocess.DEVNULL, capture_output=True,
                             text=True, timeout=10, creationflags=tlg_run.NO_WINDOW,
                             env=tlg_run.probe_env()).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    for line in out.splitlines():
        if line.startswith('Gamepad:') and 'name=' in line:
            return line.split('name=', 1)[1].strip() or None
    return None


def detect_pad(devices_text=None):
    """Name of a connected game controller. With devices_text (the format of Linux's
    /proc/bus/input/devices) only that text is read; otherwise the game's probe is asked first
    (the only way on Windows) and Linux's /proc is the fallback. None if none is found."""
    if devices_text is None:
        name = _probe_pad_name()
        if name:
            return _pad_from_name(name) or name
        try:
            devices_text = Path('/proc/bus/input/devices').read_text(errors='replace')
        except OSError:
            return None
    for line in devices_text.splitlines():
        if line.startswith('N: Name='):
            found = _pad_from_name(line.split('=', 1)[1].strip().strip('"'))
            if found:
                return found
    return None


def game_problem_text(t, problems):
    key, detail = problems[0]
    if key == 'missing':
        return f"{t('missing')}: {detail}"
    return t(key, detail=detail) if key in tlg_run.TEXT else detail


def game_state(settings, problems=None):
    """('none'|'ok'|'bad', problems without 'not_built', port_built)."""
    folder = settings.get('game_dir') or ''
    problems = tlg_run.check_game(folder) if problems is None else problems
    port_missing = any(k == 'not_built' for k, _ in problems)
    game_problems = [p for p in problems if p[0] != 'not_built']
    if not folder:
        return 'none', game_problems, not port_missing
    return ('bad' if tlg_run.blocking(game_problems) else 'ok'), game_problems, not port_missing


def compute_chips(settings, t, gpu='checking', pad=None, problems=None, states=None, cache_bytes=None):
    """The state chips of the Home screen: [{'text', 'level': ok|warn|bad}] and whether all is ready.
    gpu: 'checking' | None | (name, vulkan); pad: name or None."""
    chips = []
    if gpu == 'checking':
        chips.append((t('chip_gpu_checking'), 'warn'))
    elif gpu is None:
        chips.append((t('chip_gpu_unknown'), 'warn'))
    else:
        name, vk = gpu
        chips.append((t('chip_gpu', gpu=name, vk=vk) if vk else t('chip_gpu_novk', gpu=name), 'ok'))
    state, game_problems, built = game_state(settings, problems)
    if state == 'none':
        chips.append((t('chip_game_none'), 'bad'))
    elif state == 'bad':
        chips.append((t('chip_game_bad', problem=game_problem_text(t, tlg_run.blocking(game_problems))), 'bad'))
    else:
        chips.append((t('chip_game_ok', version='1.07'), 'ok'))
    if not built:
        chips.append((t('chip_port_missing'), 'bad'))
    states = tlg_run.experimental_states(settings) if states is None else states
    n = backports_table.changes(states)
    chips.append((t('chip_profile_rec') if n == 0 else t('chip_profile_custom', n=n), 'ok' if n == 0 else 'warn'))
    if cache_bytes is None:
        try:
            cache_bytes = tlg_run.shader_cache_size(settings)
        except Exception:
            cache_bytes = 0
    if cache_bytes:
        chips.append((t('chip_cache', size=shader_cache.human_size(cache_bytes)), 'warn' if cache_bytes > 2 ** 30 else 'ok'))
    else:
        chips.append((t('chip_cache_empty'), 'ok'))
    chips.append((t('chip_pad', name=pad) if pad else t('chip_pad_none'), 'ok' if pad else 'off'))
    ready = state == 'ok' and built
    return [{'text': text, 'level': level} for text, level in chips], ready


# ---------------------------------------------------------------------------------------- news
FALLBACK_NEWS = ('news_fallback_1', 'news_fallback_2', 'news_fallback_3')


def _plain(text):
    text = re.sub(r'\[([^\]]+)\]\([^)]*\)', r'\1', text)
    text = re.sub(r'[*`]', '', text)
    return text.strip()


def parse_news(readme_text, portuguese, count=3):
    """The first `count` bullets of README's "Novidades" / "What's new", cut to their bold lead."""
    heading = '## Novidades' if portuguese else "## What's new"
    lines, inside = [], False
    for line in readme_text.splitlines():
        if line.startswith('## '):
            if inside:
                break
            inside = line.strip() == heading
        elif inside and line.startswith('- '):
            lead = re.match(r'- \*\*(.+?)\*\*', line)
            text = _plain(lead.group(1)) if lead else _plain(line[2:])
            text = re.sub(r'\s*\(\s*\)', '', re.sub(r',?\s*#\d+', '', text))  # issue numbers
            text = text.rstrip(':. ')
            if len(text) > 70:
                text = text[:67].rsplit(' ', 1)[0] + '…'
            lines.append(text)
            if len(lines) == count:
                break
    return lines


def news(t, portuguese, readme=None):
    readme = readme or tlg_run.REPO_DIR / 'README.md'
    try:
        found = parse_news(Path(readme).read_text(encoding='utf-8'), portuguese)
    except OSError:
        found = []
    return found or [t(key) for key in FALLBACK_NEWS]


def release_version():
    """'v2026.10.15' from the nearest git tag or a VERSION file next to the port; '' if unknown."""
    for path in (tlg_run.PORT_DIR / 'VERSION', tlg_run.REPO_DIR / 'VERSION'):
        try:
            return path.read_text(encoding='utf-8').strip().split('-')[0]
        except OSError:
            pass
    try:
        out = subprocess.run(['git', '-C', str(tlg_run.REPO_DIR), 'describe', '--tags', '--abbrev=0'],
                             capture_output=True, text=True, timeout=3).stdout.strip()
        return out.split('-')[0]
    except (OSError, subprocess.SubprocessError):
        return ''


# ---------------------------------------------------------------------------------- last save
read_sfo = savemanager.read_sfo   # PSF reader shared with the save manager


def last_save_title(savedata_dir, limit=200):
    """'Welcome to Seattle — 8%' from the newest param.bin/param.sfo under the saves folder
    (SUBTITLE and DETAIL, else TITLE); None when there is no save or it cannot be read."""
    newest, seen = None, 0
    try:
        for path in Path(savedata_dir).rglob('*'):
            seen += 1
            if seen > limit * 20:
                break
            if path.name.lower() in ('param.bin', 'param.sfo') and path.is_file():
                if newest is None or path.stat().st_mtime > newest.stat().st_mtime:
                    newest = path
    except OSError:
        return None
    if newest is None:
        return None
    return savemanager.display_title(savemanager.read_param(newest.read_bytes())) or None


# -------------------------------------------------------------------------- controller guides
GUIDE_DIR = HERE / 'assets' / 'controller_guides'
GUIDE_KINDS = (('xbox', 'Xbox'), ('switch', 'Nintendo Switch Pro'), ('8bitdo', '8BitDo'))


def guide_kind(pad_name):
    """Which guide fits a detected controller name ('xbox' when unknown)."""
    low = (pad_name or '').lower()
    if '8bitdo' in low:
        return '8bitdo'
    if 'switch' in low or 'nintendo' in low or 'pro controller' in low:
        return 'switch'
    return 'xbox'


def guide_path(kind, portuguese, folder=None):
    """The guide image of a controller in the launcher's language; None when the file is absent
    (the screen then says the images are not included; they can be swapped for other files)."""
    path = Path(folder or GUIDE_DIR) / f"inFAMOUS-controle-{kind}-{'pt-BR' if portuguese else 'en-US'}.png"
    return path if path.is_file() else None
