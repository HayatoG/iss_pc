# SPDX-License-Identifier: GPL-2.0-or-later
"""The QObject behind the "Saves and data" screen. All the work is done by userdata.py and
savemanager.py (no Qt there, with their own tests); this class only turns results into texts,
asks for confirmations and keeps the lists for the QML."""
import datetime
import os
import threading
from pathlib import Path

from PySide6.QtCore import Property, QObject, QTimer, QUrl, Signal, Slot

from .logic import savemanager, shader_cache, tlg_run, userdata


def _local(url):
    return QUrl(url).toLocalFile() if str(url).startswith('file:') else str(url)


class Saves(QObject):
    changed = Signal()
    foundScanned = Signal(object)
    dialogChanged = Signal()
    scrollRequested = Signal(int)   # used by the screenshot tool

    def __init__(self, backend, probes=None):
        """probes: optional callables replacing slow/system ones (tests, screenshots):
        'scan' -> list of found installs, 'running' -> bool, 'open' -> called with a path."""
        super().__init__(backend)
        self.backend = backend
        self.probes = probes or {}
        self._found = []
        self._scanning = False
        self._scanned = False
        self._msg, self._tone = '', 'info'
        self._dialog = {}
        self._handlers = {}
        self._running = False
        self._slots, self._backups = [], []
        self.foundScanned.connect(self._on_scanned)
        self.timer = QTimer(self)
        self.timer.setInterval(3000)
        self.timer.timeout.connect(self._poll_running)
        self.reload()

    # -- helpers -----------------------------------------------------------------------------
    def t(self, key, **fmt):
        return self.backend.t_(key, **fmt)

    def when(self, stamp):
        if not stamp:
            return '—'
        fmt = '%Y-%m-%d %H:%M' if self.backend.t_.key() == 'en' else '%d.%m.%Y %H:%M'
        return datetime.datetime.fromtimestamp(stamp).strftime(fmt)

    def count(self, n):
        return self.backend.t_.count(n)

    def data_dir(self):
        return tlg_run.user_dir()

    def say(self, text, tone='info'):
        self._msg, self._tone = text, tone
        self.changed.emit()

    def fail(self, error):
        if isinstance(error, userdata.DataError):
            self.say(self.t('err_' + error.key, detail=error.detail), 'bad')
        else:
            self.say(str(error), 'bad')

    def start_timer(self):
        self.timer.start()

    def _poll_running(self):
        now = self.is_running()
        if now != self._running:
            self._running = now
            self.changed.emit()

    def is_running(self):
        if 'running' in self.probes:
            return bool(self.probes['running']())
        return self.backend._play_state != 'idle' or savemanager.game_running()

    def running_now(self):
        """For QML bindings, which are read after every change: the answer of the 3 s timer, not a new
        look at the process list (the probe a test injects is always asked)."""
        if 'running' in self.probes:
            return bool(self.probes['running']())
        return self.backend._play_state != 'idle' or self._running

    def reload(self):
        data = self.data_dir()
        self._running = self.is_running()
        slots = savemanager.list_slots(data)
        self._slots = [{
            'id': s['id'], 'title': s['title'], 'slot': s['name'], 'kind': s['kind'],
            'kindText': self.t('slot_auto' if s['kind'] == 'auto' else 'slot_user'),
            'when': self.t('saves_modified', date=self.when(s['time'])), 'size': shader_cache.human_size(s['size']),
            'icon': QUrl.fromLocalFile(s['icon']).toString() if s['icon'] else ''} for s in slots]
        self._backups = [{
            'name': b['name'], 'when': self.when(b['time']),
            'info': self.t('backups_line', count=self.count(b['saves']), size=shader_cache.human_size(b['size']))}
            for b in savemanager.list_backups(data)]
        self.changed.emit()

    # -- properties --------------------------------------------------------------------------------
    @Property(str, notify=changed)
    def userDir(self):
        return str(self.data_dir())

    @Property(str, notify=changed)
    def sourceText(self):
        return self.t('saves_source_' + userdata.user_dir_source())

    @Property(str, notify=changed)
    def source(self):
        return userdata.user_dir_source()

    @Property(bool, notify=changed)
    def canChange(self):
        return userdata.user_dir_source() != 'env'

    @Property(str, notify=changed)
    def suggestion(self):
        return str(userdata.suggested_dir())

    @Property(bool, notify=changed)
    def running(self):
        return self._running

    @Property(bool, notify=changed)
    def hasSaves(self):
        return bool(self._slots)

    @Property('QVariantList', notify=changed)
    def slots(self):
        return self._slots

    @Property('QVariantList', notify=changed)
    def backups(self):
        return self._backups

    @Property(int, notify=changed)
    def keep(self):
        return savemanager.prefs()['keep']

    @Property(str, notify=changed)
    def extraDir(self):
        return savemanager.prefs()['extra']

    @Property(str, notify=changed)
    def message(self):
        return self._msg

    @Property(str, notify=changed)
    def messageTone(self):
        return self._tone

    @Property(bool, notify=changed)
    def scanning(self):
        return self._scanning

    @Property(bool, notify=changed)
    def scanned(self):
        return self._scanned

    def _others(self):
        here = self.data_dir()
        return [f for f in self._found if not userdata.same_path(f['path'], here)]

    @Property('QVariantList', notify=changed)
    def found(self):
        return [{'path': f['path'], 'text': self.t('saves_found_line', path=f['path'], count=self.count(f['saves']),
                                                    date=self.when(f['newest']))} for f in self._others()]

    @Property(str, notify=changed)
    def homeHint(self):
        """Shown on the Home screen: the current data folder has no saves but another place does."""
        others = self._others()
        if self._slots or not others:
            return ''
        f = others[0]
        return self.t('saves_home_hint', path=f['path'], count=self.count(f['saves']), date=self.when(f['newest']))

    @Property('QVariantMap', notify=dialogChanged)
    def dialog(self):
        return self._dialog

    # -- scan ----------------------------------------------------------------------------------------
    @Slot()
    def scan(self):
        if self._scanning:
            return
        self._scanning = True
        self.changed.emit()

        def work():
            if 'scan' in self.probes:
                found = self.probes['scan']()
            else:
                roots = userdata.candidate_roots(tlg_run.PORT_DIR)
                found = userdata.find_installs(roots, exclude=[self.data_dir()])
            self.foundScanned.emit(found)
        threading.Thread(target=work, daemon=True).start()

    def _on_scanned(self, found):
        self._found = found
        self._scanning, self._scanned = False, True
        self.changed.emit()

    # -- data folder -----------------------------------------------------------------------------------
    @Slot()
    def openFolder(self):
        path = self.data_dir()
        try:
            path.mkdir(parents=True, exist_ok=True)
        except OSError:
            pass
        if 'open' in self.probes:
            self.probes['open'](path)
        else:
            tlg_run.open_external(path)

    @Slot(str)
    def chooseDir(self, url):
        path = _local(url)
        if path:
            self._switch(Path(path).expanduser(), reset=False)

    @Slot()
    def useSuggested(self):
        self._switch(userdata.suggested_dir(), reset=False)

    @Slot()
    def resetDir(self):
        self._switch(userdata.default_dir(tlg_run.PORT_DIR), reset=True)

    def _switch(self, new, reset):
        if not self.canChange:
            self.say(self.t('saves_env_locked'), 'warn')
            return
        old = self.data_dir()
        if userdata.same_path(old, new):
            if reset:
                userdata.reset_user_dir()
                self._applied(new, None)
            return
        try:
            new.mkdir(parents=True, exist_ok=True)
            probe = new / '.write-test'
            probe.write_text('x')
            probe.unlink()
        except OSError as error:
            self.fail(userdata.DataError('data_not_writable', f'{new}: {error}'))
            return
        old_n = len(userdata.slot_dirs(old / 'savedata'))
        if old_n and not userdata.has_saves(new):
            self.ask(self.t('saves_migrate_title'), self.t('saves_migrate_body', count=self.count(old_n)),
                     [('copy', self.t('saves_copy'), 'primary'), ('nocopy', self.t('saves_dont_copy'), 'ghost'),
                      ('cancel', self.t('saves_cancel'), 'ghost')],
                     {'copy': lambda: self._apply(old, new, reset, True), 'nocopy': lambda: self._apply(old, new, reset, False)})
            return
        self._apply(old, new, reset, False)

    def _apply(self, old, new, reset, copy):
        result = None
        if copy:
            try:
                result = userdata.migrate(old, new)       # copy only; the old folder is untouched
            except (userdata.DataError, OSError) as error:
                self.fail(error if isinstance(error, userdata.DataError) else userdata.DataError('data_copy_failed', str(error)))
                return
        try:
            userdata.reset_user_dir() if reset else userdata.set_user_dir(new)
        except OSError as error:
            self.fail(userdata.DataError('data_not_writable', str(error)))
            return
        self._applied(new, result)

    def _applied(self, new, result):
        self.backend.reload_user()
        self.reload()
        if result:
            self.say(self.t('saves_dir_copied', path=str(new), items=', '.join(result['copied']) or '—',
                            size=shader_cache.human_size(result['bytes'])), 'good')
        elif userdata.has_saves(new):
            self.say(self.t('saves_dir_has_saves', path=str(new)), 'info')
        else:
            self.say(self.t('saves_dir_changed', path=str(new)), 'good')
        self.scan()

    # -- confirmations --------------------------------------------------------------------------------------
    def ask(self, title, body, buttons, handlers, lines=()):
        """Opens the confirmation card. buttons: [(id, label, tone)]; handlers: id -> callable."""
        self._handlers = handlers
        self._dialog = {'open': True, 'title': title, 'body': body, 'lines': list(lines),
                        'buttons': [{'id': i, 'label': l, 'tone': t} for i, l, t in buttons]}
        self.dialogChanged.emit()

    @Slot(str)
    def answer(self, button):
        handler = self._handlers.get(button)
        self._dialog, self._handlers = {}, {}
        self.dialogChanged.emit()
        if handler:
            handler()

    # -- backups -----------------------------------------------------------------------------------------------
    def _backup_note(self, result):
        if result and result.get('extra_error'):
            return ' ' + self.t('saves_extra_failed', detail=result['extra_error'])
        return ''

    @Slot()
    def backupNow(self):
        try:
            result = savemanager.backup_now(self.data_dir())
        except userdata.DataError as error:
            self.fail(error)
            return
        self.reload()
        if result['path'] is None:
            self.say(self.t('saves_backup_empty'), 'info')
        elif result['reused']:
            self.say(self.t('saves_backup_reused', name=result['path'].name) + self._backup_note(result), 'info')
        else:
            self.say(self.t('saves_backup_done', name=result['path'].name) + self._backup_note(result), 'good')

    @Slot(str)
    def askRestore(self, name):
        info = next((b for b in savemanager.list_backups(self.data_dir()) if b['name'] == name), None)
        if not info:
            return
        self.ask(self.t('saves_restore_title'),
                 self.t('saves_restore_body', date=self.when(info['time']), count=self.count(info['saves'])),
                 [('ok', self.t('saves_restore'), 'primary'), ('cancel', self.t('saves_cancel'), 'ghost')],
                 {'ok': lambda: self._restore(name)})

    def _restore(self, name):
        try:
            result = savemanager.restore_backup(self.data_dir(), name, running=self.is_running)
        except userdata.DataError as error:
            self.fail(error)
            return
        self.backend.refresh()
        self.reload()
        safety = result['path'].name if result['path'] else '—'
        self.say(self.t('saves_restored', name=safety) + self._backup_note(result), 'good')

    @Slot(int)
    def setKeep(self, n):
        savemanager.set_prefs(keep=n)
        self.changed.emit()

    @Slot(str)
    def chooseExtra(self, url):
        path = _local(url)
        if path:
            savemanager.set_prefs(extra=path)
            self.changed.emit()

    @Slot()
    def clearExtra(self):
        savemanager.set_prefs(extra='')
        self.changed.emit()

    # -- zip / folders ------------------------------------------------------------------------------------------
    @Slot(str)
    def exportAll(self, url):
        self._export(_local(url), None)

    @Slot(str, str)
    def exportSlot(self, slot_id, url):
        self._export(_local(url), [slot_id])

    def _export(self, path, ids):
        if not path:
            return
        if not path.lower().endswith('.zip'):
            path += '.zip'
        try:
            n = savemanager.export_zip(self.data_dir(), path, ids)
        except userdata.DataError as error:
            self.fail(error)
            return
        self.say(self.t('saves_exported', count=self.count(n), path=path), 'good')

    @Slot(str)
    def importZip(self, url):
        path = _local(url)
        if not path:
            return
        try:
            plan = savemanager.plan_from_zip(path)
        except userdata.DataError as error:
            self.fail(error)
            return
        self._ask_import(plan)

    @Slot(int)
    def importFound(self, index):
        others = self._others()
        if not 0 <= index < len(others):
            return
        try:
            plan = savemanager.plan_from_folder(others[index]['path'])
        except userdata.DataError as error:
            self.fail(error)
            return
        self._ask_import(plan)

    def _ask_import(self, plan):
        data = self.data_dir()
        target = savemanager.default_target_uid(data, plan)
        preview = savemanager.import_preview(data, plan, target)
        new = [p for p in preview if not p['exists']]
        old = [p for p in preview if p['exists']]
        lines = [self.t('saves_line_new', title=p['title'], slot=p['slot']) for p in new]
        lines += [self.t('saves_line_replace', old=p['old_title'], date=self.when(p['old_time']), title=p['title'],
                         slot=p['slot']) for p in old]
        buttons, handlers = [], {}
        if old:
            if new:
                buttons.append(('new', self.t('saves_import_new_only'), 'primary'))
                handlers['new'] = lambda: self._import(plan, [], target)
            buttons.append(('all', self.t('saves_import_replace', n=len(old)), 'danger' if new else 'primary'))
            handlers['all'] = lambda: self._import(plan, [p['id'] for p in old], target)
        else:
            buttons.append(('all', self.t('saves_import_all'), 'primary'))
            handlers['all'] = lambda: self._import(plan, [], target)
        buttons.append(('cancel', self.t('saves_cancel'), 'ghost'))
        self.ask(self.t('saves_import_title'),
                 self.t('saves_import_body', new=self.count(len(new)), old=self.count(len(old))),
                 buttons, handlers, lines)

    def _import(self, plan, replace, target):
        try:
            result = savemanager.import_plan(self.data_dir(), plan, replace=replace, target_uid=target,
                                             running=self.is_running)
        except userdata.DataError as error:
            self.fail(error)
            return
        self.backend.refresh()
        self.reload()
        self.say(self.backend.t_.plural('saves_imported', len(result['imported']), r=len(result['replaced']),
                                        s=len(result['skipped'])) + self._backup_note(result['backup']), 'good')

    # -- single saves --------------------------------------------------------------------------------------------------
    @Slot(str)
    def duplicate(self, slot_id):
        try:
            new = savemanager.duplicate_slot(self.data_dir(), slot_id, running=self.is_running)
        except userdata.DataError as error:
            self.fail(error)
            return
        self.reload()
        self.say(self.t('saves_duplicated', slot=new.split('/', 1)[1]), 'good')

    @Slot(str)
    def askDelete(self, slot_id):
        info = next((s for s in self._slots if s['id'] == slot_id), None)
        if not info:
            return
        self.ask(self.t('saves_delete_title'), self.t('saves_delete_body', title=info['title'], slot=info['slot']),
                 [('ok', self.t('saves_delete'), 'danger'), ('cancel', self.t('saves_cancel'), 'ghost')],
                 {'ok': lambda: self._delete(slot_id)})

    def _delete(self, slot_id):
        try:
            backup = savemanager.delete_slot(self.data_dir(), slot_id, running=self.is_running)
        except userdata.DataError as error:
            self.fail(error)
            return
        self.backend.refresh()
        self.reload()
        name = backup['path'].name if backup and backup['path'] else '—'
        self.say(self.t('saves_deleted', name=name) + self._backup_note(backup), 'good')

    @Slot()
    def dismissMessage(self):
        self._msg = ''
        self.changed.emit()
