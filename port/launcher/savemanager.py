# SPDX-License-Identifier: GPL-2.0-or-later
"""Managing the player's saves: list, back up, export/import .zip, duplicate, delete, restore.

Rules this module enforces (the launcher screens only call it):
  * every operation that can replace or remove a save goes through guarded(): it refuses to run
    while the game runs, makes a safety backup first (and mirrors it to the optional extra backup
    folder), and only then lets the change happen. If the backup cannot be made, nothing changes;
  * copies go to a temporary name and are renamed into place;
  * a .zip is validated (structure, ../ paths, absolute paths, links, size limits) before any byte
    of it is written, and an existing save is never overwritten unless the caller says so.
No Qt, no Tk.
"""
import datetime
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import sys
import time
import zipfile
from struct import unpack_from

import userdata
from userdata import DataError, SERIAL, SERIALS, SLOT_PREFIX

_FROZEN = bool(getattr(sys, 'frozen', False))
_SCRIPTS = (Path(sys.executable).resolve().parent if _FROZEN else Path(__file__).resolve().parent.parent) / 'scripts'
if str(_SCRIPTS) not in sys.path:
    sys.path.insert(0, str(_SCRIPTS))
import save_backup  # noqa: E402

SaveError = DataError
KEEP_DEFAULT = save_backup.KEEP
KEEP_MIN, KEEP_MAX = 3, 100
ZIP_MAX_FILES = 5000
ZIP_MAX_BYTES = 1 << 30
PROBE_NAMES = ('tlg-probe', 'tlg-probe.exe')
STAMP_RE = re.compile(r'^\d{8}-\d{6}(-\d+)?$')
UID_RE = re.compile(r'^[A-Za-z0-9_-]{1,32}$')
SLOT_RE = re.compile(r'^' + SLOT_PREFIX + r'[A-Za-z0-9_]{1,40}$')


# --------------------------------------------------------------------------- param.bin / .sfo
def read_sfo(data):
    """{key: value} of a PSF file (param.sfo); {} when it is not one."""
    try:
        magic, _, keys, values, count = unpack_from('<4sIIII', data, 0)
        if magic != b'\0PSF':
            return {}
        out = {}
        for i in range(count):
            key, fmt, size, _, offset = unpack_from('<HHIII', data, 20 + i * 16)
            name = data[keys + key:data.index(0, keys + key)].decode('ascii')
            raw = data[values + offset:values + offset + size]
            out[name] = (unpack_from('<I', raw)[0] if fmt == 0x0404 else
                         raw.split(b'\0', 1)[0].decode('utf-8', 'replace'))
        return out
    except Exception:  # damaged file: no title
        return {}


def _cstr(raw):
    return raw.split(b'\0', 1)[0].decode('utf-8', 'replace').strip()


def read_param(data):
    """{'title', 'subtitle', 'detail', 'time'} of a save's param file. Two layouts exist: a PSF
    (param.sfo) and the raw SceSaveDataParam the port writes as param.bin: title[128],
    subtitle[128], detail[1024], user param, then a 64-bit time at 0x508."""
    out = {'title': '', 'subtitle': '', 'detail': '', 'time': 0}
    if data[:4] == b'\0PSF':
        sfo = read_sfo(data)
        out.update(title=str(sfo.get('TITLE', '')).strip(), subtitle=str(sfo.get('SUBTITLE', '')).strip(),
                   detail=str(sfo.get('DETAIL', '')).strip())
        return out
    if len(data) >= 256:
        out.update(title=_cstr(data[0:128]), subtitle=_cstr(data[128:256]), detail=_cstr(data[256:1280]))
    if len(data) >= 0x510:
        stamp = unpack_from('<Q', data, 0x508)[0]
        if 1_400_000_000 < stamp < 4_000_000_000:   # a plausible unix time (2014..2096)
            out['time'] = stamp
    return out


def display_title(param):
    """The line shown for a save: subtitle (the game writes the chapter, karma and percent there)
    plus detail when it adds something, else the title."""
    parts = []
    for key in ('subtitle', 'detail'):
        if param.get(key) and param[key] not in parts:
            parts.append(param[key])
    return ' — '.join(parts) or param.get('title', '')


# --------------------------------------------------------------------------------- game running
def game_running(lister=None):
    """True while a tlg-probe process exists (its saves must not be touched)."""
    names = (lister or _process_names)()
    return any(n.lower() in PROBE_NAMES for n in names)


def _process_names():
    names = []
    if os.name == 'nt':
        try:
            flags = getattr(subprocess, 'CREATE_NO_WINDOW', 0)
            out = subprocess.run(['tasklist', '/FO', 'CSV', '/NH', '/FI', 'IMAGENAME eq tlg-probe.exe'],
                                 capture_output=True, text=True, timeout=10, creationflags=flags).stdout
            names = [line.split('","')[0].strip('"') for line in (out or '').splitlines() if line.strip()]
        except (OSError, subprocess.SubprocessError):
            pass
        return names
    for comm in Path('/proc').glob('[0-9]*/comm'):
        try:
            names.append(comm.read_text().strip())
        except OSError:
            pass
    return names


# ------------------------------------------------------------------------------------- prefs
def prefs():
    """Backup preferences, stored in the pointer file (outside the game folder)."""
    data = userdata.read_pointer()
    try:
        keep = int(data.get('backup_keep', KEEP_DEFAULT))
    except (TypeError, ValueError):
        keep = KEEP_DEFAULT
    return {'keep': max(KEEP_MIN, min(KEEP_MAX, keep)), 'extra': str(data.get('backup_extra_dir') or '')}


def set_prefs(keep=None, extra=None):
    updates = {}
    if keep is not None:
        updates['backup_keep'] = max(KEEP_MIN, min(KEEP_MAX, int(keep)))
    if extra is not None:
        updates['backup_extra_dir'] = str(extra) or None
    userdata.write_pointer(updates)


# ------------------------------------------------------------------------------------ slots
def _kind(name):
    rest = name[len(SLOT_PREFIX):].lower()
    return 'auto' if rest.startswith('auto') else 'user' if rest.startswith('user') else 'other'


def _natural(name):
    return [int(t) if t.isdigit() else t for t in re.split(r'(\d+)', name)]


def list_slots(user_dir):
    """One dict per save slot of the data folder, autosaves first."""
    out = []
    for uid, name, folder in userdata.slot_dirs(Path(user_dir) / 'savedata'):
        sys_dir = folder.with_name(name + '.sce_sys')
        param = {'title': '', 'subtitle': '', 'detail': '', 'time': 0}
        for fname in ('param.bin', 'param.sfo'):
            try:
                param = read_param((sys_dir / fname).read_bytes())
                break
            except OSError:
                continue
        mtime = max(userdata.newest_mtime(folder), userdata.newest_mtime(sys_dir))
        icon = sys_dir / 'icon0.png'
        out.append({'id': f'{uid}/{name}', 'user': uid, 'name': name, 'kind': _kind(name),
                    'title': display_title(param) or name, 'time': param['time'] or mtime,
                    'size': userdata.tree_size(folder) + userdata.tree_size(sys_dir),
                    'icon': str(icon) if icon.is_file() else ''})
    order = {'auto': 0, 'user': 1, 'other': 2}
    out.sort(key=lambda s: (s['user'], order[s['kind']], _natural(s['name'])))
    return out


def slot_paths(user_dir, slot_id):
    parts = str(slot_id).split('/')
    if len(parts) == 2:
        uid, name = parts
        serial = SERIAL
    elif len(parts) == 3:
        uid, serial, name = parts
    else:
        raise SaveError('save_bad_slot', slot_id)
    if serial not in SERIALS or not UID_RE.fullmatch(uid) or not SLOT_RE.fullmatch(name):
        raise SaveError('save_bad_slot', slot_id)
    game = Path(user_dir) / 'savedata' / uid / serial
    return game / name, game / (name + '.sce_sys')


# ---------------------------------------------------------------------------------- backups
def _signature(savedata):
    sig = []
    for p in sorted(Path(savedata).rglob('*')):
        if p.is_file():
            st = p.stat()
            sig.append((p.relative_to(savedata).as_posix(), st.st_size, int(st.st_mtime)))
    return sig


def _mirror(made, extra, keep):
    """Copies the backup `made` to the extra folder and prunes it. Returns an error text or ''."""
    if not extra:
        return ''
    try:
        extra = Path(extra)
        extra.mkdir(parents=True, exist_ok=True)
        target = extra / Path(made).name
        if not target.exists():
            need = userdata.tree_size(made)
            if userdata.free_bytes(extra) < need * 1.1 + 1_000_000:
                return f'{extra}: no space'
            userdata.copy_tree_atomic(made, target)
        names = sorted((p.name for p in extra.iterdir() if p.is_dir() and STAMP_RE.match(p.name)), reverse=True)
        for old in names[keep:]:
            shutil.rmtree(extra / old, ignore_errors=True)
        return ''
    except OSError as error:
        return f'{extra}: {error}'


def backup_now(user_dir, keep=None, extra=None, now=None):
    """Backs up <user>/savedata. Returns {'path': Path|None, 'reused': bool, 'extra_error': str}.
    path is None when there are no saves; reused is True when the newest backup already holds exactly
    these files (no new copy). Raises SaveError when the backup cannot be made."""
    cfg = prefs()
    keep = cfg['keep'] if keep is None else keep
    extra = cfg['extra'] if extra is None else extra
    user_dir = Path(user_dir)
    savedata = user_dir / 'savedata'
    if not userdata.has_saves(user_dir):
        return {'path': None, 'reused': False, 'extra_error': ''}
    names = save_backup.list_backups(user_dir)
    if names:
        newest = save_backup.backups_dir(user_dir) / names[0]
        try:
            if _signature(newest) == _signature(savedata):
                return {'path': newest, 'reused': True, 'extra_error': _mirror(newest, extra, keep)}
        except OSError:
            pass
    need = userdata.tree_size(savedata)
    try:
        save_backup.backups_dir(user_dir).mkdir(parents=True, exist_ok=True)
        if userdata.free_bytes(save_backup.backups_dir(user_dir)) < need * 1.1 + 1_000_000:
            raise SaveError('save_backup_no_space', str(user_dir))
        made = save_backup.backup(user_dir, now=now, keep=keep)
    except OSError as error:
        raise SaveError('save_backup_failed', str(error))
    return {'path': made, 'reused': False, 'extra_error': _mirror(made, extra, keep) if made else ''}


def backup_before_play(user_dir):
    """The automatic backup before each start: never blocks the game. Returns a short log line."""
    try:
        result = backup_now(user_dir)
    except SaveError as error:
        return f'Save backup failed, starting anyway: {error.key} {error.detail}'
    if result['path'] and not result['reused']:
        return f"Saves backed up to {result['path']}"
    return ''


def list_backups(user_dir):
    """[{'name', 'time' (unix), 'saves', 'size'}] newest first."""
    out = []
    root = save_backup.backups_dir(user_dir)
    for name in save_backup.list_backups(user_dir):
        folder = root / name
        try:
            when = datetime.datetime.strptime(name[:15], save_backup.STAMP).timestamp()
        except ValueError:
            when = folder.stat().st_mtime
        out.append({'name': name, 'time': when, 'saves': len(userdata.slot_dirs(folder)),
                    'size': userdata.tree_size(folder)})
    return out


def guarded(user_dir, change, running=None):
    """Runs change() only after the game is known not to run and a safety backup exists.
    Returns (result of change(), backup result)."""
    if (running or game_running)():
        raise SaveError('save_game_running')
    backup = backup_now(user_dir)
    return change(), backup


def restore_backup(user_dir, name, running=None, now=None):
    """Replaces the saves with backup `name`; the replaced saves are backed up first (in the
    backups folder and the extra one). Returns the safety backup folder (or None)."""
    if (running or game_running)():
        raise SaveError('save_game_running')
    cfg = prefs()
    try:
        safety = save_backup.restore(user_dir, name, now=now, keep=cfg['keep'])
    except ValueError:
        raise SaveError('save_backup_missing', name)
    except OSError as error:
        raise SaveError('save_restore_failed', str(error))
    return {'path': safety, 'extra_error': _mirror(safety, cfg['extra'], cfg['keep']) if safety else ''}


# ------------------------------------------------------------------------ duplicate / delete
def duplicate_slot(user_dir, slot_id, running=None):
    """Copies a slot to the next free infamousssUserN. Never overwrites. Returns the new slot id."""
    if (running or game_running)():
        raise SaveError('save_game_running')
    folder, sys_dir = slot_paths(user_dir, slot_id)
    if not folder.is_dir():
        raise SaveError('save_bad_slot', slot_id)
    uid = slot_id.rsplit('/', 1)[0]
    taken = {n for u, n, _ in userdata.slot_dirs(Path(user_dir) / 'savedata') if u == uid}
    n = 0
    while f'{SLOT_PREFIX}User{n}' in taken:
        n += 1
    new = f'{SLOT_PREFIX}User{n}'
    game = folder.parent
    try:
        userdata.copy_tree_atomic(folder, game / new)
        if sys_dir.is_dir():
            userdata.copy_tree_atomic(sys_dir, game / (new + '.sce_sys'))
    except OSError as error:
        shutil.rmtree(game / new, ignore_errors=True)
        raise SaveError('save_copy_failed', str(error))
    return f'{uid}/{new}'


def delete_slot(user_dir, slot_id, running=None):
    """Deletes a slot after the safety backup. Returns the backup result."""
    folder, sys_dir = slot_paths(user_dir, slot_id)
    if not folder.is_dir():
        raise SaveError('save_bad_slot', slot_id)

    def remove():
        trash = Path(user_dir) / f'.deleting-{os.getpid()}'
        trash.mkdir(parents=True, exist_ok=True)
        try:
            for i, p in enumerate((folder, sys_dir)):
                if p.exists():
                    os.replace(p, trash / str(i))
        finally:
            shutil.rmtree(trash, ignore_errors=True)
    _, backup = guarded(user_dir, remove, running)
    return backup


# --------------------------------------------------------------------------- import / export
def _safe_parts(name):
    """Path components of a zip member name, or raises SaveError('zip_unsafe')."""
    norm = name.replace('\\', '/')
    if norm.startswith('/') or re.match(r'^[A-Za-z]:', norm) or '\0' in norm:
        raise SaveError('zip_unsafe', name)
    parts = [p for p in norm.split('/') if p not in ('', '.')]
    if any(p == '..' or ':' in p for p in parts):
        raise SaveError('zip_unsafe', name)
    return parts


def _target_user(uid, target_uid):
    if target_uid is None:
        return uid
    parts = uid.split('/')
    return str(target_uid) if len(parts) == 1 else f'{target_uid}/{parts[1]}'


class Plan:
    """What a source (a .zip or a data folder) would add: slots[(uid, slot)] -> {'dir': [(parts, src)],
    'sys': [(parts, src)]}, plus how many entries were ignored."""

    def __init__(self, source, kind):
        self.source, self.kind = source, kind
        self.slots = {}
        self.ignored = 0
        self.total_bytes = 0

    @property
    def users(self):
        return sorted({u.split('/')[0] for u, _ in self.slots})

    def add(self, uid, slot, is_sys, rel, src, size):
        entry = self.slots.setdefault((uid, slot), {'dir': [], 'sys': [], 'param': None})
        entry['sys' if is_sys else 'dir'].append((rel, src))
        if is_sys and rel == ['param.bin']:
            entry['param'] = src
        self.total_bytes += size


def _classify(parts):
    """(uid, slot, is_sys, rel parts) of savedata/<uid>/CUSA00223/<slot>[.sce_sys]/<file>, tolerating
    up to two wrapper folders in front; None for anything else."""
    for lead in range(0, 3):
        rest = parts[lead:]
        if len(rest) >= 5 and rest[0] == 'savedata':
            uid, serial, slotdir = rest[1], rest[2], rest[3]
            is_sys = slotdir.endswith('.sce_sys')
            slot = slotdir[:-8] if is_sys else slotdir
            if serial in SERIALS and UID_RE.fullmatch(uid) and SLOT_RE.fullmatch(slot):
                return uid if serial == SERIAL else f'{uid}/{serial}', slot, is_sys, rest[4:]
    return None


def plan_from_zip(path):
    """Validates a .zip and lists its saves; nothing is written. Raises SaveError: zip_invalid (not a
    zip / damaged), zip_unsafe (../, absolute path, link), zip_too_big, zip_empty (no saves in it)."""
    plan = Plan(Path(path), 'zip')
    try:
        with zipfile.ZipFile(path) as zf:
            infos = zf.infolist()
            if len(infos) > ZIP_MAX_FILES:
                raise SaveError('zip_too_big', str(len(infos)))
            for info in infos:
                parts = _safe_parts(info.filename)
                mode = info.external_attr >> 16
                if mode and stat.S_ISLNK(mode):
                    raise SaveError('zip_unsafe', info.filename)
                if info.is_dir():
                    continue
                if info.file_size > ZIP_MAX_BYTES or plan.total_bytes + info.file_size > ZIP_MAX_BYTES:
                    raise SaveError('zip_too_big', info.filename)
                hit = _classify(parts)
                if hit is None:
                    plan.ignored += 1
                    continue
                uid, slot, is_sys, rel = hit
                plan.add(uid, slot, is_sys, rel, info.filename, info.file_size)
    except zipfile.BadZipFile as error:
        raise SaveError('zip_invalid', str(error))
    except OSError as error:
        raise SaveError('zip_invalid', str(error))
    if not plan.slots:
        raise SaveError('zip_empty', str(path))
    return plan


def plan_from_folder(data_dir):
    """The saves of another data folder (an old install), as an import plan."""
    plan = Plan(Path(data_dir), 'dir')
    for uid, name, folder in userdata.slot_dirs(Path(data_dir) / 'savedata'):
        for is_sys, base in ((False, folder), (True, folder.with_name(name + '.sce_sys'))):
            for p in sorted(base.rglob('*')) if base.is_dir() else []:
                if p.is_file() and not p.is_symlink():
                    plan.add(uid, name, is_sys, list(p.relative_to(base).parts), p, p.stat().st_size)
    if not plan.slots:
        raise SaveError('zip_empty', str(data_dir))
    return plan


def default_target_uid(user_dir, plan):
    """The user id folder the imported saves go to: the zip's own, unless the local data has exactly
    one other user folder and the source has exactly one (a different id): then the local one."""
    local = sorted(p.name for p in (Path(user_dir) / 'savedata').iterdir() if p.is_dir()) \
        if (Path(user_dir) / 'savedata').is_dir() else []
    if len(plan.users) == 1 and len(local) == 1 and plan.users[0] != local[0]:
        return local[0]
    return None


def import_preview(user_dir, plan, target_uid=None):
    """[{'id', 'slot', 'exists', 'title', 'old_title', 'time'}]: what the import would add, and which
    saves it would replace (exists=True)."""
    existing = {s['id']: s for s in list_slots(user_dir)}
    out = []
    for (uid, slot), entry in sorted(plan.slots.items()):
        uid = _target_user(uid, target_uid)
        title = ''
        if entry['param'] is not None:
            try:
                title = display_title(read_param(_read_source(plan, entry['param'])))
            except (OSError, KeyError, zipfile.BadZipFile):
                title = ''
        sid = f'{uid}/{slot}'
        old = existing.get(sid)
        out.append({'id': sid, 'slot': slot, 'exists': old is not None, 'title': title or slot,
                    'old_title': old['title'] if old else '', 'old_time': old['time'] if old else 0})
    return out


def _read_source(plan, src):
    if plan.kind == 'zip':
        with zipfile.ZipFile(plan.source) as zf:
            return zf.read(src)
    return Path(src).read_bytes()


def _write_member(plan, zf, src, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    if plan.kind == 'dir':
        shutil.copy2(src, dest)
        return
    written = 0
    with zf.open(src) as fin, open(dest, 'wb') as fout:
        while chunk := fin.read(1 << 16):
            written += len(chunk)
            if written > ZIP_MAX_BYTES:
                raise SaveError('zip_too_big', src)
            fout.write(chunk)


def import_plan(user_dir, plan, replace=(), target_uid=None, running=None):
    """Writes the plan's saves into the data folder. Saves that already exist are skipped unless their
    id (<uid>/<slot>) is in `replace`; replacing makes the safety backup first. Everything is
    extracted to a staging folder first and renamed into place per slot. Returns
    {'imported': [...], 'replaced': [...], 'skipped': [...], 'backup': result|None}."""
    if (running or game_running)():
        raise SaveError('save_game_running')
    user_dir = Path(user_dir)
    replace = set(replace)
    preview = {p['id']: p for p in import_preview(user_dir, plan, target_uid)}
    todo, skipped = [], []
    for (uid, slot), entry in sorted(plan.slots.items()):
        sid = f'{_target_user(uid, target_uid)}/{slot}'
        if preview[sid]['exists'] and sid not in replace:
            skipped.append(sid)
        else:
            todo.append(((uid, slot), sid, entry))
    result = {'imported': [], 'replaced': [], 'skipped': skipped, 'backup': None}
    if not todo:
        return result
    if any(preview[sid]['exists'] for _, sid, _ in todo):
        result['backup'] = backup_now(user_dir)      # nothing is replaced without this
    staging = user_dir / f'.importing-{os.getpid()}'
    try:
        user_dir.mkdir(parents=True, exist_ok=True)
        if userdata.free_bytes(user_dir) < plan.total_bytes * 2 + 1_000_000:
            raise SaveError('save_backup_no_space', str(user_dir))
        shutil.rmtree(staging, ignore_errors=True)
        zf = zipfile.ZipFile(plan.source) if plan.kind == 'zip' else None
        try:
            for (uid, slot), sid, entry in todo:
                for tag, suffix in (('dir', ''), ('sys', '.sce_sys')):
                    base = staging / sid / (slot + suffix)
                    base.mkdir(parents=True, exist_ok=True)
                    for rel, src in entry[tag]:
                        dest = base.joinpath(*rel)
                        if base.resolve() not in dest.resolve().parents:
                            raise SaveError('zip_unsafe', '/'.join(rel))
                        _write_member(plan, zf, src, dest)
        finally:
            if zf:
                zf.close()
        for (uid, slot), sid, entry in todo:
            game = slot_paths(user_dir, sid)[0].parent
            game.mkdir(parents=True, exist_ok=True)
            replaced = []
            try:
                for suffix in ('', '.sce_sys'):
                    new = staging / sid / (slot + suffix)
                    final = game / (slot + suffix)
                    if final.exists():
                        aside = game / f'.replaced-{slot}{suffix}-{os.getpid()}'
                        os.replace(final, aside)
                        replaced.append((final, aside))
                    if new.exists():
                        os.replace(new, final)
            except OSError as error:
                for final, aside in replaced:       # put the old save back
                    shutil.rmtree(final, ignore_errors=True)
                    os.replace(aside, final)
                raise SaveError('save_import_failed', str(error))
            for _, aside in replaced:
                shutil.rmtree(aside, ignore_errors=True)
            result['replaced' if replaced else 'imported'].append(sid)
    except OSError as error:
        raise SaveError('save_import_failed', str(error))
    finally:
        shutil.rmtree(staging, ignore_errors=True)
    return result


def export_zip(user_dir, dest, slot_ids=None):
    """Writes the saves (all, or the given slot ids) to a .zip with the layout
    savedata/<uid>/CUSA00223/<slot>[.sce_sys]/... Written to a temp name and renamed. Returns the
    number of saves exported."""
    dest = Path(dest)
    wanted = None if slot_ids is None else set(slot_ids)
    slots = [s for s in list_slots(user_dir) if wanted is None or s['id'] in wanted]
    if not slots:
        raise SaveError('zip_empty', str(user_dir))
    tmp = dest.with_name(dest.name + '.tmp')
    try:
        dest.parent.mkdir(parents=True, exist_ok=True)
        with zipfile.ZipFile(tmp, 'w', zipfile.ZIP_DEFLATED) as zf:
            for s in slots:
                folder, sys_dir = slot_paths(user_dir, s['id'])
                for base in (folder, sys_dir):
                    for p in sorted(base.rglob('*')) if base.is_dir() else []:
                        if p.is_file():
                            arc = '/'.join(('savedata', s['user'].split('/')[0], base.parent.name, base.name, *p.relative_to(base).parts))
                            zf.write(p, arc)
        os.replace(tmp, dest)
    except OSError as error:
        tmp.unlink(missing_ok=True)
        raise SaveError('save_export_failed', str(error))
    return len(slots)
