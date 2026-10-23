# SPDX-License-Identifier: GPL-2.0-or-later
"""Automatic graphics setup: what the player's hardware is, and what runs best on it.

The launcher no longer offers presets: it detects the GPU and the RAM and picks the setup that the
reports showed works on that hardware (issues up to v1.0, 2026-10-22):
  - renderer v2 everywhere. This port's renderer crashed inside AMD's Windows driver on every
    generation (amdvlk64.dll+0xebbbb on Polaris and Ryzen iGPUs, +0x2063335 on RDNA2 and later,
    44 reports) and runs at half the FPS (26 against 51 on the 11% route, RTX 2060);
  - native passes only on NVIDIA RTX cards on Windows with more than 16 GB of RAM: the best FPS
    there, but they hold ~0.8 GB more RAM (9.4 against 8.6 GB) and the reports elsewhere are too
    few (GTX, AMD, Intel) or crashed (NVIDIA's Linux driver);
  - the texture budget stays automatic (renderer v2 measures the card live).
Detection never needs vulkaninfo, which many Windows players lack: the display adapters' registry
key on Windows, /proc/driver/nvidia and /sys/class/drm on Linux.
"""
import os
import re
import sys
from pathlib import Path

VENDORS = {0x10de: 'NVIDIA', 0x1002: 'AMD', 0x8086: 'Intel'}
# Windows: the display adapters class key (one numbered subkey per adapter).
DISPLAY_CLASS = r'SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'


def vendor_of(name):
    """'NVIDIA' | 'AMD' | 'Intel' | '' from an adapter name."""
    text = (name or '').lower()
    if re.search(r'nvidia|geforce|quadro|\brtx\b|\bgtx\b', text):
        return 'NVIDIA'
    if re.search(r'amd|radeon|\bati\b', text):
        return 'AMD'
    if re.search(r'intel|\barc\b|iris|uhd graphics', text):
        return 'Intel'
    return ''


def _windows_adapters(winreg=None):
    """[(name, vram bytes or None)] of the display adapters in the registry."""
    if winreg is None:
        import winreg
    adapters = []
    try:
        root = winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, DISPLAY_CLASS)
    except OSError:
        return adapters
    for index in range(64):
        try:
            sub = winreg.EnumKey(root, index)
        except OSError:
            break
        if not sub.isdigit():
            continue
        try:
            key = winreg.OpenKey(root, sub)
        except OSError:
            continue

        def value(name):
            try:
                return winreg.QueryValueEx(key, name)[0]
            except OSError:
                return None
        name = value('DriverDesc')
        if not name:
            continue
        vram = value('HardwareInformation.qwMemorySize')
        if vram is None:
            raw = value('HardwareInformation.MemorySize')
            vram = int.from_bytes(raw[:4], 'little') if isinstance(raw, (bytes, bytearray)) else raw
        adapters.append((str(name), int(vram) if vram else None))
    return adapters


def _linux_adapters(root='/'):
    """[(name, vram bytes or None)]: NVIDIA's /proc models, then each DRM card's vendor and VRAM."""
    root = Path(root)
    adapters = []
    for info in sorted((root / 'proc/driver/nvidia/gpus').glob('*/information')):
        m = re.search(r'^Model:\s*(.+)$', info.read_text(errors='replace'), re.M)
        if m:
            adapters.append((m.group(1).strip(), None))
    for device in sorted((root / 'sys/class/drm').glob('card[0-9]*/device')):
        try:
            vendor = int((device / 'vendor').read_text().strip(), 16)
        except (OSError, ValueError):
            continue
        name = VENDORS.get(vendor, '')
        if not name or (name == 'NVIDIA' and adapters):
            continue
        try:
            vram = int((device / 'mem_info_vram_total').read_text().strip())
        except (OSError, ValueError):
            vram = None
        adapters.append((name, vram))
    return adapters


def ram_bytes():
    """Physical RAM in bytes; None when unknown."""
    if sys.platform.startswith('linux'):
        try:
            m = re.search(r'^MemTotal:\s*(\d+) kB', Path('/proc/meminfo').read_text(), re.M)
            return int(m.group(1)) * 1024 if m else None
        except OSError:
            return None
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


def _pick(adapters):
    """The adapter the game will most likely use: a discrete-looking NVIDIA/AMD card first."""
    def rank(adapter):
        name, vram = adapter
        vendor = vendor_of(name)
        integrated = bool(re.search(r'radeon\(tm\) graphics|radeon graphics|vega \d+ graphics|\d{3}m\b|uhd|iris',
                                    name.lower()))
        return (vendor == '' , integrated, vendor == 'Intel', -(vram or 0))
    return min(adapters, key=rank) if adapters else None


def detect(platform=None, adapters=None, ram=None):
    """{'os', 'gpu', 'vendor', 'vram_gb', 'ram_gb'}; unknown values are '' or None."""
    platform = platform or sys.platform
    if adapters is None:
        try:
            adapters = _windows_adapters() if platform == 'win32' else _linux_adapters()
        except Exception:
            adapters = []
    best = _pick(adapters)
    name, vram = best if best else ('', None)
    ram = ram_bytes() if ram is None else ram
    return {'os': 'windows' if platform == 'win32' else 'linux', 'gpu': name, 'vendor': vendor_of(name),
            'vram_gb': round(vram / 2 ** 30, 1) if vram else None,
            'ram_gb': round(ram / 2 ** 30, 1) if ram else None}


def decide(hw):
    """{'native_passes': bool, 'profile': text for the log}. Renderer v2 always."""
    rtx = hw['vendor'] == 'NVIDIA' and re.search(r'\brtx\b', hw['gpu'].lower()) is not None
    enough_ram = hw['ram_gb'] is not None and hw['ram_gb'] > 16.5
    native = hw['os'] == 'windows' and rtx and enough_ram
    why = ('nvidia rtx, windows, >16 GB RAM' if native else
           'linux' if hw['os'] != 'windows' else
           '16 GB RAM or less' if rtx and not enough_ram else
           'not an NVIDIA RTX card')
    profile = (f"v2 native_passes={int(native)} ({why}); gpu={hw['gpu'] or '?'} vendor={hw['vendor'] or '?'} "
               f"vram={hw['vram_gb'] or '?'} GB ram={hw['ram_gb'] or '?'} GB")
    return {'native_passes': native, 'profile': profile}


def advanced_mode(settings, environ=None):
    """The hidden advanced mode (support and testers): every option and preset is shown and applies.
    ISS_ADVANCED=1, or the launcher's Ctrl+Shift+A (settings.json 'advanced_mode')."""
    environ = os.environ if environ is None else environ
    if environ.get('ISS_ADVANCED', '') in ('0', '1'):
        return environ['ISS_ADVANCED'] == '1'
    return bool(settings.get('advanced_mode', False))
