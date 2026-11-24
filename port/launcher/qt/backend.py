# SPDX-License-Identifier: GPL-2.0-or-later
"""The QObject the QML screens talk to. All real work is delegated to the modules the Tk
launcher uses (infamous_run: prepare/launch/build_env/save_experimental; backports_table)."""
import datetime
import os
import threading
from pathlib import Path

from PySide6.QtCore import (Property, QAbstractListModel, QCoreApplication, QModelIndex, QObject, Qt, QTimer, QUrl,
                            Signal, Slot)

from . import logic
from .saves import Saves
from .support import Support
from .updates import Updates
from .logic import backports_table, tlg_run
import advanced_table  # noqa: E402
import logtail  # noqa: E402
import precompile  # noqa: E402
import userdata  # noqa: E402
import trophies  # noqa: E402

LOG_LINES = 500           # lines the log panel keeps
LOG_POLL_MS = 1000        # how often the game's log file is read for the panel
CLOSE_DELAY_MS = 2000     # 'Close the launcher when the game starts': wait after the start

URLS = {'channel': 'https://www.youtube.com/@hayatog-dev', 'discord': 'https://discord.gg/UMkdMYZh9g',
        'sponsor': 'https://github.com/sponsors/HayatoG',
        'coffee': 'https://buymeacoffee.com/hayatog',
        'apolar': 'https://github.com/sapoguruoficial-cmd'}

# Sentences the Mods page leaves out of a mod's note (2026-11-23): which game version it needs (the
# list only shows mods for the player's version anyway) and "applies on the next launch".
_MOD_NOTE_DROP = (
    r'\s*(?:CUSA\d{5}(?:/CUSA\d{5})?\s+)?(?:update\s+)?1\.07\s+only\.',
    r'\s*Requires an? (?:verified )?CUSA\d{5}(?:/CUSA\d{5})?\s+(?:update\s+)?1\.07(?: build| update)?\.',
    r'\s*Applies on (?:the )?next (?:game )?(?:launch|start)[^.]*\.',
    r'\s*Takes effect (?:on|at) (?:the )?next (?:game )?(?:launch|start)[^.]*\.',
)


# Portuguese (Brazil) name and note of the mods that come with the port (2026-11-24), by their English
# name in the XML. Mods the player adds keep their own text.
MOD_TEXT_PT = {
    'God mode': ('Modo deus', 'Delsin não sofre dano: os golpes não tiram mais vida dele.'),
    'Unlimited energy': ('Energia infinita',
                         'Usar poderes, incluindo os tiros carregados, não gasta mais energia. Absorver ainda '
                         'recarrega. Encha a energia numa fonte de poder antes.'),
    'Unlimited missiles': ('Mísseis infinitos',
                           'Mísseis e bombas nunca acabam: o contador fica sempre em 5. A roda de poderes '
                           'continua funcionando (ela para de contar os mísseis de cada poder enquanto isto '
                           'estiver ligado).'),
    'Endless video dash': ('Video dash sem fim',
                           'O video dash continua voando enquanto você segurar o botão, como o dash infinito '
                           'do neon. Soltar o botão termina como sempre.'),
    'Skip intro movie': ('Pular vídeo de abertura',
                         'Volta na hora do início do vídeo de abertura. Pula o vídeo dos logotipos antes da '
                         'tela de título; as cenas da história não mudam.'),
    'Disable motion blur': ('Desligar desfoque de movimento',
                            'Desliga o desfoque de movimento da câmera pelo próprio caminho do jogo que o '
                            'desativa; para voltar, desligue e reinicie. Não remove a neblina nem a '
                            'profundidade de campo.'),
    'Camera improvements (1.5x + slowdown fix)': (
        'Melhorias de câmera (1,5x + correção da lentidão)',
        'Aumenta em 50% os limites de velocidade da câmera na horizontal e na vertical e evita a lentidão '
        'na diagonal nos limites verticais com o zoom desligado. A mira com zoom e os limites verticais '
        'não mudam.'),
}


def mod_text(name, note, portuguese):
    """(name, note) in the launcher language: the Portuguese ones for the port's own mods."""
    if portuguese and name in MOD_TEXT_PT:
        return MOD_TEXT_PT[name]
    return name, note


def clean_mod_note(note):
    """A mod's note without the version and next-launch sentences."""
    import re
    for pattern in _MOD_NOTE_DROP:
        note = re.sub(pattern, '', note, flags=re.IGNORECASE)
    return re.sub(r'\s{2,}', ' ', note).strip()


class FixesModel(QAbstractListModel):
    """Rows of the Fixes screen: group headers and one row per BPnn / A-B switch."""
    DATA = Qt.UserRole + 1

    def __init__(self, backend, scope='fixes'):
        super().__init__(backend)
        self.backend = backend
        self.scope = scope          # 'fixes' (BPnn + A/B switches) or 'adv' (the Advanced screen)
        self.filter = ''
        self.rows = []
        self.rebuild()

    def roleNames(self):
        return {self.DATA: b'd'}

    def rowCount(self, parent=QModelIndex()):
        return 0 if parent.isValid() else len(self.rows)

    def data(self, index, role=DATA):
        if role == self.DATA and 0 <= index.row() < len(self.rows):
            return self.rows[index.row()]
        return None

    def rebuild(self):
        t = self.backend.t_
        states = self.backend.fix_states
        rows = []
        if self.scope == 'adv':
            self._rebuild_advanced(t, states)
            return
        titles = {'light': 'fixes_group_light', 'perf': 'fixes_group_perf', 'tex': 'fixes_group_tex',
                  'exp': 'fixes_group_exp'}
        for group in logic.GROUP_ORDER:
            items = [i for i in logic.fix_items() if logic.group_of(i) == group]
            if not items:
                continue
            rows.append({'kind': 'header', 'id': 'hdr-' + group, 'title': t(titles[group]),
                         'note': t('fixes_group_exp_note') if group == 'exp' else ''})
            rows += [self.item_row(i, states) for i in items]
        self.beginResetModel()
        self.rows = rows
        self.endResetModel()

    def _rebuild_advanced(self, t, states):
        words = self.filter.lower().split()
        rows = []
        for area, title_en, title_pt in advanced_table.AREAS:
            items = [i for i in backports_table.ADVANCED if i.tab == area and self._matches(i, words, t)]
            if not items:
                continue
            rows.append({'kind': 'header', 'id': 'hdr-' + area, 'title': title_pt if t.pt() else title_en, 'note': ''})
            rows += [self.item_row(i, states) for i in items]
        self.beginResetModel()
        self.rows = rows
        self.endResetModel()

    @staticmethod
    def _matches(item, words, t):
        text = (item.id + ' ' + t.pair(item.name) + ' ' + t.pair(item.what)).lower()
        return all(w in text for w in words)

    def set_filter(self, text):
        self.filter = text
        self.rebuild()

    def item_row(self, item, states):
        t = self.backend.t_
        value = backports_table.effective(item, states)
        name = t.pair(item.name)
        what = t.pair(item.what)
        if item.source:
            what += ' (' + t('exp_source', source=item.source) + ')'
        tags = [{'text': t('exp_live') if item.live else t('exp_restart'), 'tone': 'info'}]
        if item.group == 'adv':
            tags.append({'text': item.env, 'tone': 'info'})
        if backports_table.is_recommended(item):
            tags.append({'text': t('exp_recommended'), 'tone': 'good'})
        if item.risky:
            tags.append({'text': t('exp_experimental'), 'tone': 'warn'})
        if item.broken:
            tags.append({'text': t('exp_broken'), 'tone': 'bad'})
        row = {'kind': 'item', 'id': item.id, 'code': item.id if item.group == 'bp' else '', 'label': name,
               'what': what, 'tags': tags, 'shaderWarn': t('fixes_shader_warn') if item.shader else '',
               'changed': states.get(item.id) is not None, 'control': 'switch', 'on': value == '1',
               'value': value if value is not None else 'auto', 'options': [], 'index': 0,
               'lo': 0, 'hi': 0, 'unit': ''}
        if item.vtype == 'int':
            row['control'] = 'number'
            row['value'] = '' if states.get(item.id) is None else states[item.id]
            row['lo'], row['hi'], row['unit'] = item.lo, item.hi, item.unit
            row['placeholder'] = t('adv_auto') if item.default is None else item.default
        elif not (item.vtype == 'bool' and item.default is not None):
            if item.vtype == 'bool':  # automatic: the GPU code decides
                options = [('auto', t('exp_auto')), ('1', t('exp_on')), ('0', t('exp_off'))]
            else:
                options = [(v, (pt if t.pt() else en)) for v, en, pt in item.choices]
            row['control'] = 'combo'
            row['options'] = [{'value': v, 'label': label} for v, label in options]
            row['index'] = next((n for n, (v, _) in enumerate(options) if v == row['value']), 0)
        return row

    def refresh_item(self, item_id):
        for n, row in enumerate(self.rows):
            if row['id'] == item_id:
                item = next(i for i in backports_table.ITEMS if i.id == item_id)
                self.rows[n] = self.item_row(item, self.backend.fix_states)
                self.dataChanged.emit(self.index(n), self.index(n))
                return

    def refresh_all(self):
        self.rebuild()


class Backend(QObject):
    langChanged = Signal()
    showAdvancedChanged = Signal()
    chipsChanged = Signal()
    stateChanged = Signal()
    gameDirChanged = Signal()
    logChanged = Signal()
    profileChanged = Signal()
    gameLanguageChanged = Signal()
    gpuFound = Signal(object)
    padFound = Signal(object)              # controller name or None (from the probing thread)
    lineReceived = Signal(str)
    jobsProgress = Signal(str, int, int)   # shader list: phase, done, total (from the play thread)
    cacheMeasured = Signal(object)         # bytes of the shader cache (from the measuring thread)
    finished = Signal(int)
    gameStarted = Signal(str)   # the log path; from the play thread right after the game was launched
    message = Signal(str)
    guideChanged = Signal()
    scrollRequested = Signal(int)   # used by the screenshot tool
    modsChanged = Signal()
    verifyChanged = Signal()
    verifyStep = Signal(object, object)     # verify game files: (done bytes, total) or (None, result) at the end

    def __init__(self, settings=None, translator=None, probes=None):
        """probes: optional dict of callables replacing the slow/hardware ones (tests, screenshots)."""
        super().__init__()
        self.langChanged.connect(self.gameLanguageChanged)   # 'Automatic: <name>' follows the interface language
        self.gameDirChanged.connect(self.gameLanguageChanged)   # the languages offered follow the game files
        self.settings = tlg_run.load_settings() if settings is None else settings
        self.t_ = translator or logic.Translator(self.settings)
        self.probes = probes or {}
        self.fonts = ('sans-serif', 'sans-serif')  # app.py puts the loaded families here
        self.fix_states = tlg_run.experimental_states(self.settings)
        self._gpu = 'checking'
        self._pad = None
        self._pad_scanning = False
        self._chips, self._ready = [], False
        self._play_state = 'idle'
        self._log = []
        self._news = []
        self._save = None
        self._message = ''
        self._guide_kind, self._guide_message = None, ''
        self.process = None
        self._cache_bytes = None   # None until the first measure ends; QML properties never walk the folder
        self._measuring = self._measure_again = False
        self.cacheMeasured.connect(self._on_cache_measured)
        self.fixes = FixesModel(self)
        self.advanced = FixesModel(self, 'adv')
        self.gpuFound.connect(self._on_gpu)
        self.padFound.connect(self._on_pad)
        self.lineReceived.connect(self._on_line)
        self.jobsProgress.connect(self._on_jobs_progress)
        self.precompiler = precompile.Precompiler()
        self._jobs = None        # (phase, done, total) while the shader list is being made
        self.finished.connect(self._on_finished)
        self.gameStarted.connect(self._on_game_started)
        self._verify_running, self._verify_stop = False, False
        self._verify_progress, self._verify_text, self._verify_level = -1.0, '', ''
        self.verifyStep.connect(self._on_verify_step)
        self._tail = None        # logtail.LogTail of the running game's log
        self._tail_timer = QTimer(self)
        self._tail_timer.setInterval(LOG_POLL_MS)
        self._tail_timer.timeout.connect(self._poll_log)
        self._trophies = []
        self.refresh()
        self.saves = Saves(self, self.probes.get('saves'))
        self.support = Support(self, self.probes.get('support'))
        self.updates = Updates(self, self.probes.get('updates'))

    # -- fonts and motion (set by app.py) ------------------------------------------------------
    @Property(str, constant=True)
    def fontDisplay(self):
        return self.fonts[0]

    @Property(str, constant=True)
    def fontText(self):
        return self.fonts[1]

    @Property(bool, constant=True)
    def reducedMotion(self):
        return bool(os.environ.get('ISS_QT_REDUCED_MOTION'))

    @Property(bool, constant=True)
    def logShown(self):
        return False

    # -- helpers -------------------------------------------------------------------------------
    def probe(self, name, default):
        return self.probes[name]() if name in self.probes else default()

    def start_probes(self):
        """GPU/Vulkan detection and the search for saves run off the UI thread."""
        self.saves.scan()
        self.saves.start_timer()
        self.support.collect()
        if self.settings.get('check_updates_on_open'):   # off by default: the only network call at start
            self.updates.check()
        def work():
            self.gpuFound.emit(self.probe('gpu', logic.detect_gpu))
        threading.Thread(target=work, daemon=True).start()
        self.rescan_pad()

    def rescan_pad(self):
        """Looks for a controller off the UI thread: `tlg-probe --gamepad` is a process that waits at
        least 300 ms when none is connected, and every click used to wait for it."""
        if 'pad' in self.probes or self._pad_scanning:
            return
        self._pad_scanning = True

        def work():
            try:
                self.padFound.emit(logic.detect_pad())
            finally:
                self._pad_scanning = False
        threading.Thread(target=work, daemon=True).start()

    def _on_pad(self, pad):
        if pad != self._pad:
            self._pad = pad
            self._compute_chips()
            self.chipsChanged.emit()
            self.stateChanged.emit()
            self.guideChanged.emit()   # the Controller guides page shows the detected controller

    def _on_gpu(self, gpu):
        self._gpu = gpu
        self.refresh()

    def refresh(self):
        if 'pad' in self.probes:   # a test or the selftest injects the answer: no thread
            self._pad = self.probes['pad']()
        self.measure_cache()
        self._compute_chips()
        self._news = self.probe('news', lambda: logic.news(self.t_, self.t_.pt()))
        savedata = tlg_run.user_dir() / 'savedata'
        self._save = self.probe('save', lambda: logic.last_save_title(savedata))
        self._load_trophies()
        self.chipsChanged.emit()
        self.stateChanged.emit()
        self.profileChanged.emit()
        self.gameDirChanged.emit()

    def _compute_chips(self):
        problems = self.probes['problems']() if 'problems' in self.probes else None
        self._chips, self._ready = logic.compute_chips(
            self.settings, self.t_, gpu=self._gpu, pad=self._pad, problems=problems,
            states=self.fix_states, cache_bytes=self._cache_bytes, measure_cache=False)

    # -- shader cache size ---------------------------------------------------------------------
    def measure_cache(self):
        """Sizes the shader cache off the UI thread. After the first start it holds tens of thousands of
        files: walking them on the UI thread at every state change (the Home chip, showFirstRun, each
        progress line of the shader list) froze the window on Windows, close-on-play timer included."""
        if 'cache' in self.probes:
            self._cache_bytes = self.probes['cache']()
            return
        if self._measuring:
            self._measure_again = True
            return
        self._measuring = True
        settings = dict(self.settings)
        def work():
            try:
                size = tlg_run.shader_cache_size(settings)
            except Exception:
                size = 0
            self.cacheMeasured.emit(size)
        threading.Thread(target=work, daemon=True).start()

    def _on_cache_measured(self, size):
        self._measuring = False
        if self._measure_again:   # the cache changed while it was measured (cleared, folder changed)
            self._measure_again = False
            self.measure_cache()
        if size == self._cache_bytes:
            return
        self._cache_bytes = size
        self._compute_chips()
        self.chipsChanged.emit()
        self.stateChanged.emit()

    def cache_bytes(self):
        """The last measured size; 0 while the first measure runs."""
        return self._cache_bytes or 0

    def reload_user(self):
        """The data folder changed: tlg.ini (fix switches) and the last save come from the new one."""
        self.fix_states = tlg_run.experimental_states(self.settings)
        self.fixes.refresh_all()
        self.profileChanged.emit()
        self.refresh()
        if hasattr(self, 'saves'):
            self.saves.reload()

    # -- text -----------------------------------------------------------------------------------
    @Slot(str, result=str)
    def t(self, key):
        return self.t_(key)

    @Slot(str, 'QVariantMap', result=str)
    def tf(self, key, args):
        return self.t_(key, **args)

    @Property(str, notify=langChanged)
    def lang(self):
        return 'pt_BR' if self.t_.pt() else 'en_US'

    # -- Advanced settings switch (2026-11-22): shows the Advanced and Fixes tabs; off by default -----
    @Property(bool, notify=showAdvancedChanged)
    def showAdvanced(self):
        return bool(self.settings.get('show_advanced_tabs', False))

    @Slot(bool)
    def setShowAdvanced(self, on):
        if bool(on) == self.showAdvanced:
            return
        self.settings['show_advanced_tabs'] = bool(on)
        tlg_run.save_settings(self.settings)
        self.showAdvancedChanged.emit()

    @Slot(str)
    def setLang(self, lang):
        self.settings['ui_language'] = lang
        tlg_run.save_settings(self.settings)
        self.langChanged.emit()
        self.modsChanged.emit()    # the port's own mods have Portuguese texts (2026-11-24)
        self.guideChanged.emit()
        self.fixes.refresh_all()
        self.refresh()
        if hasattr(self, 'saves'):
            self.saves.reload()   # its texts follow the language
        if hasattr(self, 'support'):
            self.support.reload()
            self.updates.changed.emit()

    # -- home state -----------------------------------------------------------------------------
    @Property('QVariantList', notify=chipsChanged)
    def chips(self):
        return self._chips

    @Property(bool, notify=stateChanged)
    def ready(self):
        return self._ready

    @Property(str, notify=stateChanged)
    def statusTitle(self):
        return self.t_('status_ready' if self._ready else 'status_attention')

    @Property('QVariantList', notify=stateChanged)
    def newsLines(self):
        return self._news

    @Property(str, notify=stateChanged)
    def version(self):
        return self.probe('version', logic.release_version)

    def _build_info(self):
        """SHAREABLE_BUILD.json next to the port (the package's own description), {} when absent."""
        if 'build_info' in self.probes:
            return self.probes['build_info']()
        try:
            import json
            return json.loads((tlg_run.PORT_DIR / 'SHAREABLE_BUILD.json').read_text(encoding='utf-8'))
        except (OSError, ValueError):
            return {}

    @Property(str, notify=stateChanged)
    def footerVersion(self):
        """The rail's version line (2026-11-23): this package's own build date as the version
        ('v2026.11.22', from SHAREABLE_BUILD.json, so it follows whatever build is installed); the
        release name from VERSION only when the package carries no date."""
        date = str(self._build_info().get('packaged_date') or '').strip()
        if date:
            return 'v' + date.replace('-', '.')
        return (self.version or '').split('-')[0]

    @Property(bool, notify=stateChanged)
    def isBeta(self):
        return 'beta' in str(self._build_info().get('package', '')).lower()

    @Property(str, notify=stateChanged)
    def heroKicker(self):
        game = tlg_run.GAMES[tlg_run.current_game()]
        serial = (logic.shader_cache.title_id(self.settings.get('game_dir', '')) or
                  game.get('titles', ('CUSA00223',))[0])
        return self.t_('hero_kicker', serial=serial, version=game['version'])

    @Property(str, notify=stateChanged)
    def heroTagline(self):
        return self.t_('hero_tagline_first_light' if tlg_run.current_game() == 'first_light' else 'hero_tagline')

    @Property(str, notify=stateChanged)
    def playSubtitle(self):
        if self._jobs:
            return self.jobsText
        if self._play_state == 'launching':
            return self.t_('play_launching')
        if self._play_state == 'running':
            return self.t_('play_running')
        if not self.canPlay:
            return self.t_('play_blocked')
        return self.t_('play_continue', save=self._save) if self._save else self.t_('play_new')

    @Property(str, notify=stateChanged)
    def jobsText(self):
        """'Indexing the game files… 42%' while the generator runs, else ''."""
        if not self._jobs:
            return ''
        phase, done, total = self._jobs
        text = precompile.message(precompile.PHASE_TEXT.get(phase, 'jobs_phase_other'), self.settings, phase=phase)
        return f'{text} {100 * done // total}%' if total else text

    @Property(float, notify=stateChanged)
    def jobsProgressValue(self):
        """0..1 of the generator's current phase, -1 when it is not running."""
        if not self._jobs:
            return -1.0
        _, done, total = self._jobs
        return min(1.0, done / total) if total else 0.0

    def _on_jobs_progress(self, phase, done, total):
        self._jobs = (phase, done, total)
        self.stateChanged.emit()

    @Property(bool, notify=stateChanged)
    def canPlay(self):
        return self._ready and self._play_state == 'idle'

    @Property(str, notify=stateChanged)
    def playState(self):
        return self._play_state

    @Property(bool, notify=stateChanged)
    def showFirstRun(self):
        empty = self._cache_bytes == 0   # unknown (still measuring) is not empty
        return empty or backports_table.notice_pending(self.settings)

    @Property(str, notify=gameDirChanged)
    def gameDir(self):
        return self.settings.get('game_dir', '')

    @Property(str, notify=gameDirChanged)
    def artSource(self):
        """pic1.png of the player's own dump (never shipped); empty when absent."""
        for name in ('pic1.png', 'pic0.png'):
            path = _art(self.settings, name)
            if path:
                return QUrl.fromLocalFile(str(path)).toString()
        return ''

    @Property(bool, notify=gameDirChanged)
    def artAnimated(self):
        """True when the Play tab shows an animated GIF (whole, not cropped)."""
        return play_art(self.settings)[1] == 'gif'

    @Property(bool, notify=gameDirChanged)
    def artWhole(self):
        """True when the Play tab's picture is shown whole on its own background colour ('paper'): a GIF,
        or the game's own picture (GAME_PICTURE)."""
        return play_art(self.settings)[1] in ('gif', 'whole')

    @Property(bool, notify=gameDirChanged)
    def artBackdrop(self):
        """True when the Play tab's picture fills the whole tab behind the cards and buttons (Second
        Son's pic0.png from the player's game folder)."""
        return play_art(self.settings)[1] == 'backdrop'

    @Property(str, notify=gameDirChanged)
    def artBackground(self):
        """A whole picture's background colour (fills the panel and the page around it); '' otherwise."""
        path, kind = play_art(self.settings)
        if kind == 'gif':
            return art_colours(path)[0]
        return paper_colour(path) if kind == 'whole' else ''

    @Property(str, notify=gameDirChanged)
    def artShade(self):
        """The colour under the Play buttons: the animated picture's smoke colour, else the drawn art's black."""
        path, kind = play_art(self.settings)
        return (art_colours(path)[1] if kind == 'gif' else '') or '#08080a'

    @Property(str, notify=logChanged)
    def logText(self):
        return '\n'.join(self._log[-LOG_LINES:])

    @Property(str, notify=stateChanged)
    def notice(self):
        return self._message

    # -- verify game files (full check of art/ against the reference list, off the UI thread) -------
    @Property(bool, notify=verifyChanged)
    def verifyRunning(self):
        return self._verify_running

    @Property(float, notify=verifyChanged)
    def verifyProgress(self):
        """0..1 while the check runs, -1 otherwise."""
        return self._verify_progress

    @Property(str, notify=verifyChanged)
    def verifyText(self):
        return self._verify_text

    @Property(str, notify=verifyChanged)
    def verifyLevel(self):
        """'ok' | 'warn' | 'bad' | '' (nothing to show)."""
        return self._verify_level

    @Slot()
    def verifyGame(self):
        folder = self.settings.get('game_dir', '')
        if self._verify_running or not folder:
            return
        self._verify_running, self._verify_stop = True, False
        self._verify_progress, self._verify_text, self._verify_level = 0.0, self.t_('verify_running'), ''
        self.verifyChanged.emit()

        def work():
            try:
                result = tlg_run.verify_game(folder, progress=lambda done, total: self.verifyStep.emit(done, total),
                                             stopped=lambda: self._verify_stop)
            except Exception as error:   # never leave the button stuck
                result = {'state': 'error', 'error': str(error)}
            self.verifyStep.emit(None, result)
        threading.Thread(target=work, daemon=True).start()

    @Slot()
    def stopVerify(self):
        self._verify_stop = True

    def _on_verify_step(self, done, value):
        if done is not None:
            if value:
                progress = done / value
                if progress - self._verify_progress >= 0.002 or progress >= 1:
                    self._verify_progress = progress
                    self.verifyChanged.emit()
            return
        r, t = value, self.t_
        self._verify_running, self._verify_progress = False, -1.0
        state = r.get('state')
        if state == 'ok':
            self._verify_level = 'ok'
            self._verify_text = t('verify_ok', files=f"{r['files']:,}", gb=f"{r['bytes'] / 2**30:.1f}")
        elif state == 'bad':
            bad = r['missing'] + r['size'] + r['content']
            names = ', '.join(bad[:3]) + (t('verify_more', n=len(bad) - 3) if len(bad) > 3 else '')
            self._verify_level = 'bad'
            self._verify_text = t('verify_bad', missing=len(r['missing']), size=len(r['size']),
                                  content=len(r['content']), names=names)
        elif state == 'stopped':
            self._verify_level, self._verify_text = 'warn', t('verify_stopped')
        elif state == 'other_title':
            self._verify_level, self._verify_text = 'warn', t('verify_other_title', title=r.get('title') or '?')
        elif state == 'nolist':
            self._verify_level, self._verify_text = 'warn', t('verify_nolist')
        else:
            self._verify_level, self._verify_text = 'bad', t('verify_error', error=r.get('error', ''))
        self.verifyChanged.emit()

    # -- game folder ----------------------------------------------------------------------------
    @Slot(str)
    def chooseFolder(self, url):
        path = QUrl(url).toLocalFile() if url.startswith('file:') else url
        if not path:
            return
        state, problems, _ = logic.game_state(self.settings, tlg_run.check_game(path) if 'problems' not in self.probes else [])
        blocking = tlg_run.blocking(problems)
        if blocking:
            self._message = self.t_('folder_rejected', why=logic.game_problem_text(self.t_, blocking))
        else:
            self.settings['game_dir'] = path
            tlg_run.save_settings(self.settings)
            self._message = self.t_('folder_saved')
            if not self._verify_running:   # an earlier result was about the old folder
                self._verify_text, self._verify_level = '', ''
                self.verifyChanged.emit()
        self.refresh()

    @Slot()
    def clearNotice(self):
        self._message = ''
        self.stateChanged.emit()

    @Slot(str)
    def openUrl(self, name):
        tlg_run.open_external(URLS[name])

    # -- play (same path as the Tk launcher: prepare() then launch()) -------------------------------
    @Slot()
    def play(self):
        if self.updates.busy:
            return
        if self._play_state != 'idle' or not self._ready:
            return
        tlg_run.save_settings(self.settings)
        if backports_table.notice_pending(self.settings):
            backports_table.mark_notice_seen(self.settings)
            tlg_run.save_settings(self.settings)
        self._play_state = 'launching'
        self.precompiler = precompile.Precompiler()
        self._log = [self.t_('preparing')]
        self.logChanged.emit()
        self.stateChanged.emit()
        threading.Thread(target=self._run_game, daemon=True).start()

    def _run_game(self):
        try:
            tlg_run.prepare(self.settings, log=lambda line: self.lineReceived.emit(line))
            outcome = precompile.prepare_jobs(self.settings, log=lambda line: self.lineReceived.emit(line),
                                              progress=lambda *args: self.jobsProgress.emit(*args),
                                              precompiler=self.precompiler)
            self._jobs = None
            if outcome == 'cancelled':   # window closed or Quit in the middle: no half file, no game
                self.lineReceived.emit(self.t_('jobs_cancelled'))
                self.finished.emit(-1000)
                return
            self.process, log = tlg_run.launch(self.settings)
        except tlg_run.GameError as error:
            self.lineReceived.emit(self.t_(error.key, detail=error.detail))
            self.finished.emit(-1000)
            return
        except Exception as error:  # shown to the player; the launcher keeps running
            self._jobs = None
            self.lineReceived.emit(str(error))
            self.finished.emit(-1000)
            return
        self.lineReceived.emit(f"{self.t_('running')} {log}")
        self._play_state = 'running'
        self.stateChanged.emit()
        self.gameStarted.emit(str(log))
        code = self.process.wait()   # the game writes its own log file; here only the exit code
        self.process = None
        self.finished.emit(code)

    def _on_game_started(self, log):
        """Main thread: follow the log file once a second and, if asked, close the launcher."""
        self._tail = logtail.LogTail(log, LOG_LINES)
        self._tail_timer.start()
        if self.settings.get('close_on_play'):
            QTimer.singleShot(CLOSE_DELAY_MS, self._quit_app)

    def _quit_app(self):
        process = self.process
        if self.settings.get('reopen_launcher', True) and process is not None and process.poll() is None:
            tlg_run.reopen_after(process.pid)
        app = QCoreApplication.instance()
        if app is None:
            return
        for window in getattr(app, 'topLevelWindows', lambda: [])():   # gone at once, even if the exit is slow
            window.hide()
        app.quit()

    def _poll_log(self, final=False):
        """Reads the bytes the game appended; the panel is rebuilt once per tick, never per line."""
        if self._tail is None:
            return
        new = self._tail.poll(final=final)
        if new:
            self._log.extend(new)
            del self._log[:-LOG_LINES]
            self.logChanged.emit()

    def _on_line(self, line):
        self._log.append(line)
        del self._log[:-LOG_LINES]
        self.logChanged.emit()

    def _on_finished(self, code):
        self._play_state = 'idle'
        self._tail_timer.stop()
        self._poll_log(final=True)
        self._tail = None
        if code == -1000:
            pass
        else:
            self._log.append(self.t_('ended' if code in (0, -15) else 'crashed', code=code))
            self.logChanged.emit()
            fps_item = self._menu_item('fps_limit')
            self.fix_states[fps_item.id] = tlg_run.experimental_states(self.settings).get(fps_item.id)
        self.refresh()

    def shutdown(self):
        # The game is an independent process: closing the launcher does not close it.
        self.precompiler.cancel()
        self._tail_timer.stop()

    # -- fixes ----------------------------------------------------------------------------------
    @Property(QObject, constant=True)
    def savesController(self):
        return self.saves

    @Property(QObject, constant=True)
    def fixesModel(self):
        return self.fixes

    @Property(QObject, constant=True)
    def advancedModel(self):
        return self.advanced

    @Property(str, notify=profileChanged)
    def advancedText(self):
        n = backports_table.advanced_changes(self.fix_states)
        return self.t_('adv_profile_default') if n == 0 else self.t_('adv_profile_custom', n=n)

    @Property(bool, notify=profileChanged)
    def advancedIsDefault(self):
        return backports_table.advanced_changes(self.fix_states) == 0

    @Slot(str)
    def setAdvancedFilter(self, text):
        self.advanced.set_filter(text)

    @Slot()
    def copyCombination(self):
        """Every ISS_ variable the player's choices add, one per line, for pasting into an issue."""
        from PySide6.QtGui import QGuiApplication
        env = backports_table.environment(self.fix_states, {})
        text = '\n'.join(f'{k}={v}' for k, v in sorted(env.items())) or '(defaults)'
        app = QGuiApplication.instance()
        if app is not None:
            app.clipboard().setText(text)
        self.message.emit(self.t_('adv_copied'))

    @Slot()
    def resetAdvanced(self):
        self.fix_states = backports_table.defaults(self.fix_states, 'adv')
        self._save_fixes()
        self.advanced.refresh_all()
        self.refresh()

    # -- mods/cheats -----------------------------------------------------------------------------
    @Property('QVariantList', notify=modsChanged)
    def modsModel(self):
        """Every shadPS4/GoldHEN cheat XML found in the mods folder for this game's version,
        with its current enabled state. Nothing here has any effect until turned on."""
        try:
            mods = tlg_run.list_mods(self.settings)
        except Exception:
            mods = []
        portuguese = self.t_.pt()
        rows = []
        for key, name, note, enabled in mods:
            shown, text = mod_text(name or key, clean_mod_note(note or ''), portuguese)
            rows.append({'key': key, 'name': shown, 'note': text, 'on': bool(enabled)})
        return rows

    @Slot(str, bool)
    def setModEnabled(self, key, enabled):
        """Saves the switch. The list is not rebuilt (the switch shows its new state itself): a rebuilt
        list started again at the top, so every toggle scrolled the Mods page all the way up."""
        tlg_run.set_mod_enabled(key, enabled)

    @Slot()
    def refreshMods(self):
        self.modsChanged.emit()

    @Slot()
    def openModsFolder(self):
        tlg_run.open_external(tlg_run.mods_dir())

    @Property(str, notify=profileChanged)
    def profileText(self):
        n = backports_table.changes(self.fix_states)
        return self.t_('exp_profile_rec') if n == 0 else self.t_('exp_profile_custom', n=n)

    @Property(bool, notify=profileChanged)
    def profileIsRecommended(self):
        return backports_table.changes(self.fix_states) == 0

    @Property(bool, notify=profileChanged)
    def anyShaderChange(self):
        return any(self.fix_states.get(i.id) is not None and i.shader for i in logic.fix_items())

    def _occlusion_item(self):
        return next(i for i in backports_table.ITEMS if i.key == 'occlusion_mode')

    @Property('QVariantList', notify=profileChanged)
    def occlusionOptions(self):
        """The values of the "Light glare (occlusion)" selector on the Home screen."""
        item = self._occlusion_item()
        value = backports_table.effective(item, self.fix_states)
        return [{'value': v, 'label': self.t_.pair((en, pt)), 'on': v == value} for v, en, pt in item.choices]

    @Property(int, notify=profileChanged)
    def occlusionIndex(self):
        return next((n for n, o in enumerate(self.occlusionOptions) if o['on']), 0)

    @Slot(str)
    def setOcclusion(self, value):
        self.setFix(self._occlusion_item().id, value)

    def _verbose_log_item(self):
        return next(i for i in backports_table.ITEMS if i.key == 'verbose_log')

    @Property(bool, notify=profileChanged)
    def verboseLog(self):
        """The "Detailed log" switch on the Home screen, under the light glare selector."""
        return backports_table.effective(self._verbose_log_item(), self.fix_states) == '1'

    @Slot(bool)
    def setVerboseLog(self, on):
        self.setFix(self._verbose_log_item().id, '1' if on else '0')

    # -- upscaler (Home screen): one selector over fsr1 / iss_temporal / iss_temporal_backend ----------------
    _UPSCALER_KEYS = ('fsr1', 'iss_temporal', 'iss_temporal_backend', 'iss_temporal_mode')

    def _menu_item(self, key):
        return next(i for i in backports_table.ITEMS if i.key == key)

    def _menu_value(self, key):
        return backports_table.effective(self._menu_item(key), self.fix_states)

    @Property('QVariantList', notify=profileChanged)
    def fpsLimitOptions(self):
        item = self._menu_item('fps_limit')
        return [{'value': v, 'label': self.t_.pair((en, pt)) if v == '0' else f'{v} FPS'}
                for v, en, pt in item.choices]

    @Property(int, notify=profileChanged)
    def fpsLimitIndex(self):
        value = self._menu_value('fps_limit')
        return next(i for i, option in enumerate(self.fpsLimitOptions) if option['value'] == value)

    @Slot(str)
    def setFpsLimit(self, value):
        item = self._menu_item('fps_limit')
        if any(value == choice[0] for choice in item.choices):
            self.setFix(item.id, value)

    def _upscaler_choice(self):
        """0 off, 1 FSR 1, 2 FSR 3.1 temporal, 3 NVIDIA DLSS temporal."""
        if self._menu_value('iss_temporal') == '1':
            return 3 if self._menu_value('iss_temporal_backend') == '1' else 2
        return 1 if self._menu_value('fsr1') == '1' else 0

    @Property('QVariantList', notify=profileChanged)
    def upscalerOptions(self):
        names = (('Off', 'Desligado'), ('FSR 1 (spatial)', 'FSR 1 (espacial)'),
                 ('FSR 3.1 (temporal)', 'FSR 3.1 (temporal)'), ('NVIDIA DLSS (temporal)', 'NVIDIA DLSS (temporal)'))
        return [{'value': str(n), 'label': self.t_.pair(pair)} for n, pair in enumerate(names)]

    @Property(int, notify=profileChanged)
    def upscalerIndex(self):
        return self._upscaler_choice()

    @Slot(str)
    def setUpscaler(self, value):
        """Writes the three switches behind the selector; a temporal one also needs an output resolution."""
        pick = int(value)
        self.fix_states[self._menu_item('fsr1').id] = backports_table.normalize(self._menu_item('fsr1'), '1' if pick == 1 else '0')
        self.fix_states[self._menu_item('iss_temporal').id] = backports_table.normalize(self._menu_item('iss_temporal'), '1' if pick >= 2 else '0')
        if pick >= 2:
            self.fix_states[self._menu_item('iss_temporal_backend').id] = backports_table.normalize(
                self._menu_item('iss_temporal_backend'), '1' if pick == 3 else '0')
            if self._menu_value('iss_temporal_mode') == '0':
                self.fix_states[self._menu_item('iss_temporal_mode').id] = backports_table.normalize(
                    self._menu_item('iss_temporal_mode'), '1')   # Quality, the usual choice
        self._save_fixes()
        for key in self._UPSCALER_KEYS:
            self.fixes.refresh_item(self._menu_item(key).id)
            self.advanced.refresh_item(self._menu_item(key).id)
        self.refresh()

    @Property(bool, notify=profileChanged)
    def upscalerHasQuality(self):
        return self._upscaler_choice() >= 2

    @Property('QVariantList', notify=profileChanged)
    def upscalerQualityOptions(self):
        item = self._menu_item('iss_temporal_mode')
        return [{'value': v, 'label': self.t_.pair((en, pt))} for v, en, pt in item.choices if v != '0']

    @Property(int, notify=profileChanged)
    def upscalerQualityIndex(self):
        value = self._menu_value('iss_temporal_mode')
        return max(0, int(value) - 1)

    @Slot(str)
    def setUpscalerQuality(self, value):
        self.setFix(self._menu_item('iss_temporal_mode').id, value)

    # -- game language (voice and text; the interface language above is separate) ---------------
    @Property('QVariantList', notify=gameLanguageChanged)
    def gameLanguageOptions(self):
        """'Automatic' first, then the languages of infamous_run.game_language_choices (those the game files have)."""
        return [{'value': k, 'label': self.t_('game_language_auto') if k == 'auto'
                 else logic.GAME_LANGUAGE_NAMES.get(k, k)} for k in tlg_run.game_language_choices(self.settings)]

    @Property(int, notify=gameLanguageChanged)
    def gameLanguageIndex(self):
        keys = tlg_run.game_language_choices(self.settings)
        return keys.index(self.settings.get('game_language')) if self.settings.get('game_language') in keys else 0

    @Property(str, notify=gameLanguageChanged)
    def gameLanguageNow(self):
        """'Automatic: Português (Brasil)' while 'auto' is active, else empty."""
        if self.settings.get('game_language', 'auto') in tlg_run.GAME_LANGUAGES:
            return ''
        return self.t_('game_language_now', name=logic.GAME_LANGUAGE_NAMES[tlg_run.resolve_game_language(self.settings)])

    @Slot(str)
    def setGameLanguage(self, key):
        if key != 'auto' and key not in tlg_run.GAME_LANGUAGES:
            return
        self.settings['game_language'] = key   # build_env reads it at the next start (ISS_LANGUAGE)
        tlg_run.save_settings(self.settings)
        self.gameLanguageChanged.emit()

    # -- trophies showcase (Play tab; the port tracks Second Son's 48 and First Light's 25) ---------
    def _load_trophies(self):
        rows = []
        game = tlg_run.current_game()
        if game in trophies.GAMES and self.settings.get('game_dir') and 'trophies' not in self.probes:
            try:
                rows = trophies.showcase(self.settings['game_dir'], tlg_run.user_dir(), game)
            except Exception:   # never stops the launcher: the card just stays hidden
                rows = []
        elif 'trophies' in self.probes:
            rows = self.probes['trophies']()
        if os.environ.get('ISS_TROPHY_PREVIEW') == 'all':
            # Display test (2026-11-22): shows every trophy as earned to check the look. Only what the
            # launcher shows changes; nothing is written to the saves or the trophy record.
            now = datetime.datetime.now().timestamp()
            for row in rows:
                row.update(earned=True, time=row.get('time') or now, recovered=False)
        portuguese = self.t_.pt()
        for row in rows:
            # The requirement line follows the launcher language (2026-11-24); names stay the game's own.
            if game in trophies.GAMES and isinstance(row.get('id'), int):
                english = trophies.GAMES[game][2][row['id']][1] if row['id'] < len(trophies.GAMES[game][2]) else row['requirement']
                row['requirement'] = trophies.requirement(game, row['id'], english, portuguese)
            if row['earned'] and row['recovered']:
                row['status'] = self.t_('trophy_recovered')
            elif row['earned'] and row['time']:
                stamp = datetime.datetime.fromtimestamp(row['time']).strftime('%Y-%m-%d %H:%M')
                row['status'] = self.t_('trophy_earned_on', date=stamp)
            elif row['earned']:
                row['status'] = self.t_('trophy_earned')
            else:
                row['status'] = self.t_('trophy_not_earned')
            row['icon'] = QUrl.fromLocalFile(row['icon']).toString() if row.get('icon') else ''
        self._trophies = rows

    @Property('QVariantList', notify=stateChanged)
    def trophies(self):
        return self._trophies

    @Property(int, notify=stateChanged)
    def trophiesEarned(self):
        return sum(1 for row in self._trophies if row['earned'])

    # -- which game (Second Son / First Light) -------------------------------------------------
    @Property('QVariantList', notify=langChanged)
    def gameOptions(self):
        from .ui_game_options import enrich_game_options
        options = [{'value': key, 'label': self.t_('game_' + key)} for key in tlg_run.GAMES]
        # Home tiles: each game's icon and its own trophy counts (read-only; never grants trophies).
        return enrich_game_options(options, self.settings, tlg_run, trophies)

    @Property(int, notify=gameDirChanged)
    def gameIndex(self):
        return list(tlg_run.GAMES).index(tlg_run.current_game())

    @Property(str, notify=gameDirChanged)
    def gameName(self):
        return tlg_run.GAMES[tlg_run.current_game()]['name']

    @Slot(str)
    def setGame(self, key):
        """Switches between the games: their folders, saves, settings and shader caches stay apart."""
        if key not in tlg_run.GAMES or key == tlg_run.current_game() or self._play_state != 'idle':
            return
        tlg_run.switch_game(self.settings, key)
        tlg_run.save_settings(self.settings)
        self._message = ''
        self._verify_text, self._verify_level = '', ''
        self.verifyChanged.emit()
        self.reload_user()          # tlg.ini, fixes and the last save come from the game's own folder
        if hasattr(self, 'saves'):  # saves of other installs: look again, for this game's saves
            self.saves._found = []
            self.saves.scan()
        self.gameLanguageChanged.emit()
        self.modsChanged.emit()

    # -- controller guides ----------------------------------------------------------------------
    @Property('QVariantList', constant=True)
    def guideKinds(self):
        return [{'key': k, 'label': label} for k, label in logic.GUIDE_KINDS]

    @Property(str, notify=guideChanged)
    def guideKind(self):
        if self._guide_kind is None:   # preselect the detected controller; Xbox when unknown
            self._guide_kind = logic.guide_kind(self._pad)
        return self._guide_kind

    @Property(str, notify=guideChanged)
    def guideDetected(self):
        return self.t_('guides_detected', name=self._pad) if self._pad else ''

    def _guide_file(self):
        return logic.guide_path(self.guideKind, self.t_.pt(), self.probes.get('guide_dir'))

    @Property(str, notify=guideChanged)
    def guideSource(self):
        path = self._guide_file()
        return QUrl.fromLocalFile(str(path)).toString() if path else ''

    @Property('QVariantMap', notify=guideChanged)
    def guideSpots(self):
        """Where each number of the current guide image is (2026-11-23, qt/data/guide_spots.json, made
        by tools-guide_spots_build.py from the images): legend rows on the right, marker boxes on the
        controller. Empty when the image is not one of the shipped ones (a swapped file)."""
        path = self._guide_file()
        if not path:
            return {}
        try:
            import json
            table = json.loads((Path(__file__).parent / 'data' / 'guide_spots.json').read_text(encoding='utf-8'))
            spots = dict(table.get(path.name) or {})
            size = spots.get('size') or [0, 0]
            # a swapped image of another size has other positions: no highlights then
            if [int(v) for v in size] != list(self._image_size(path)):
                return {}
            # the clean picture and one layer per number (tools-guide_layers_build.py), as URLs
            clean = path.parent / spots.get('clean', '')
            layers = {n: path.parent / rel for n, rel in (spots.get('layers') or {}).items()}
            if spots.get('clean') and clean.is_file() and layers and all(p.is_file() for p in layers.values()):
                spots['cleanUrl'] = QUrl.fromLocalFile(str(clean)).toString()
                spots['layerUrls'] = {n: QUrl.fromLocalFile(str(p)).toString() for n, p in layers.items()}
            return spots
        except (OSError, ValueError, TypeError):
            return {}

    @staticmethod
    def _image_size(path):
        from PySide6.QtGui import QImageReader
        size = QImageReader(str(path)).size()
        return size.width(), size.height()

    @Property(str, notify=guideChanged)
    def guideMessage(self):
        return self._guide_message

    @Slot(str)
    def setGuideKind(self, kind):
        self._guide_kind, self._guide_message = kind, ''
        self.guideChanged.emit()

    def _guide_say(self, text):
        self._guide_message = text
        self.guideChanged.emit()

    @Slot(str)
    def saveGuide(self, url):
        path, target = self._guide_file(), QUrl(url).toLocalFile() if url.startswith('file:') else url
        if not path or not target:
            return
        if not target.lower().endswith('.png'):
            target += '.png'
        try:
            import shutil
            shutil.copyfile(path, target)
        except OSError as error:
            self._guide_say(self.t_('guides_failed', detail=str(error)))
            return
        self._guide_say(self.t_('guides_saved', path=target))

    @Slot()
    def copyGuide(self):
        path = self._guide_file()
        if not path:
            return
        from PySide6.QtGui import QGuiApplication, QImage
        image = QImage(str(path))
        app = QGuiApplication.instance()
        if image.isNull() or app is None:
            self._guide_say(self.t_('guides_failed', detail=str(path)))
            return
        app.clipboard().setImage(image)
        self._guide_say(self.t_('guides_copied'))

    # -- the controller guides UI (2026-11-23): the player's photos with rings drawn by the launcher ----
    @Property('QVariantMap', notify=guideChanged)
    def guideUi(self):
        """The current controller for GuidesPage: photo URL and size, button positions, which buttons
        each numbered line lights, and the texts in the launcher's language (qt/data/controller_ui.json)."""
        try:
            import json
            table = json.loads((Path(__file__).parent / 'data' / 'controller_ui.json').read_text(encoding='utf-8'))
        except (OSError, ValueError):
            return {}
        kind = table['kinds'].get(self.guideKind) or table['kinds'].get('xbox')
        photo = logic.GUIDE_DIR / kind['photo']
        if not photo.is_file():
            return {}
        lang = 'pt' if self.t_.pt() else 'en'
        common = table['common']
        return {'photoUrl': QUrl.fromLocalFile(str(photo)).toString(), 'size': kind['size'], 'name': kind['name'],
                'controls': kind['controls'], 'callouts': kind.get('callouts', {}), 'colors': table['colors'],
                'kicker': common['kicker'][lang], 'title': common['title'][lang],
                'subtitle': common['subtitle'][lang], 'footer': common['footer'][lang],
                'rows': [{'n': i + 1, 'title': common['titles'][i][lang], 'detail': kind['details'][i][lang],
                          'controls': kind['items'][i]} for i in range(len(kind['items']))]}

    @Property(str, constant=True)
    def guideGrabPath(self):
        """Where GuidesPage writes its picture (grabToImage) before Save image / Copy image."""
        import tempfile
        return str(Path(tempfile.gettempdir()) / 'iss-controller-guide.png')

    @Slot(str)
    def saveGuideImage(self, url):
        target = QUrl(url).toLocalFile() if url.startswith('file:') else url
        if not target:
            return
        if not target.lower().endswith('.png'):
            target += '.png'
        try:
            import shutil
            shutil.copyfile(self.guideGrabPath, target)
        except OSError as error:
            self._guide_say(self.t_('guides_failed', detail=str(error)))
            return
        self._guide_say(self.t_('guides_saved', path=target))

    @Slot()
    def copyGuideImage(self):
        from PySide6.QtGui import QGuiApplication, QImage
        image = QImage(self.guideGrabPath)
        app = QGuiApplication.instance()
        if image.isNull() or app is None:
            self._guide_say(self.t_('guides_failed', detail=self.guideGrabPath))
            return
        app.clipboard().setImage(image)
        self._guide_say(self.t_('guides_copied'))

    @Slot()
    def openGuidesFolder(self):
        tlg_run.open_external(self.probes.get('guide_dir') or logic.GUIDE_DIR)

    @Slot()
    def openReadme(self):
        readme = tlg_run.REPO_DIR / 'README.md'
        if readme.is_file():
            tlg_run.open_external(readme)

    @Slot(str, str)
    def setFix(self, item_id, value):
        if item_id == 'menu:fps_limit' and self._play_state != 'idle':
            return
        item = next(i for i in backports_table.ITEMS if i.id == item_id)
        self.fix_states[item_id] = backports_table.normalize(item, None if value == 'auto' else value)
        # Personal graphics preset selected (2026-11-23): the change becomes part of the player's own setup.
        from .gfxlogic import remember_personal
        remember_personal(self.settings, self.fix_states)
        self._save_fixes()
        self.fixes.refresh_item(item_id)
        self.advanced.refresh_item(item_id)
        self.refresh()

    @Slot(str)
    def resetFixes(self, which):
        if which == 'previous':
            self.fix_states = backports_table.previous_release(self.fix_states)
        else:
            self.fix_states = backports_table.defaults(self.fix_states)
        self._save_fixes()
        self.fixes.refresh_all()
        self.advanced.refresh_all()
        self.refresh()

    def _save_fixes(self):
        if 'save_fixes' in self.probes:
            self.probes['save_fixes'](self.settings, self.fix_states)
        else:
            tlg_run.save_experimental(self.settings, self.fix_states)


ART_EXTENSIONS = ('.jpg', '.jpeg', '.png', '.webp')


def custom_art(game):
    """The player's own picture for a game's Play tab: <data folder>/launcher-art/<game>.jpg|png|webp
    (second_son, first_light). Never shipped: the player puts it there."""
    folder = userdata.user_dir(tlg_run.PORT_DIR) / 'launcher-art'
    for ext in ART_EXTENSIONS:
        if (folder / f'{game}{ext}').is_file():
            return folder / f'{game}{ext}'
    return None


def animated_art(game):
    """The animated Play tab picture: the player's own <data folder>/launcher-art/<game>.gif, else the
    one shipped with the launcher (qt/assets/art/<game>.gif), or None."""
    for path in (userdata.user_dir(tlg_run.PORT_DIR) / 'launcher-art' / f'{game}.gif',
                 logic.HERE / 'assets' / 'art' / f'{game}.gif'):
        if path.is_file():
            return path
    return None


_art_colours = {}


def art_colours(path):
    """(background, smoke) colours of a picture as '#rrggbb': the average of its side edges, and
    the average of the coloured (saturated) pixels of its bottom 8%. Remembered per file version."""
    from PySide6.QtGui import QColor, QImage
    try:
        stat = Path(path).stat()
    except OSError:
        return '', ''
    key = (str(path), stat.st_size, stat.st_mtime_ns)
    if key in _art_colours:
        return _art_colours[key]
    image = QImage(str(path))   # the first frame of a GIF
    result = ('', '')
    if not image.isNull():
        w, h = image.width(), image.height()
        # the background: both side edges around the middle (corners often hold part of the subject)
        corner = [image.pixelColor(x, y) for x in (*range(0, max(1, w // 30)), *range(w - max(1, w // 30), w))
                  for y in range(int(h * 0.3), int(h * 0.55), 4)]
        bottom = [image.pixelColor(x, y) for x in range(0, w, 3) for y in range(int(h * 0.92), h, 3)]
        smoke = [c for c in bottom if c.hsvSaturationF() > 0.35 and c.valueF() > 0.25] or bottom

        def mean(colours):
            n = max(1, len(colours))
            return QColor(sum(c.red() for c in colours) // n, sum(c.green() for c in colours) // n,
                          sum(c.blue() for c in colours) // n).name()
        result = (mean(corner), mean(smoke))
    _art_colours[key] = result
    return result


def paper_colour(path):
    """The background colour of a still picture shown whole: the median of its border pixels, as
    '#rrggbb' (a subject touching one edge does not change it). Remembered per file version."""
    from PySide6.QtGui import QColor, QImage
    try:
        stat = Path(path).stat()
    except OSError:
        return ''
    key = ('paper', str(path), stat.st_size, stat.st_mtime_ns)
    if key not in _art_colours:
        image = QImage(str(path))
        result = ''
        if not image.isNull():
            w, h = image.width(), image.height()
            border = ([image.pixelColor(x, y) for x in range(0, w, 2) for y in (0, h - 1)] +
                      [image.pixelColor(x, y) for y in range(0, h, 2) for x in (0, w - 1)])

            def median(values):
                values = sorted(values)
                return values[len(values) // 2]
            result = QColor(median(c.red() for c in border), median(c.green() for c in border),
                            median(c.blue() for c in border)).name()
        _art_colours[key] = result
    return _art_colours[key]


# The game's own picture for a game's Play tab, read from the player's game folder (never shipped), and
# how it is shown: Second Son's pic0.png (Delsin, black and white) behind the whole tab ('backdrop');
# First Light's square game icon (Fetch and the title on white) whole on its own paper ('whole').
GAME_PICTURE = {'second_son': ('pic0.png', 'backdrop'), 'first_light': ('icon0.png', 'whole')}


def game_picture(settings, game):
    """(path, kind) of the player's game folder's picture for this game's Play tab, or (None, '')."""
    name, kind = GAME_PICTURE.get(game, (None, ''))
    if not name:
        return None, ''
    game_dir = settings.get('game_dir', '')
    # only from this game's own folder (a Second Son folder chosen by mistake must not show its icon)
    # (Second Son lists no titles in GAMES: US and European, as infamous_run.check_game assumes)
    if logic.shader_cache.title_id(game_dir) not in tlg_run.GAMES[game].get('titles', ('CUSA00223', 'CUSA00004')):
        return None, ''
    path = Path(game_dir) / 'sce_sys' / name
    return (path, kind) if path.is_file() else (None, '')


def play_art(settings):
    """(picture, kind) for the Play tab. kind 'gif': an animated GIF shown whole (the player's own
    launcher-art/<game>.gif first); then the game's own picture (GAME_PICTURE: 'whole' shown whole,
    'backdrop' behind the whole tab); then a shipped GIF; 'cover': a picture that covers the panel (the
    drawn art, ...); (None, '') if none."""
    game = tlg_run.current_game()
    own_gif = userdata.user_dir(tlg_run.PORT_DIR) / 'launcher-art' / f'{game}.gif'
    if own_gif.is_file():
        return own_gif, 'gif'
    picture, kind = game_picture(settings, game)
    if picture:
        return picture, kind
    animated = animated_art(game)
    if animated:
        return animated, 'gif'
    for name in ('pic1.png', 'pic0.png'):
        cover = _cover_art(settings, name)
        if cover:
            return cover, 'cover'
    return None, ''


def _art(settings, name):
    """The Play tab's picture (play_art)."""
    return play_art(settings)[0]


def _cover_art(settings, name):
    """The covering picture: always the port's drawn artwork (qt/assets/art/<game>.png, made by
    art/draw_art.py: no game asset). Only if that file is missing from the package: the player's
    launcher-art picture, then the game's pic1/pic0."""
    drawn = logic.HERE / 'assets' / 'art' / f'{tlg_run.current_game()}.png'
    if drawn.is_file():
        return drawn
    custom = custom_art(tlg_run.current_game())
    if custom:
        return custom
    for folder in (Path(settings.get('game_dir', '')) / 'sce_sys', tlg_run.REPO_DIR / 'dump' / 'sce_sys'):
        if (folder / name).is_file():
            return folder / name
    return None
