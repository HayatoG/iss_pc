# SPDX-License-Identifier: GPL-2.0-or-later
"""Where the player's saves and data live, and how to move or find them (no Qt, no Tk).

The folder is resolved in one place, user_dir(), in this order:
  1. ISS_USER_DIR in the environment (TLG_USER_DIR is the old name and still works);
  2. the per-user pointer file, kept OUTSIDE the game folder on purpose so that it survives
     extracting a new version somewhere else or deleting the old one:
       Windows  %APPDATA%\\inFAMOUS-PC\\launcher.json
       Linux    $XDG_CONFIG_HOME/infamous-pc/launcher.json   (default ~/.config)
     key "user_dir" (the same file keeps "backup_extra_dir" and "backup_keep");
  3. <port folder>/user, the portable mode (what the port always did).

Nothing here deletes or moves a save: copies go to a temporary name and are renamed into place.
"""
import json
import os
from pathlib import Path
import shutil
import time

SERIAL = 'CUSA00223'
SERIALS = (SERIAL, 'CUSA00004')
SLOT_PREFIX = 'infamousss'
ENV_NAMES = ('ISS_USER_DIR', 'TLG_USER_DIR')
# What a data folder holds that is worth carrying over. The shader cache (cache/) is left out: it is
# large and the game rebuilds it.
MIGRATE = ('savedata', 'savedata_backups', 'tlg.ini')


# ------------------------------------------------------------------------------- pointer file
def pointer_path(env=None, nt=None):
    env = os.environ if env is None else env
    nt = (os.name == 'nt') if nt is None else nt
    if nt:
        base = Path(env.get('APPDATA') or Path.home() / 'AppData' / 'Roaming')
        return base / 'inFAMOUS-PC' / 'launcher.json'
    return Path(env.get('XDG_CONFIG_HOME') or Path.home() / '.config') / 'infamous-pc' / 'launcher.json'


def read_pointer(path=None):
    try:
        data = json.loads(Path(path or pointer_path()).read_text(encoding='utf-8'))
        return data if isinstance(data, dict) else {}
    except (OSError, ValueError):
        return {}


def write_pointer(updates, path=None):
    """Sets keys in the pointer file (None removes one); written to a temp file, then renamed."""
    path = Path(path or pointer_path())
    data = read_pointer(path)
    for key, value in updates.items():
        if value is None:
            data.pop(key, None)
        else:
            data[key] = value
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_name(path.name + '.tmp')
    tmp.write_text(json.dumps(data, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    os.replace(tmp, path)


# --------------------------------------------------------------------------------- resolution
def _env_dir(env):
    for name in ENV_NAMES:
        if env.get(name):
            return env[name]
    return ''


def user_dir_source(env=None, pointer=None):
    """'env', 'pointer' or 'default': which rule picks the data folder."""
    env = os.environ if env is None else env
    if _env_dir(env):
        return 'env'
    pointer = read_pointer() if pointer is None else pointer
    return 'pointer' if str(pointer.get('user_dir') or '').strip() else 'default'


def default_dir(port_dir):
    return Path(port_dir) / 'user'


def user_dir(port_dir, env=None, pointer=None):
    """The data folder (saves, tlg.ini, backups, GPU caches). `pointer` is the dict of the pointer
    file, for tests; by default it is read from disk."""
    env = os.environ if env is None else env
    value = _env_dir(env)
    if not value:
        pointer = read_pointer() if pointer is None else pointer
        value = str(pointer.get('user_dir') or '').strip()
    if value:
        return Path(value).expanduser()
    return default_dir(port_dir)


def set_user_dir(path):
    write_pointer({'user_dir': str(Path(path).expanduser())})


def reset_user_dir():
    write_pointer({'user_dir': None})


def suggested_dir(nt=None, env=None):
    """A stable place outside the game folder: Documents/inFAMOUS-PC (Windows),
    ~/.local/share/infamous-pc (Linux)."""
    nt = (os.name == 'nt') if nt is None else nt
    env = os.environ if env is None else env
    if nt:
        return Path.home() / 'Documents' / 'inFAMOUS-PC'
    return Path(env.get('XDG_DATA_HOME') or Path.home() / '.local' / 'share') / 'infamous-pc'


def same_path(a, b):
    try:
        return Path(a).resolve() == Path(b).resolve()
    except OSError:
        return str(a) == str(b)


# ------------------------------------------------------------------------- saves on disk
def slot_dirs(savedata):
    """[(user id, slot name, slot folder)] under <savedata>/<user>/CUSA00223/, sorted."""
    out = []
    root = Path(savedata)
    try:
        users = sorted(p for p in root.iterdir() if p.is_dir())
    except OSError:
        return out
    for user in users:
        for serial in SERIALS:
            game = user / serial
            try:
                slots = sorted(p for p in game.iterdir() if p.is_dir() and not p.name.endswith('.sce_sys'))
            except OSError:
                continue
            # Preserve old US identifiers; qualify other regions to prevent collisions.
            uid = user.name if serial == SERIAL else f'{user.name}/{serial}'
            out += [(uid, p.name, p) for p in slots]
    return out


def tree_size(path):
    total = 0
    for p in Path(path).rglob('*'):
        try:
            if p.is_file():
                total += p.stat().st_size
        except OSError:
            pass
    return total


def newest_mtime(path):
    best = 0.0
    for p in Path(path).rglob('*'):
        try:
            if p.is_file():
                best = max(best, p.stat().st_mtime)
        except OSError:
            pass
    return best


def summarize(data_dir):
    """(number of saves, mtime of the newest one) of a data folder; (0, 0) without saves."""
    slots = slot_dirs(Path(data_dir) / 'savedata')
    newest = 0.0
    for _, name, folder in slots:
        newest = max(newest, newest_mtime(folder), newest_mtime(folder.with_name(name + '.sce_sys')))
    return len(slots), newest


def has_saves(data_dir):
    return bool(slot_dirs(Path(data_dir) / 'savedata'))


# --------------------------------------------------------------------- copy (never move)
class DataError(Exception):
    """A readable problem for the player: key (i18n) + detail."""

    def __init__(self, key, detail=''):
        super().__init__(f'{key}: {detail}')
        self.key, self.detail = key, detail


def _existing_parent(path):
    path = Path(path).absolute()
    while not path.exists() and path != path.parent:
        path = path.parent
    return path


def free_bytes(path):
    return shutil.disk_usage(_existing_parent(path)).free


def copy_tree_atomic(src, dst):
    """Copies a folder to `dst` (which must not exist) through a temporary sibling, then renames."""
    src, dst = Path(src), Path(dst)
    if dst.exists():
        raise DataError('data_exists', str(dst))
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_name(f'{dst.name}.copying-{os.getpid()}-{int(time.time() * 1000) % 100000}')
    try:
        shutil.copytree(src, tmp)
        os.replace(tmp, dst)
    except BaseException:
        shutil.rmtree(tmp, ignore_errors=True)
        raise


def copy_file_atomic(src, dst):
    src, dst = Path(src), Path(dst)
    dst.parent.mkdir(parents=True, exist_ok=True)
    tmp = dst.with_name(dst.name + '.copying')
    try:
        shutil.copy2(src, tmp)
        os.replace(tmp, dst)
    except BaseException:
        tmp.unlink(missing_ok=True)
        raise


def migration_size(src):
    total = 0
    for name in MIGRATE:
        p = Path(src) / name
        if p.is_dir():
            total += tree_size(p)
        elif p.is_file():
            total += p.stat().st_size
    return total


def migrate(src, dst):
    """Copies saves, backups and tlg.ini from the old data folder to the new one. The old folder
    is never touched. Items already present in `dst` are kept (not overwritten). Returns
    {'copied': [names], 'skipped': [names], 'bytes': n}; raises DataError when it cannot even start
    (same folder, no space, not writable)."""
    src, dst = Path(src), Path(dst)
    if same_path(src, dst):
        raise DataError('data_same_folder', str(dst))
    need = migration_size(src)
    try:
        dst.mkdir(parents=True, exist_ok=True)
        free = free_bytes(dst)
    except OSError as error:
        raise DataError('data_not_writable', f'{dst}: {error}')
    if free < need * 1.1 + 1_000_000:
        raise DataError('data_no_space', f'{need} > {free}')
    copied, skipped = [], []
    for name in MIGRATE:
        a, b = src / name, dst / name
        if not a.exists():
            continue
        if b.exists():
            skipped.append(name)
            continue
        try:
            (copy_tree_atomic if a.is_dir() else copy_file_atomic)(a, b)
        except OSError as error:
            raise DataError('data_copy_failed', f'{name}: {error}')
        copied.append(name)
    return {'copied': copied, 'skipped': skipped, 'bytes': need}


# ----------------------------------------------------------------- find saves of other installs
SKIP_NAMES = {'node_modules', '.git', 'windows', 'program files', 'program files (x86)', 'programdata',
              '$recycle.bin', 'system volume information', 'appdata', 'out', 'savedata', 'savedata_backups',
              'cache', 'user', '__pycache__', 'proc', 'sys', 'dev', 'snap'}


def candidate_roots(port_dir, nt=None, home=None):
    """Common places a previous copy of the port may sit in, plus the folders around this one."""
    nt = (os.name == 'nt') if nt is None else nt
    home = Path(home) if home else Path.home()
    roots = [home / n for n in ('Downloads', 'Transferências', 'Desktop', 'Área de Trabalho', 'Documents',
                                'Documentos', 'Games', 'Jogos')]
    roots.append(home)
    if nt:
        for letter in 'CDEFG':
            roots += [Path(f'{letter}:/Games'), Path(f'{letter}:/Jogos'), Path(f'{letter}:/')]
    else:
        roots += [home / '.local' / 'share']
    port = Path(port_dir).resolve()
    # the folder this port sits in, and its parents: covers folder-inside-folder extractions
    for p in (port, *list(port.parents)[:3]):
        roots.append(p)
        try:
            roots += [s for s in p.iterdir() if s.is_dir() and 'infamous' in s.name.lower()]
        except OSError:
            pass
    seen, out = set(), []
    for r in roots:
        try:
            key = r.resolve()
        except OSError:
            continue
        if key not in seen and r.is_dir():
            seen.add(key)
            out.append(r)
    return out


def find_installs(roots, exclude=(), max_dirs=30000, deadline=8.0, depth=3):
    """Data folders with saves: any folder X where X/savedata/*/CUSA00223/<slot> exists or
    X/user/savedata/... exists. Walks `roots` with limits (depth, number of folders, seconds); folders
    whose name contains "infamous" get extra depth (folder-inside-folder extractions).
    Returns [{'path': data folder, 'saves': n, 'newest': mtime}] newest first."""
    started = time.monotonic()
    excluded = []
    for e in exclude:
        try:
            excluded.append(Path(e).resolve())
        except OSError:
            pass
    found, seen_dirs, visited = {}, set(), [0]

    def consider(data):
        try:
            key = data.resolve()
        except OSError:
            return
        if key in found or key in excluded:
            return
        n, newest = summarize(data)
        if n:
            found[key] = {'path': str(key), 'saves': n, 'newest': newest}

    def walk(folder, remaining):
        if remaining <= 0 or visited[0] > max_dirs or time.monotonic() - started > deadline:
            return
        try:
            real = folder.resolve()
        except OSError:
            return
        if real in seen_dirs:
            return
        seen_dirs.add(real)
        visited[0] += 1
        try:
            entries = [e for e in os.scandir(folder) if e.is_dir(follow_symlinks=False)]
        except OSError:
            return
        names = {e.name.lower() for e in entries}
        if 'savedata' in names:
            consider(folder)
        if 'user' in names:
            consider(folder / 'user')
        for e in entries:
            low = e.name.lower()
            if low in SKIP_NAMES or (low.startswith('.') and 'infamous' not in low):
                continue
            nxt = max(remaining - 1, 3) if 'infamous' in low else remaining - 1
            walk(Path(e.path), nxt)

    for root in roots:
        walk(Path(root), depth)
    return sorted(found.values(), key=lambda f: -f['newest'])
