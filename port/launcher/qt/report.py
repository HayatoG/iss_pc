# SPDX-License-Identifier: GPL-2.0-or-later
"""Support report, no Qt: what the "Report a problem" wizard collects, summarises and redacts.

Everything stays on this PC. Nothing here opens a socket; the only outputs are text (copied to the
clipboard), a .zip the player saves, and a URL the player's browser opens when the player clicks.
"""
import getpass
import json
import os
import platform
import re
import socket
import subprocess
import sys
import zipfile
from pathlib import Path
from urllib.parse import quote

HERE = Path(__file__).resolve().parent
ISSUES_URL = 'https://github.com/HayatoG/iss_pc/issues/new'
URLS = {'discord': 'https://discord.gg/UMkdMYZh9g', 'youtube': 'https://www.youtube.com/@hayatog-dev',
        'github': 'https://github.com/HayatoG/iss_pc'}
BODY_LIMIT = 6000        # characters of the issue body that may travel in the URL
URL_LIMIT = 7500         # the encoded URL (browsers and GitHub refuse much longer ones)
LOG_READ_LIMIT = 32 * 1024 * 1024


# ------------------------------------------------------------------------------------ redaction
# "Users" and "home" parents: the next path component is a person's name. A name may hold one
# space ("Ana Silva") only when a separator follows, so a sentence such as "C:\Users\ana wrote" is
# not swallowed.
_SEP = r'[\\/]+'
_NAME = r'(?:[^\\/\s"\'<>|:*?]+(?: [^\\/\s"\'<>|:*?]+)?(?=[\\/])|[^\\/\s"\'<>|:*?]+)'
_PARENTS = r'(?:Users|home|var[\\/]+home|run[\\/]+media|media)'
_HOME_RE = re.compile(r'(?P<head>(?<![\w.-])' + _PARENTS + _SEP + r')(?P<name>' + _NAME + r')', re.IGNORECASE)
KEEP_NAMES = {'public', 'default', 'all users', 'default user', 'shared', '<user>'}
_GENERIC = {'user', 'users', 'root', 'admin', 'home', 'game', 'games', 'port', 'out', 'logs', 'player', 'guest',
            'steam', 'deck', 'pc', 'main', 'data', 'temp', 'tmp', 'user1', 'local', 'bin', 'lib', 'usr', 'etc'}


def own_names(extra=()):
    """The names that identify this player on this machine: login, home folder, host name."""
    names = set(extra)
    for getter in (getpass.getuser, lambda: os.environ.get('USERNAME', ''), lambda: os.environ.get('USER', ''),
                   lambda: Path.home().name, socket.gethostname):
        try:
            value = (getter() or '').strip()
        except Exception:  # no passwd entry in a container, etc.
            value = ''
        if value:
            names.add(value)
    return {n for n in names if len(n) >= 3 and n.lower() not in _GENERIC}


def redact(text, names=()):
    """(redacted text, replacements). The user name of Windows/macOS/Linux paths becomes <user>
    (C:\\Users\\ana\\x -> C:\\Users\\<user>\\x, /home/ana/x -> /home/<user>/x, also /Users, /var/home,
    /run/media and /media) and so does any known name of this player wherever it still appears."""
    count = 0

    def path(match):
        nonlocal count
        if match.group('name').lower() in KEEP_NAMES:
            return match.group(0)
        count += 1
        return match.group('head') + '<user>'

    text = _HOME_RE.sub(path, text)
    for name in sorted(names, key=len, reverse=True):
        if len(name) < 3 or name.lower() in _GENERIC:
            continue
        text, n = re.subn(r'(?<![\w])' + re.escape(name) + r'(?![\w])', '<user>', text, flags=re.IGNORECASE)
        count += n
    return text, count


def leftover_home_paths(text, names=()):
    """What a redacted text still leaks: home paths whose name is not <user>, or a known name."""
    left = [m.group(0) for m in _HOME_RE.finditer(text) if m.group('name').lower() not in KEEP_NAMES]
    for name in names:
        if len(name) >= 3 and name.lower() not in _GENERIC and re.search(
                r'(?<![\w])' + re.escape(name) + r'(?![\w])', text, re.IGNORECASE):
            left.append(name)
    return left


# ------------------------------------------------------------------------------------ log files
def list_logs(log_dir):
    """Log files, newest first: run-*.log (what the launcher writes), else any *.log."""
    folder = Path(log_dir)
    try:
        files = list(folder.glob('run-*.log')) or list(folder.glob('*.log'))
        files = [f for f in files if f.is_file()]
        return sorted(files, key=lambda f: (f.stat().st_mtime, f.name), reverse=True)
    except OSError:
        return []


def read_log(path, limit=LOG_READ_LIMIT):
    """(text, truncated). A huge log keeps its beginning (Build:/Config:) and its end."""
    size = Path(path).stat().st_size
    with open(path, 'rb') as handle:
        if size <= limit:
            data, cut = handle.read(), False
        else:
            head = handle.read(limit // 8)
            handle.seek(size - (limit - limit // 8))
            data, cut = head + b'\n[... log cut by the launcher ...]\n' + handle.read(), True
    return data.decode('utf-8', 'replace'), cut


_FPS_RE = re.compile(r'Frame stats:\s*([0-9.]+)\s*FPS(?:,\s*worst frame\s*([0-9.]+)\s*ms)?')
_EVENT_RE = re.compile(r'^\s*(Fault|STOP|FREEZE|ERROR):\s*(.*)$')
# Runtime lines that explain a STOP, e.g. "Runtime: cannot create 6080 MiB of guest memory (error 1450)"
_RUNTIME_FAIL_RE = re.compile(r'^\s*(Runtime):\s*(cannot .*)$')
_EVENT_PREFIXES = ('STOP:', 'Fault:', 'FREEZE:', 'ERROR:', 'Runtime: cannot ')
_HEADER_KEYS = ('Build:', 'Settings:', 'Window:', 'GPU:', 'GPU 0:', 'VideoOut:', 'CPU:')


def summarize_log(text):
    """What a log says at a glance: the Build:/Config: header lines, Fault:/STOP:/FREEZE:/ERROR: and
    "Runtime: cannot ..." events and the FPS of the "Frame stats" lines."""
    lines = text.splitlines()
    summary = {'build': '', 'config': [], 'header': [], 'events': [], 'event_counts': {}, 'fps_avg': None,
               'fps_min': None, 'fps_max': None, 'worst_ms': None, 'fps_samples': 0, 'lines': len(lines)}
    # Config: block = the line and the indented lines after it
    n = 0
    while n < len(lines):
        line = lines[n]
        if line.startswith('Build:') and not summary['build']:
            summary['build'] = line.strip()
        if line.startswith('Config:') and not summary['config']:
            block = [line.strip()]
            n += 1
            while n < len(lines) and lines[n][:1] in (' ', '\t') and lines[n].strip() and len(block) < 40:
                block.append(lines[n].strip())
                n += 1
            summary['config'] = block
            continue
        n += 1
    if not summary['config']:   # no Config: block in this build: the first lines that describe the setup
        summary['header'] = [l.strip() for l in lines[:120] if l.startswith(_HEADER_KEYS)][:12]
    seen = {}
    fps, worst = [], []
    for line in lines:
        event = _EVENT_RE.match(line) or _RUNTIME_FAIL_RE.match(line)
        if event:
            kind = event.group(1)
            key = (kind, event.group(2)[:160])
            summary['event_counts'][kind] = summary['event_counts'].get(kind, 0) + 1
            seen[key] = seen.get(key, 0) + 1
            if seen[key] == 1 and len(summary['events']) < 12:
                summary['events'].append(key)
            continue
        m = _FPS_RE.search(line)
        if m:
            fps.append(float(m.group(1)))
            if m.group(2):
                worst.append(float(m.group(2)))
    summary['event_repeats'] = {f'{k}: {v}': c for (k, v), c in seen.items() if c > 1}
    summary['events'] = [(k, v, seen[(k, v)]) for k, v in summary['events']]
    if fps:
        summary['fps_samples'] = len(fps)
        summary['fps_avg'] = sum(fps) / len(fps)
        summary['fps_min'], summary['fps_max'] = min(fps), max(fps)
    if worst:
        summary['worst_ms'] = max(worst)
    return summary


def summary_lines(summary, t):
    """The summary as readable lines; t(key, **fmt) is the launcher's translator."""
    out = []
    if summary['build']:
        out.append(summary['build'])
    for line in (summary['config'] or summary['header']):
        out.append(('  ' if summary['config'] and not line.startswith('Config:') else '') + line)
    if summary['fps_avg'] is not None:
        out.append(t('rep_fps', avg=f"{summary['fps_avg']:.1f}", lo=f"{summary['fps_min']:.1f}",
                     hi=f"{summary['fps_max']:.1f}", n=summary['fps_samples']))
        if summary['worst_ms'] is not None:
            out.append(t('rep_worst', ms=f"{summary['worst_ms']:.0f}"))
    else:
        out.append(t('rep_no_fps'))
    if summary['events']:
        for kind, value, count in summary['events']:
            out.append(f'{kind}: {value}' + (f'  (x{count})' if count > 1 else ''))
    else:
        out.append(t('rep_no_events'))
    return out


# -------------------------------------------------------------------------------- the hardware
def _read(path):
    try:
        return Path(path).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return ''


def os_text():
    if sys.platform.startswith('linux'):
        pretty = re.search(r'^PRETTY_NAME="?([^"\n]+)"?', _read('/etc/os-release'), re.M)
        return f"{pretty.group(1) if pretty else 'Linux'} (kernel {platform.release()})"
    if sys.platform == 'win32':
        return f'Windows {platform.release()} ({platform.version()})'
    return f'{platform.system()} {platform.release()}'


def parse_cpuinfo(text):
    """(name, set of flags) from /proc/cpuinfo."""
    name = re.search(r'^model name\s*:\s*(.+)$', text, re.M)
    flags = re.search(r'^flags\s*:\s*(.+)$', text, re.M)
    return (name.group(1).strip() if name else ''), set(flags.group(1).split()) if flags else set()


CPU_FEATURES = (('SSE4A', 'sse4a'), ('AVX2', 'avx2'), ('F16C', 'f16c'), ('MOVBE', 'movbe'), ('BMI1', 'bmi1'),
                ('LZCNT', 'abm'))


def cpu_info(missing_probe=None):
    """{'name', 'features': {label: True/False/None}}. Linux reads /proc/cpuinfo. Elsewhere the
    port's own probe (tlg-probe --cpu-features, via missing_probe) lists what the CPU lacks; AVX2 is
    asked of the OS. None means unknown."""
    features = {label: None for label, _ in CPU_FEATURES}
    name = platform.processor()
    if sys.platform.startswith('linux'):
        found, flags = parse_cpuinfo(_read('/proc/cpuinfo'))
        name = found or name
        if flags:
            for label, flag in CPU_FEATURES:
                features[label] = flag in flags or (flag == 'abm' and 'lzcnt' in flags)
    else:
        if sys.platform == 'win32':
            try:
                import winreg
                with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, r'HARDWARE\DESCRIPTION\System\CentralProcessor\0') as key:
                    name = winreg.QueryValueEx(key, 'ProcessorNameString')[0].strip()
                import ctypes
                features['AVX2'] = bool(ctypes.windll.kernel32.IsProcessorFeaturePresent(40))
            except Exception:  # a locked-down registry or no ctypes: the name stays generic
                pass
        missing = None
        try:
            missing = missing_probe() if missing_probe else None
        except Exception:
            missing = None
        if missing is not None:
            low = {m.lower() for m in missing}
            for label, flag in CPU_FEATURES:
                if label != 'AVX2':
                    features[label] = flag not in low and label.lower() not in low
    return {'name': name or '?', 'features': features}


def ram_bytes():
    if sys.platform.startswith('linux'):
        m = re.search(r'^MemTotal:\s*(\d+) kB', _read('/proc/meminfo'), re.M)
        return int(m.group(1)) * 1024 if m else None
    if sys.platform == 'win32':
        try:
            import ctypes

            class Status(ctypes.Structure):
                _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong),
                            ('avail', ctypes.c_ulonglong), ('ptotal', ctypes.c_ulonglong),
                            ('pavail', ctypes.c_ulonglong), ('vtotal', ctypes.c_ulonglong),
                            ('vavail', ctypes.c_ulonglong), ('ext', ctypes.c_ulonglong)]
            status = Status()
            status.length = ctypes.sizeof(Status)
            ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status))
            return int(status.total)
        except Exception:
            return None
    return None


def parse_vulkan_devices(summary_text, full_text=''):
    """Devices of `vulkaninfo --summary` (name, type, vendor, driver name/info, Vulkan version); with
    the full output also the biggest device-local heap (VRAM) of each, in GiB."""
    devices, current = [], {}
    for line in summary_text.splitlines():
        m = re.match(r'\s*(\w+)\s*=\s*(.+?)\s*$', line)
        if line.startswith('GPU') and line.rstrip().endswith(':'):
            current = {}
            devices.append(current)
        elif m and current is not None:
            current[m.group(1)] = m.group(2)
    devices = [d for d in devices if d.get('deviceName')]
    heaps = {}
    for block in re.split(r'^(?=GPU\d+:)', full_text, flags=re.M):
        if not re.match(r'GPU\d+:', block):
            continue
        name = re.search(r'deviceName\s*=\s*(.+)', block)
        best, size = 0, 0
        for line in block.splitlines():
            if re.match(r'\s*memoryHeaps\[\d+\]:', line):
                size = 0
            elif size == 0 and (m := re.match(r'\s*size\s*=\s*(\d+)', line)):
                size = int(m.group(1))
            elif 'MEMORY_HEAP_DEVICE_LOCAL_BIT' in line:
                best = max(best, size)
        if name and best:
            heaps[name.group(1).strip()] = best / 2 ** 30
    for d in devices:
        d['vram_gb'] = heaps.get(d['deviceName'])
    rank = {'PHYSICAL_DEVICE_TYPE_DISCRETE_GPU': 0, 'PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU': 1}
    devices.sort(key=lambda d: rank.get(d.get('deviceType', ''), 3))
    return devices


def gpu_info(run=subprocess.run, env=None):
    """The best Vulkan device as {'name','driver','vulkan','vendor','type','vram_gb'}; None when
    vulkaninfo is missing. The environment is the game's (tlg_vulkan.configure), like the Home chips."""
    env = dict(os.environ if env is None else env)
    try:
        import tlg_vulkan
        tlg_vulkan.configure(env)
    except Exception:
        pass
    exe = os.environ.get('TLG_VULKANINFO', 'vulkaninfo')
    flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
    try:
        summary = run([exe, '--summary'], stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=8, env=env,
                      creationflags=flags).stdout
    except (OSError, subprocess.SubprocessError):
        return None
    full, smi_gb = '', None
    first = parse_vulkan_devices(summary, '')
    if first and first[0].get('vendorID', '').lower() == '0x10de':
        # NVIDIA: nvidia-smi answers in about 200 ms. The plain `vulkaninfo` can sit for a minute
        # on some systems, so it is not even started when the card already told its memory.
        try:
            try:
                from . import gfxlogic
            except ImportError:   # imported as a plain module (tests, tools)
                import gfxlogic
            smi = run(['nvidia-smi', '--query-gpu=memory.total', '--format=csv,noheader,nounits'],
                      stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=5, creationflags=flags).stdout
            smi_gb = (gfxlogic.parse_nvidia_smi(smi) or 0) / 1024 or None
        except (ImportError, OSError, subprocess.SubprocessError):
            smi_gb = None
    if smi_gb is None:
        try:   # the heaps are only in the long output; without it VRAM stays unknown
            full = run([exe], stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=25, env=env,
                       creationflags=flags).stdout
        except (OSError, subprocess.SubprocessError):
            full = ''
    devices = parse_vulkan_devices(summary, full)
    if smi_gb is not None and devices:
        devices[0]['vram_gb'] = smi_gb
    if not devices:
        return None
    best = devices[0]
    driver = ' '.join(x for x in (best.get('driverName'), best.get('driverInfo') or best.get('driverVersion')) if x)
    vendor = {'0x10de': 'NVIDIA', '0x1002': 'AMD', '0x8086': 'Intel'}.get(best.get('vendorID', '').lower(), '')
    api = re.match(r'(\d+\.\d+)', best.get('apiVersion', ''))
    return {'name': best['deviceName'], 'driver': driver, 'vulkan': api.group(1) if api else '', 'vendor': vendor,
            'type': best.get('deviceType', '').replace('PHYSICAL_DEVICE_TYPE_', '').lower(),
            'vram_gb': best.get('vram_gb')}


def sfo_game(game_dir, read_sfo):
    """(title id, app version) of <game>/sce_sys/param.sfo; ('', '') without it."""
    try:
        values = read_sfo((Path(game_dir) / 'sce_sys' / 'param.sfo').read_bytes())
    except OSError:
        return '', ''
    return str(values.get('TITLE_ID', '')), str(values.get('APP_VER', ''))


def current_version(port_dirs, log_dir):
    """The release name from the VERSION file the package carries (vYYYY.MM.DD-<commit>); else the
    commit of the last log's "Build:" line; '' when neither exists."""
    for folder in port_dirs:
        text = _read(Path(folder) / 'VERSION').strip()
        if text:
            return text.splitlines()[0].strip()
    for log in list_logs(log_dir)[:5]:
        head, _ = read_log(log, 256 * 1024)
        m = re.search(r'^Build:\s*([0-9a-f]{7,40})', head, re.M)
        if m:
            return m.group(1)
    return ''


def gather(port_dirs, log_dir, game_dir, read_sfo, probes=None):
    """All of step 1 as one dict. `probes` replaces the slow system calls (tests, screenshots)."""
    probes = probes or {}
    pick = lambda name, default: probes[name]() if name in probes else default()   # noqa: E731
    title, app_ver = pick('game', lambda: sfo_game(game_dir, read_sfo))
    return {
        'version': pick('version', lambda: current_version(port_dirs, log_dir)),
        'game': {'title_id': title, 'app_ver': app_ver},
        'os': pick('os', os_text),
        'cpu': pick('cpu', cpu_info),
        'ram': pick('ram', ram_bytes),
        'gpu': pick('gpu', gpu_info),
    }


def hardware_rows(info, t):
    """(label, value) pairs for the screen and the report."""
    na = t('rep_unknown')
    features = ', '.join(f'{label} {t("rep_yes") if ok else t("rep_no") if ok is False else "?"}'
                         for label, ok in info['cpu']['features'].items() if label in ('SSE4A', 'AVX2', 'F16C'))
    gpu = info['gpu']
    game = info['game']
    ram = info['ram']
    rows = [
        (t('rep_row_version'), info['version'] or na),
        (t('rep_row_game'), (f"{game['title_id']} {game['app_ver']}".strip()) or na),
        (t('rep_row_os'), info['os'] or na),
        (t('rep_row_cpu'), f"{info['cpu']['name']} ({features})"),
        (t('rep_row_ram'), f'{ram / 2 ** 30:.1f} GiB' if ram else na),
        (t('rep_row_gpu'), gpu['name'] + (f" ({gpu['vram_gb']:.1f} GiB VRAM)" if gpu and gpu.get('vram_gb') else '')
         if gpu else t('rep_gpu_none')),
        (t('rep_row_driver'), (gpu['driver'] or na) + (f" · Vulkan {gpu['vulkan']}" if gpu and gpu['vulkan'] else '')
         if gpu else na),
    ]
    return rows


# --------------------------------------------------------------------------------- known issues
def load_known_issues(path=HERE / 'data' / 'known_issues.json'):
    try:
        return json.loads(Path(path).read_text(encoding='utf-8')).get('issues', [])
    except (OSError, ValueError):
        return []


def _os_key():
    return 'windows' if sys.platform == 'win32' else 'linux' if sys.platform.startswith('linux') else sys.platform


def matches(issue, gpu, os_key=None):
    """Does a known issue apply to this GPU/OS? Every condition the entry names must hold; an entry
    names at least one. Unknown values (no GPU, unknown VRAM) never match a condition on them."""
    m = issue.get('match', {})
    if not m:
        return False
    os_key = os_key or _os_key()
    if 'os' in m and os_key not in m['os']:
        return False
    needs_gpu = any(k in m for k in ('vendor', 'gpu', 'driver', 'vram_gb_max', 'vram_gb_min'))
    if needs_gpu and not gpu:
        return False
    if 'vendor' in m and (gpu.get('vendor') or '').lower() not in [v.lower() for v in m['vendor']]:
        return False
    if 'gpu' in m and not any(re.search(p, gpu.get('name', ''), re.IGNORECASE) for p in m['gpu']):
        return False
    if 'driver' in m and not re.search(m['driver'], gpu.get('driver', ''), re.IGNORECASE):
        return False
    if 'vram_gb_max' in m and not (gpu.get('vram_gb') and gpu['vram_gb'] <= m['vram_gb_max']
                                   and gpu.get('type', 'discrete_gpu') == 'discrete_gpu'):
        return False
    if 'vram_gb_min' in m and not (gpu.get('vram_gb') and gpu['vram_gb'] >= m['vram_gb_min']):
        return False
    return True


def known_for(issues, gpu, os_key=None):
    """Issues with a 'matched' flag, the ones that match this GPU first (stable otherwise)."""
    rows = [dict(i, matched=matches(i, gpu, os_key)) for i in issues]
    return sorted(rows, key=lambda r: not r['matched'])


# --------------------------------------------------------------------------------- the report
def markdown_report(rows, summary_text, log_name, known, description='', t=None, redacted_count=0):
    """The text the player copies or puts in the issue: description, hardware block, log summary."""
    parts = []
    if description.strip():
        parts += [description.strip(), '']
    parts += [f'### {t("rep_h_hw")}', '']
    parts += [f'- **{label}:** {value}' for label, value in rows]
    parts += ['', f'### {t("rep_h_log")}' + (f' (`{log_name}`)' if log_name else ''), '']
    parts += ['```', *summary_text, '```'] if summary_text else [t('rep_no_log')]
    hits = [k for k in known if k.get('matched')]
    if hits:
        parts += ['', f'### {t("rep_h_known")}', '']
        parts += [f'- {k["title"]}' + (' (' + ', '.join(f'#{n}' for n in k['issues']) + ')' if k.get('issues') else '')
                  for k in hits]
    parts += ['', '_' + t('rep_footer') + '_']
    return '\n'.join(parts)


def short_report(rows, summary_text, t):
    """Only the hardware block and the headline of the log: what fits in a URL."""
    keep = [l for l in summary_text if l.startswith(('Build:',) + _EVENT_PREFIXES) or 'FPS' in l][:8]
    parts = [f'### {t("rep_h_hw")}', ''] + [f'- **{a}:** {b}' for a, b in rows]
    if keep:
        parts += ['', f'### {t("rep_h_log")}', '', '```', *keep, '```']
    parts += ['', t('rep_attach_zip')]
    return '\n'.join(parts)


def issue_url(title, body, short_body, base=ISSUES_URL):
    """(url, used_short). The full body goes in the query string when it is at most BODY_LIMIT
    characters and the encoded URL stays under URL_LIMIT; otherwise the short summary (cut if it
    still does not fit)."""
    def build(text):
        return f'{base}?title={quote(title, safe="")}&body={quote(text, safe="")}'
    if len(body) <= BODY_LIMIT:
        url = build(body)
        if len(url) <= URL_LIMIT:
            return url, False
    text = short_body
    url = build(text)
    while len(url) > URL_LIMIT and len(text) > 200:
        text = text[:int(len(text) * 0.8)].rstrip()
        url = build(text)
    return url, True


def write_zip(path, report_text, log_text, log_name):
    """Saves report.md + the redacted log in a .zip. The log is stored under its own name with
    a '.redacted.log' ending so nobody mistakes it for the original."""
    stem = Path(log_name).stem if log_name else 'log'
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('report.md', report_text)
        if log_text is not None:
            z.writestr(f'{stem}.redacted.log', log_text)
