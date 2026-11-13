# SPDX-License-Identifier: GPL-2.0-or-later
"""The QObject behind the Support screen ("Report a problem" in three steps, known problems for the
GPU, links). The collecting, summarising and redacting is report.py (no Qt, own tests); this class
holds the wizard state and turns results into texts. Nothing leaves the PC unless the player
clicks: copy (clipboard), save .zip (a file the player picks) or open the issue page (browser)."""
import datetime
import threading
from pathlib import Path

from PySide6.QtCore import Property, QObject, QUrl, Signal, Slot
from PySide6.QtGui import QGuiApplication

from . import report
from .logic import savemanager, tlg_run


def _local(url):
    return QUrl(url).toLocalFile() if str(url).startswith('file:') else str(url)


class Support(QObject):
    changed = Signal()
    loaded = Signal(object)    # a worker thread finished: payload dict
    logLoaded = Signal(object)
    scrollRequested = Signal(int)   # used by the screenshot tool

    def __init__(self, backend, probes=None):
        """probes (tests, screenshots): 'info' -> report.gather dict, 'known' -> list of issues,
        'logs_dir' -> folder, 'clipboard' -> f(text), 'open_url' -> f(url), 'open_dir' -> f(path),
        'names' -> user names to redact."""
        super().__init__(backend)
        self.backend = backend
        self.probes = probes or {}
        self._info = None
        self._collecting = False
        self._issues = (self.probes['known']() if 'known' in self.probes else report.load_known_issues())
        self._logs = []
        self._log_index = -1
        self._log = None          # {'name','summary','text','count','cut'}
        self._busy_log = False
        self._step = 0
        self._description = ''
        self._message, self._tone = '', 'info'
        self._used_short = False
        self.loaded.connect(self._on_loaded)
        self.logLoaded.connect(self._on_log)
        self.list_logs()

    # -- helpers ---------------------------------------------------------------------------------
    def t(self, key, **fmt):
        return self.backend.t_(key, **fmt)

    def names(self):
        return self.probes['names']() if 'names' in self.probes else report.own_names()

    def os_key(self):
        return self.probes['os_key']() if 'os_key' in self.probes else None

    def logs_dir(self):
        return Path(self.probes['logs_dir']()) if 'logs_dir' in self.probes else tlg_run.LOG_DIR

    def say(self, text, tone='info'):
        self._message, self._tone = text, tone
        self.changed.emit()

    def list_logs(self):
        self._logs = report.list_logs(self.logs_dir())
        if self._logs and not (0 <= self._log_index < len(self._logs)):
            self._log_index = 0
        elif not self._logs:
            self._log_index = -1
            self._log = None

    # -- step 1: collection (thread) -------------------------------------------------------------
    @Slot()
    def collect(self):
        if self._collecting:
            return
        self._collecting = True
        self.changed.emit()

        def work():
            def missing():
                return tlg_run.cpu_missing_sets(lambda line: None)
            try:
                if 'info' in self.probes:
                    info = self.probes['info']()
                else:
                    probes = {'cpu': lambda: report.cpu_info(missing)}
                    info = report.gather((tlg_run.PORT_DIR, tlg_run.REPO_DIR), self.logs_dir(),
                                         self.backend.settings.get('game_dir', ''), savemanager.read_sfo, probes)
            except Exception as error:   # the screen still opens; the rows say what failed
                info = {'error': str(error)}
            self.loaded.emit(info)
        threading.Thread(target=work, daemon=True).start()

    def _on_loaded(self, info):
        self._collecting = False
        self._info = None if 'error' in info else info
        if self._logs and self._log is None:
            self.selectLog(self._log_index)
        self.changed.emit()

    def reload(self):
        """The language changed: texts are rebuilt from the same data."""
        self.changed.emit()
        if self._log:
            self._log['lines'] = report.summary_lines(self._log['summary'], self.t)

    @Property(bool, notify=changed)
    def collecting(self):
        return self._collecting

    @Property('QVariantList', notify=changed)
    def hwRows(self):
        if not self._info:
            return []
        return [{'label': a, 'value': b} for a, b in report.hardware_rows(self._info, self.t)]

    @Property(bool, notify=changed)
    def hwReady(self):
        return self._info is not None

    @Property('QVariantList', notify=changed)
    def known(self):
        gpu = self._info.get('gpu') if self._info else None
        lang = self.backend.t_.key()
        rows = []
        for issue in report.known_for(self._issues, gpu, self.os_key()):
            rows.append({'title': issue['title'][lang], 'what': issue['what'][lang], 'matched': issue['matched'],
                         'issues': ', '.join(f'#{n}' for n in issue.get('issues', []))})
        return rows

    @Property(int, notify=changed)
    def matchedCount(self):
        gpu = self._info.get('gpu') if self._info else None
        return sum(1 for i in report.known_for(self._issues, gpu, self.os_key()) if i['matched'])

    @Property(str, notify=changed)
    def gpuName(self):
        gpu = self._info.get('gpu') if self._info else None
        return gpu['name'] if gpu else ''

    # -- step 2: the log ---------------------------------------------------------------------------
    @Property('QVariantList', notify=changed)
    def logs(self):
        en = self.backend.t_.key() == 'en'
        rows = []
        for n, path in enumerate(self._logs[:30]):
            try:
                stat = path.stat()
            except OSError:
                continue
            when = datetime.datetime.fromtimestamp(stat.st_mtime).strftime('%Y-%m-%d %H:%M' if en else '%d.%m.%Y %H:%M')
            rows.append({'name': path.name, 'when': when, 'size': f'{stat.st_size / 2 ** 20:.1f} MB' if stat.st_size > 2 ** 20
                         else f'{stat.st_size / 1024:.0f} KB', 'selected': n == self._log_index,
                         'latest': n == 0})
        return rows

    @Property(int, notify=changed)
    def logIndex(self):
        return self._log_index

    @Property(bool, notify=changed)
    def busyLog(self):
        return self._busy_log

    @Slot(int)
    def selectLog(self, index):
        if not (0 <= index < len(self._logs)):
            return
        self._log_index = index
        self._busy_log = True
        self._log = None
        self.changed.emit()
        path = self._logs[index]
        names = self.names()

        def work():
            try:
                text, cut = report.read_log(path)
                redacted, count = report.redact(text, names)
                self.logLoaded.emit({'name': path.name, 'summary': report.summarize_log(redacted), 'text': redacted,
                                     'count': count, 'cut': cut, 'path': str(path), 'index': index})
            except OSError as error:
                self.logLoaded.emit({'name': path.name, 'error': str(error), 'index': index})
        threading.Thread(target=work, daemon=True).start()

    def _on_log(self, payload):
        if payload.get('index') != self._log_index:
            return    # the player already picked another one
        self._busy_log = False
        if 'error' in payload:
            self._log = None
            self.say(self.t('sup_log_error', detail=payload['error']), 'bad')
            return
        payload['lines'] = report.summary_lines(payload['summary'], self.t)
        self._log = payload
        self._message = ''
        self.changed.emit()

    @Property(str, notify=changed)
    def summaryText(self):
        if self._log:
            return '\n'.join(report.summary_lines(self._log['summary'], self.t))
        return self.t('sup_no_logs') if not self._logs else ''

    @Property(str, notify=changed)
    def redactionNote(self):
        if not self._log:
            return ''
        note = self.t('sup_redacted', n=self._log['count'])
        return note + (' ' + self.t('sup_log_cut') if self._log.get('cut') else '')

    @Property(bool, notify=changed)
    def hasLogs(self):
        return bool(self._logs)

    # -- step 3: output ----------------------------------------------------------------------------
    @Slot(str)
    def setDescription(self, text):
        self._description = text
        self.changed.emit()

    @Property(str, notify=changed)
    def description(self):
        return self._description

    def build_report(self, short=False):
        rows = report.hardware_rows(self._info, self.t) if self._info else []
        summary = report.summary_lines(self._log['summary'], self.t) if self._log else []
        gpu = self._info.get('gpu') if self._info else None
        known = report.known_for(self._issues, gpu, self.os_key())
        for k in known:
            k['title'] = k['title'][self.backend.t_.key()]
        if short:
            text = report.short_report(rows, summary, self.t)
            if self._description.strip():
                text = self._description.strip() + '\n\n' + text
        else:
            text = report.markdown_report(rows, summary, self._log['name'] if self._log else '', known,
                                          self._description, self.t)
        return report.redact(text, self.names())[0]

    @Property(str, notify=changed)
    def reportText(self):
        return self.build_report()

    @Property(bool, notify=changed)
    def reportIsLong(self):
        return len(self.build_report()) > report.BODY_LIMIT

    @Property(str, notify=changed)
    def defaultZipName(self):
        return f'iss-report-{datetime.datetime.now():%Y%m%d-%H%M}.zip'

    @Property(str, notify=changed)
    def defaultZipUrl(self):
        """file:// URL of <Downloads or home>/<default name>: where the save dialog starts."""
        from PySide6.QtCore import QStandardPaths
        folder = QStandardPaths.writableLocation(QStandardPaths.StandardLocation.DownloadLocation) or str(Path.home())
        return QUrl.fromLocalFile(str(Path(folder) / self.defaultZipName)).toString()

    @Property(str, notify=changed)
    def message(self):
        return self._message

    @Property(str, notify=changed)
    def tone(self):
        return self._tone

    @Slot()
    def dismissMessage(self):
        self._message = ''
        self.changed.emit()

    @Slot()
    def copyReport(self):
        text = self.build_report()
        if 'clipboard' in self.probes:
            self.probes['clipboard'](text)
        else:
            QGuiApplication.clipboard().setText(text)
        self.say(self.t('sup_copied'), 'good')

    @Slot(str)
    def saveZip(self, url):
        path = _local(url)
        if not path:
            return
        if not path.lower().endswith('.zip'):
            path += '.zip'
        text = self.build_report()
        log_text = self._log['text'] if self._log else None
        left = report.leftover_home_paths((log_text or '') + text, self.names())
        if left:    # redaction missed something: do not write a file that leaks it
            self.say(self.t('sup_leak', n=len(left)), 'bad')
            return
        try:
            report.write_zip(path, text, log_text, self._log['name'] if self._log else '')
        except OSError as error:
            self.say(self.t('sup_zip_error', detail=str(error)), 'bad')
            return
        self.say(self.t('sup_zip_saved', path=path), 'good')

    def issue_title(self):
        gpu = self._info.get('gpu') if self._info else None
        return self.t('rep_issue_title', gpu=gpu['name'] if gpu else self.t('rep_unknown'))

    @Slot()
    def openIssue(self):
        url, short = report.issue_url(self.issue_title(), self.build_report(), self.build_report(short=True))
        self._used_short = short
        self._open(url)
        self.say(self.t('sup_issue_short' if short else 'sup_issue_opened'), 'warn' if short else 'good')

    def _open(self, url):
        if 'open_url' in self.probes:
            self.probes['open_url'](url)
        else:
            tlg_run.open_external(url)

    @Slot(str)
    def openLink(self, name):
        if name in report.URLS:
            self._open(report.URLS[name])

    @Slot()
    def openLogsFolder(self):
        folder = self.logs_dir()
        if 'open_dir' in self.probes:
            self.probes['open_dir'](folder)
            return
        folder.mkdir(parents=True, exist_ok=True)
        tlg_run.open_external(folder)

    @Slot()
    def refreshLogs(self):
        self.list_logs()
        self.changed.emit()
        if self._logs:
            self.selectLog(self._log_index)

    # -- wizard position ---------------------------------------------------------------------------
    @Property(int, notify=changed)
    def step(self):
        return self._step

    @Slot(int)
    def setStep(self, n):
        self._step = max(0, min(2, n))
        if self._step == 1 and self._logs and self._log is None and not self._busy_log:
            self.selectLog(max(self._log_index, 0))
        self.changed.emit()
