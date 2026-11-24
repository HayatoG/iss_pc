# SPDX-License-Identifier: GPL-2.0-or-later
"""What held the frame rate back in the last play session (2026-11-24), read from the game's own log:
nothing is measured by the launcher and nothing is estimated from part names.

The renderer v2 log has, every ~15 s:
  PRESENT_ACQUIRE count=N            frames shown in the window
  GPU_FRAME_MS avg=X busy_pct=Y      the graphics card's own time per shown frame (2026-11-24 builds)
  FRAME_PACE p99_ms=X over33=A over50=B frames=N   slowest 1 % of frame times, hitches (2026-11-24)
  SESSION_SETTINGS lod_scale=.. quality_floor=.. fps_limit=.. render_resolution=..  (2026-11-24)
and, now and then: FRAMES_AHEAD waited_ms_per_frame=X (the processor waiting for the card),
VIDEOOUT fps_limit=N (the frame rate limit in force, 0 = none), VRAM_BREAKDOWN usage=X MB
budget_now=Y MB (video memory in use and what the driver allows). The first windows (loading,
shader preparation) are left out; medians of the rest describe the session.
"""
import re
from pathlib import Path
from statistics import median

WINDOW_S = 15.0          # PRESENT_ACQUIRE windows (vk_swapchain.cpp: at least 15 s)
SKIP_WINDOWS = 2         # the first 30 s: loading and shader preparation
MIN_WINDOWS = 4          # about a minute of play before saying anything
ENGINE_CEILING = 120     # the game's own frame rate patch (tlg_graphics.h ParseEngineFps default)
AT_LIMIT = 0.93          # within 7 % of a limit counts as held by it
GPU_WAIT_SHARE = 0.20    # waiting a fifth of the frame or more for the card: the card is the limit
GPU_BUSY_SHARE = 0.85    # with GPU_FRAME_MS: the card busy 85 % of the frame or more
HITCHES_PER_MIN = 3.0    # frames over 50 ms per minute from which the card mentions freezes
VRAM_FULL = 0.90         # video memory in use against the driver's budget: nearly full
VRAM_FULL_SHARE = 1 / 3  # ... in at least a third of the samples

_ACQUIRE = re.compile(r'PRESENT_ACQUIRE\b.*?\bcount=(\d+)')
_WAITED = re.compile(r'FRAMES_AHEAD\b.*?\bwaited_ms_per_frame=([\d.]+)')
_LIMIT = re.compile(r'VIDEOOUT fps_limit=(\d+)')
_GPU = re.compile(r'GPU_FRAME_MS\b.*?\bavg=([\d.]+)(?:.*?\bbusy_pct=([\d.]+))?')
_PACE = re.compile(r'FRAME_PACE\b.*?\bp99_ms=(\d+)\b.*?\bover33=(\d+)\b.*?\bover50=(\d+)\b.*?\bframes=(\d+)')
_VRAM = re.compile(r'VRAM_BREAKDOWN\b.*?\busage=(\d+) MB\b.*?\bbudget_now=(\d+) MB')
_SETTINGS = re.compile(r'SESSION_SETTINGS\s+(.*)$')


def read(path):
    """The session's numbers from one game log, or None when the file cannot be read:
    fps, windows, limit, waited_ms, gpu_ms, busy_pct, p99_ms, hitches_per_min, vram_used_mb,
    vram_budget_mb, vram_full, settings (the last window's), settings_changed, renderer_v2."""
    try:
        text = Path(path).read_text(encoding='utf-8', errors='replace')
    except OSError:
        return None
    counts, waited, gpu, busy, pace, vram, settings, limit = [], [], [], [], [], [], [], None
    for line in text.splitlines():
        if 'PRESENT_ACQUIRE' in line:
            m = _ACQUIRE.search(line)
            if m:
                counts.append(int(m.group(1)))
        elif 'FRAMES_AHEAD' in line:
            m = _WAITED.search(line)
            if m:
                waited.append(float(m.group(1)))
        elif 'VIDEOOUT fps_limit' in line:
            m = _LIMIT.search(line)
            if m:
                limit = int(m.group(1))          # the last one in force
        elif 'GPU_FRAME_MS' in line:
            m = _GPU.search(line)
            if m:
                gpu.append(float(m.group(1)))
                if m.group(2):
                    busy.append(float(m.group(2)))
        elif 'FRAME_PACE' in line:
            m = _PACE.search(line)
            if m:
                pace.append(tuple(int(x) for x in m.groups()))
        elif 'VRAM_BREAKDOWN' in line:
            m = _VRAM.search(line)
            if m and int(m.group(2)) > 0:
                vram.append((int(m.group(1)), int(m.group(2))))
        elif 'SESSION_SETTINGS' in line:
            m = _SETTINGS.search(line)
            if m:
                settings.append(dict(kv.split('=', 1) for kv in m.group(1).split() if '=' in kv))
    play = counts[SKIP_WINDOWS:]
    # The other per-window lines come right after each PRESENT_ACQUIRE: skip the same loading windows.
    gpu_play, busy_play = gpu[SKIP_WINDOWS:] or gpu, busy[SKIP_WINDOWS:] or busy
    pace_play = pace[SKIP_WINDOWS:] or pace
    settings_play = settings[SKIP_WINDOWS:] or settings
    report = {
        'fps': round(median(play) / WINDOW_S, 1) if play else None,
        'windows': len(play),
        'limit': limit,
        'waited_ms': round(median(waited[1:] or waited), 2) if waited else None,
        'gpu_ms': round(median(gpu_play), 2) if gpu_play else None,
        'busy_pct': round(median(busy_play)) if busy_play else None,
        'p99_ms': None, 'hitches_per_min': None,
        'vram_used_mb': None, 'vram_budget_mb': None, 'vram_full': False,
        'settings': settings_play[-1] if settings_play else None,
        'settings_changed': len({tuple(sorted(s.items())) for s in settings_play}) > 1,
        'renderer_v2': 'renderer v2' in text or bool(waited),
    }
    if pace_play:
        report['p99_ms'] = int(median(p[0] for p in pace_play))
        over50, frames = sum(p[2] for p in pace_play), sum(p[3] for p in pace_play)
        minutes = len(pace_play) * WINDOW_S / 60.0
        report['hitches_per_min'] = round(over50 / minutes, 1) if minutes and frames else None
    if vram:
        used, budget = median(u for u, _ in vram), median(b for _, b in vram)
        report['vram_used_mb'], report['vram_budget_mb'] = int(used), int(budget)
        full = sum(1 for u, b in vram if u >= b * VRAM_FULL)
        report['vram_full'] = full >= len(vram) * VRAM_FULL_SHARE
    return report


def verdict(report):
    """(key, values) for the launcher's text. key: none | classic | short | limit | ceiling | gpu | cpu."""
    if not report or report['fps'] is None and not report.get('windows'):
        return 'none', {}
    if not report['renderer_v2']:
        return 'classic', {}
    if report['windows'] < MIN_WINDOWS or report['fps'] is None:
        return 'short', {}
    fps = report['fps']
    values = {'fps': int(round(fps))}
    limit = report['limit'] or 0
    if limit and fps >= limit * AT_LIMIT:
        return 'limit', dict(values, limit=limit)
    if not limit and fps >= ENGINE_CEILING * AT_LIMIT:
        return 'ceiling', dict(values, limit=ENGINE_CEILING)
    frame_ms = 1000.0 / fps if fps > 0 else 0.0
    if report.get('gpu_ms') is not None and frame_ms:
        # Measured: the card's own time per shown frame against the frame time.
        busy = report.get('busy_pct')
        busy = busy if busy is not None else round(100 * report['gpu_ms'] / frame_ms)
        values.update(busy=int(min(100, busy)), sure=True)
        card_limit = report['gpu_ms'] >= frame_ms * GPU_BUSY_SHARE
    else:
        card_limit = report['waited_ms'] is not None and frame_ms and report['waited_ms'] >= frame_ms * GPU_WAIT_SHARE
    return ('gpu' if card_limit else 'cpu'), values


def notes(report):
    """[(key, values)] said after the verdict: hitches, video memory nearly full, settings changed
    during the session. Empty without the newer log lines."""
    if not report or report.get('fps') is None or report.get('windows', 0) < MIN_WINDOWS:
        return []
    out = []
    if report.get('vram_full'):
        out.append(('vram', {'used': round(report['vram_used_mb'] / 1024, 1),
                             'budget': round(report['vram_budget_mb'] / 1024, 1)}))
    hitches = report.get('hitches_per_min')
    if hitches is not None and hitches >= HITCHES_PER_MIN:
        out.append(('hitches_vram' if report.get('vram_full') else 'hitches',
                    {'n': int(round(hitches)), 'p99': report.get('p99_ms') or 0}))
    if report.get('settings_changed'):
        out.append(('mixed', {}))
    return out


def summary(report):
    """The few numbers kept to compare the next session with (before/after an Apply)."""
    if not report or report.get('fps') is None:
        return None
    return {'fps': int(round(report['fps'])), 'p99_ms': report.get('p99_ms'),
            'busy_pct': report.get('busy_pct'), 'hitches_per_min': report.get('hitches_per_min')}


def latest_log(log_dir):
    """The newest run-*.log in the game's log folder, or None."""
    try:
        logs = sorted(Path(log_dir).glob('run-*.log'), key=lambda p: p.stat().st_mtime)
    except OSError:
        return None
    return logs[-1] if logs else None
