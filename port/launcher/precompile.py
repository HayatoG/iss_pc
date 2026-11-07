# SPDX-License-Identifier: GPL-2.0-or-later
"""Shader jobs file of the first start: decides whether it must be (re)generated, runs the
generator (port/tools/xpps) in a subprocess and records what it was made from.

The game compiles the shaders listed in <GPU user dir>/cache/<serial>-jobs.bin behind its
"Compiling shaders" screen; without the file it starts as before. Nothing here may stop the player
from playing: prepare_jobs() never raises and a failed generator only leaves a line in the log.

Files (next to each other, in the GPU cache folder):
  <serial>-jobs.bin    the jobs, written by the generator to <serial>-jobs.bin.part and renamed here
  <serial>-jobs.json   {format, generator, fingerprint, xpps_files, ...}: the file is up to date while
                       generator and fingerprint match; written after the .bin, so no json = redo
The generator version is a hash of the tools/xpps sources (no number to forget to bump); the game
fingerprint is a hash of path, size and mtime of every .xpps of the player's game folder.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import threading
import time

import infamous_run as tlg_run
import shader_cache

META_FORMAT = 1
DEFAULT_SERIAL = 'CUSA00223'
PROGRESS = re.compile(r'^PROGRESS (\w+) (\d+) (\d+)\s*$')
# generator phase -> TEXT key (infamous_launcher.py), shown in the launcher's language
PHASE_TEXT = {'index': 'jobs_phase_index', 'jobs': 'jobs_phase_jobs'}


def tools_dir():
    return tlg_run.PORT_DIR / 'tools'


def parse_progress(line):
    """'PROGRESS <phase> <n> <total>' -> (phase, n, total), else None."""
    found = PROGRESS.match(line.strip()) if isinstance(line, str) else None
    return (found.group(1), int(found.group(2)), int(found.group(3))) if found else None


def generator_version(root=None):
    """Hash of the generator's sources (every .py of tools/xpps) and of its data (data/*.json, the
    observed witness table): changes when the generator or what it reads does."""
    digest = hashlib.sha256()
    folder = Path(root) if root else tools_dir() / 'xpps'
    for path in sorted(folder.glob('*.py')) + sorted(folder.glob('data/*.json')):
        name = path.relative_to(folder).as_posix()
        digest.update(name.encode() + b'\0' + path.read_bytes() + b'\0')
    return digest.hexdigest()[:16]


def xpps_root(game_dir):
    """The folder with the player's .xpps packages (searched recursively by the generator)."""
    game = Path(game_dir)
    for candidate in (game / 'art' / 'cache', game / 'art'):
        if candidate.is_dir():
            return candidate
    return None


def fingerprint(root):
    """(hash of relative path + size + mtime of every .xpps under root, file count)."""
    digest = hashlib.sha256()
    entries = []
    for path in Path(root).rglob('*.xpps'):
        try:
            info = path.stat()
        except OSError:
            continue
        entries.append((path.relative_to(root).as_posix(), info.st_size, info.st_mtime_ns))
    for name, size, mtime in sorted(entries):
        digest.update(f'{name}\0{size}\0{mtime}\n'.encode())
    return digest.hexdigest()[:32], len(entries)


def serial_of(game_dir):
    return shader_cache.title_id(game_dir) or DEFAULT_SERIAL


def paths(game_dir):
    """(jobs file, json file, index file) of this game in the GPU cache folder."""
    cache = tlg_run.gpu_user_dir() / 'cache'
    serial = serial_of(game_dir)
    return cache / f'{serial}-jobs.bin', cache / f'{serial}-jobs.json', cache / f'{serial}-jobs.index.sqlite'


def read_meta(path):
    try:
        data = json.loads(Path(path).read_text(encoding='utf-8'))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def atomic_write(path, data):
    """Writes bytes or text to <path>.tmp and renames it over path: never half a file."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    try:
        if isinstance(data, str):
            data = data.encode('utf-8')
        with open(tmp, 'wb') as out:
            out.write(data)
            out.flush()
            os.fsync(out.fileno())
        os.replace(tmp, path)
    finally:
        tmp.unlink(missing_ok=True)


def remove_all(*files):
    """Deletes these files and their .part/.tmp leftovers (option turned off)."""
    for path in files:
        for name in (path, *(path.with_name(path.name + suffix) for suffix in ('.part', '.part.tmp', '.tmp'))):
            try:
                name.unlink(missing_ok=True)
            except OSError:
                pass


def decide(settings, version=None, print_fingerprint=None):
    """('off'|'nogame'|'current'|'generate', reason). The cheap part of the decision: no generator run."""
    if not settings.get('precompile_shaders', True):
        return 'off', 'option off'
    game = settings.get('game_dir') or ''
    root = xpps_root(game) if game else None
    if root is None:
        return 'nogame', 'no .xpps folder'
    jobs, meta_path, _ = paths(game)
    meta = read_meta(meta_path)
    version = version or generator_version()
    prints = print_fingerprint or fingerprint(root)
    if not jobs.is_file():
        return 'generate', 'no jobs file'
    if meta.get('format') != META_FORMAT or meta.get('generator') != version:
        return 'generate', 'generator changed'
    if meta.get('fingerprint') != prints[0]:
        return 'generate', 'game files changed'
    return 'current', 'up to date'


def generator_command(root, out, index):
    """THE place that knows how to call the generator (interface: iss_ref/agent_status/jobs_cli.txt):
    scripts/build_jobs.py <xpps folder> <out> --index <sqlite> --jobs <N>, run by the launcher itself
    (--script) in a packaged build, by python otherwise. The player waits for it before the game
    starts, so it takes every core but one (no --nice)."""
    script = tlg_run.PORT_DIR / 'scripts' / 'build_jobs.py'
    return [*tlg_run.python_command(script), str(root), str(out), '--index', str(index),
            '--jobs', str(generator_processes())]


def generator_processes():
    return max(1, (os.cpu_count() or 2) - 1)


NEW_GROUP = getattr(subprocess, 'CREATE_NEW_PROCESS_GROUP', 0)


def kill_tree(process):
    """Stops the generator and its worker processes (a group on POSIX, taskkill /T on Windows)."""
    try:
        if os.name == 'nt':
            subprocess.run(['taskkill', '/F', '/T', '/PID', str(process.pid)], capture_output=True,
                           creationflags=tlg_run.NO_WINDOW)
        else:
            import signal
            os.killpg(process.pid, signal.SIGKILL)
    except (OSError, ProcessLookupError):
        try:
            process.kill()
        except OSError:
            pass


class Cancelled(Exception):
    pass


class Precompiler:
    """One run of prepare(); cancel() may come from any thread (window closed, Quit)."""

    def __init__(self):
        self._stop = threading.Event()
        self._lock = threading.Lock()
        self._process = None

    @property
    def cancelled(self):
        return self._stop.is_set()

    def cancel(self):
        self._stop.set()
        with self._lock:
            process = self._process
        if process is not None and process.poll() is None:
            kill_tree(process)

    def _spawn(self, command):
        with self._lock:
            if self._stop.is_set():
                raise Cancelled()
            self._process = subprocess.Popen(
                command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, encoding='utf-8',
                errors='replace', bufsize=1, creationflags=tlg_run.NO_WINDOW | NEW_GROUP,
                start_new_session=(os.name != 'nt'))   # the index phase has worker processes: one group
        return self._process

    def run(self, settings, log=print, progress=None, version=None):
        """Makes the jobs file when decide() says so. Returns 'off'|'nogame'|'current'|'done'|
        'failed'|'cancelled'; never raises (a problem is a log line and the game starts anyway).
        progress(phase, n, total) is called from this thread for every generator line."""
        try:
            return self._run(settings, log, progress, version)
        except Cancelled:
            return 'cancelled'
        except Exception as error:  # noqa: BLE001  nothing here may stop the game
            log(message('jobs_error', detail=f'{type(error).__name__}: {error}', settings=settings))
            return 'failed'

    def _run(self, settings, log, progress, version):
        game = settings.get('game_dir') or ''
        action, reason = decide(settings, version)
        if action == 'off':
            if game:
                remove_all(*paths(game))
            return 'off'
        if action != 'generate':
            return action
        root = xpps_root(game)
        jobs, meta_path, index = paths(game)
        prints = fingerprint(root)
        version = version or generator_version()
        log(message('jobs_start', settings=settings))
        meta_path.unlink(missing_ok=True)      # the old file no longer matches: no json until the new one is whole
        part = jobs.with_name(jobs.name + '.part')
        for leftover in (part, part.with_name(part.name + '.tmp')):   # a killed run's leftovers
            leftover.unlink(missing_ok=True)
        jobs.parent.mkdir(parents=True, exist_ok=True)
        started = time.monotonic()
        tail, stats, phase = [], {}, None
        process = self._spawn(generator_command(root, part, index))
        try:
            for line in process.stdout:
                seen = parse_progress(line)
                if seen:
                    if progress:
                        progress(*seen)
                    if seen[0] != phase:
                        phase = seen[0]
                        log(message(PHASE_TEXT.get(phase, 'jobs_phase_other'), settings=settings, phase=phase))
                    continue
                line = line.rstrip()
                if line.startswith('{'):
                    try:
                        stats = json.loads(line)
                    except ValueError:
                        pass
                if line:
                    tail.append(line)
                    del tail[:-12]
            code = process.wait()
        finally:                 # also when a callback raises: never leave the generator running
            if process.poll() is None:
                kill_tree(process)
            process.stdout.close()
            process.wait()
        if self.cancelled:       # closed or cancelled midway: no half file (an older finished one stays)
            for leftover in (part, part.with_name(part.name + '.tmp')):
                leftover.unlink(missing_ok=True)
            raise Cancelled()
        if code != 0 or not part.is_file():
            for line in tail:
                log(f'  {line}')
            part.unlink(missing_ok=True)
            log(message('jobs_failed', settings=settings, code=code))
            return 'failed'
        os.replace(part, jobs)
        seconds = round(time.monotonic() - started, 1)
        meta = {'format': META_FORMAT, 'generator': version, 'fingerprint': prints[0], 'xpps_files': prints[1],
                'jobs_bytes': jobs.stat().st_size, 'seconds': seconds, 'created': int(time.time())}
        if isinstance(stats, dict) and isinstance(stats.get('records'), int):
            meta['records'] = stats['records']
        atomic_write(meta_path, json.dumps(meta, indent=2) + '\n')
        index.unlink(missing_ok=True)          # ~300 MB, only useful for a faster redo after a game update
        log(message('jobs_done', settings=settings, seconds=seconds))
        return 'done'


TEXT = {
    'precompile_shaders': ('Compile shaders on the first start', 'Compilar shaders na primeira abertura'),
    'jobs_start': ('Preparing the shader list (first start, about a minute)…',
                   'Preparando a lista de shaders (primeira abertura, cerca de um minuto)…'),
    'jobs_phase_index': ('Indexing the game files…', 'Indexando os arquivos do jogo…'),
    'jobs_phase_jobs': ('Preparing the shader list…', 'Preparando a lista de shaders…'),
    'jobs_phase_other': ('Preparing the shader list ({phase})…', 'Preparando a lista de shaders ({phase})…'),
    'jobs_done': ('Shader list ready in {seconds} s.', 'Lista de shaders pronta em {seconds} s.'),
    'jobs_failed': ('The shader list could not be made (code {code}); the game starts without it and compiles '
                    'shaders while you play, as before.',
                    'Não deu para gerar a lista de shaders (código {code}); o jogo abre sem ela e compila os '
                    'shaders durante a partida, como antes.'),
    'jobs_error': ('Shader list skipped: {detail}', 'Lista de shaders ignorada: {detail}'),
    'jobs_cancelled': ('Shader list cancelled.', 'Lista de shaders cancelada.'),
}


def message(key, settings=None, **fmt):
    """TEXT[key] in the launcher's language (the Tk and Qt launchers merge TEXT into their own)."""
    pair = TEXT[key]
    try:
        return pair[1 if _portuguese(settings or {}) else 0].format(**fmt)
    except (KeyError, IndexError):
        return pair[0]


def _portuguese(settings):
    choice = settings.get('ui_language', 'auto')
    if choice != 'auto':
        return choice == 'pt_BR'
    return tlg_run.system_game_language() == 'pt_BR'


def prepare_jobs(settings, log=print, progress=None, precompiler=None):
    """What both launchers call before starting the game."""
    return (precompiler or Precompiler()).run(settings, log, progress)
