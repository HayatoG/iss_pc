# SPDX-License-Identifier: GPL-2.0-or-later
"""The QObject behind the Graphics screen. Reads and writes exactly what the Tk launcher and the
in-game Insert menu use: settings.json (display mode, VSync, FPS overlay, GPU accuracy, readback
switches, VRAM budget) and tlg.ini through the same table and the same save path as the Fixes
screen (Backend.setFix -> infamous_run.save_experimental). It adds no key of its own."""
import threading
from pathlib import Path

from PySide6.QtCore import Property, QObject, QUrl, Signal, Slot

from . import gfxlogic, logic, report
from .logic import backports_table, shader_cache, tlg_run
from .rowsmodel import RowsModel
import dlss_setup  # noqa: E402  (launcher/dlss_setup.py: the NVIDIA DLL, the bridge, the download)
import native_resolution  # noqa: E402  (launcher/native_resolution.py: output + internal resolution)
import preset_share  # noqa: E402  (launcher/preset_share.py; logic put the folder on sys.path)

# (id, kind, live) of the options kept in settings.json. live: also in the Insert menu.
SETTING_ROWS = {
    's:display_mode': ('display_mode', 'combo', True),
    's:vsync': ('vsync', 'switch', True),
    's:show_fps': ('show_fps', 'switch', True),
    's:gpu_readbacks': ('gpu_readbacks', 'combo', False),
    's:eager_readback': ('eager_readback', 'switch', False),
    's:write_merge': ('write_merge', 'switch', False),
    's:precompile_shaders': ('precompile_shaders', 'switch', False),
    's:close_on_play': ('close_on_play', 'switch', True),
    's:reopen_launcher': ('reopen_launcher', 'switch', False),
    's:vram_auto': ('vram_auto', 'switch', False),
    's:vram_budget_mb': ('vram_budget_mb', 'slider', False),
}
DISPLAY_MODES = ('Windowed', 'Borderless', 'Fullscreen')
OVERLAY_IDS = tuple(i.id for i in backports_table.MENU if i.tab == 'Overlay')
# Two-value choices shown here as a switch: id -> (value when off, value when on, label key).
SWITCH_CHOICES = {'TLG_RENDERER': ('1', '2', 'gfx_renderer_v2')}
SECTIONS = (
    ('nat_section', ('n:output', 'n:internal')),
    ('gfx_sec_renderer', ('TLG_RENDERER', 'TLG_NATIVE_PASSES')),
    ('gfx_sec_image', ('s:display_mode', 's:vsync', 'menu:fsr1', 'menu:rcas', 'menu:rcas_strength', 'menu:iss_temporal', 'menu:iss_temporal_mode',
                        'TLG_BARRIER_MERGE')),
    ('gfx_sec_rate', ('menu:fps_limit', 'menu:frames_queued')),
    ('gfx_sec_view', tuple(i.id for i in backports_table.MENU if i.tab == 'View' and not i.hidden)),
    ('gfx_sec_gpu', ('TLG_ASYNC_SHADERS', 'TLG_BACKGROUND_WARMUP', 's:gpu_readbacks', 's:eager_readback',
                     's:write_merge', 'TLG_DEFERRED_READBACK', 'TLG_STALE_READBACK', 'TLG_DRAIN_SLEEP', 's:precompile_shaders',
                     's:close_on_play', 's:reopen_launcher')),
    ('gfx_sec_vram', ('s:vram_auto', 's:vram_budget_mb')),
    ('gfx_sec_overlay', ('s:show_fps',) + OVERLAY_IDS),
)
STEPS = {'menu:overlay_scale': 10, 's:vram_budget_mb': 200}


class Graphics(QObject):
    changed = Signal()
    vramFound = Signal(object)
    dlssProgressed = Signal(int, int)
    dlssChecked = Signal(object)
    dlssFinished = Signal(object)    # a Component, or a DlssError
    scrollRequested = Signal(int)   # used by the screenshot tool
    dialogChanged = Signal()

    def __init__(self, backend, probes=None):
        """probes: optional callables replacing the slow/system ones (tests, screenshots):
        'vram' -> MB or None, 'clear' -> (bytes freed, failures)."""
        super().__init__(backend)
        self.backend = backend
        self.probes = probes or {}
        self.model = RowsModel(self)
        self._vram_mb = None
        self._vram_done = False
        self._confirm = False
        self._cache = 0
        self._msg, self._tone = '', 'info'
        self._share_msg, self._share_tone = '', 'info'
        self._dialog, self._handlers = {}, {}
        self._imported = None      # (profile name, targets) of the last imported file
        self.vramFound.connect(self._on_vram)
        self._dlss_confirm, self._dlss_busy = False, False
        self._dlss_cancel = threading.Event()
        self._dlss_msg, self._dlss_tone, self._dlss_done, self._dlss_total = '', 'info', 0, 0
        self._dlss_checked = None
        self.dlssChecked.connect(self._on_dlss_checked)
        self.dlssProgressed.connect(self._on_dlss_progress)
        self.dlssFinished.connect(self._on_dlss_finished)
        backend.profileChanged.connect(self.rebuild)
        backend.langChanged.connect(self.rebuild)
        backend.saves.changed.connect(self.changed)
        backend.cacheMeasured.connect(self._on_cache_measured)
        self.rebuild()

    # -- helpers ------------------------------------------------------------------------------------
    def t(self, key, **fmt):
        return self.backend.t_(key, **fmt)

    @property
    def settings(self):
        return self.backend.settings

    def start(self):
        """Reads the card's VRAM off the UI thread (a process)."""
        def work():
            self.vramFound.emit(self.probes['vram']() if 'vram' in self.probes else
                                gfxlogic.detect_vram_mb(self._gpu_name()))
        threading.Thread(target=work, daemon=True).start()
        self._check_dlss()

    def _gpu_name(self):
        gpu = self.backend._gpu
        return gpu[0] if isinstance(gpu, tuple) else None

    def _on_vram(self, mb):
        self._vram_mb, self._vram_done = mb, True
        self.rebuild()

    def set_vram(self, mb):
        """Direct entry for tests and screenshots."""
        self._on_vram(mb)

    # -- rows ------------------------------------------------------------------------------------------
    def _tag(self, live):
        return {'text': self.t('exp_live' if live else 'exp_restart'), 'tone': 'info'}

    def _setting_row(self, rid):
        key, control, live = SETTING_ROWS[rid]
        label_key = 'vram' if key == 'vram_budget_mb' else key
        row = {'kind': 'item', 'id': rid, 'code': '', 'label': self.t(label_key),
               'what': self.t('gfx_what_' + key), 'tags': [self._tag(live)], 'shaderWarn': '',
               'changed': self.settings.get(key) != tlg_run.DEFAULT_SETTINGS.get(key), 'control': control,
               'on': False, 'options': [], 'index': 0, 'lo': 0, 'hi': 0, 'step': 1, 'unit': '', 'value': 0,
               'valueText': '', 'disabled': False, 'decimals': 0, 'note': '', 'info': '', 'action': None}
        value = self.settings.get(key, tlg_run.DEFAULT_SETTINGS.get(key))
        default = tlg_run.DEFAULT_SETTINGS.get(key)
        if key == 'vram_auto':
            # Only renderer v2 computes it; this port's renderer always uses the budget below.
            row['disabled'] = backports_table.effective(gfxlogic.item_by_id('TLG_RENDERER'),
                                                        self.backend.fix_states) != '2'
        if key == 'vram_budget_mb':
            row['disabled'] = bool(self.settings.get('vram_auto', True)) and backports_table.effective(
                gfxlogic.item_by_id('TLG_RENDERER'), self.backend.fix_states) == '2'
            row['info'] = self.vramLine
            row['note'] = self.vramWarn
            if self.canSuggest:
                row['action'] = {'id': 'suggest', 'label': self.t('gfx_vram_use', budget=self.suggested)}
        if control == 'switch':
            row['on'] = bool(value)
        elif control == 'slider':
            row.update(lo=gfxlogic.BUDGET_MIN, hi=gfxlogic.BUDGET_MAX, step=STEPS[rid], unit='MB',
                       value=int(value), valueText=f'{int(value)} MB')
        else:
            names = DISPLAY_MODES if key == 'display_mode' else ('0', '1', '2')
            options = [(n, self.t(n if key == 'display_mode' else 'readbacks_' + n)) for n in names]
            options = [(n, label + (f' ({self.t("exp_default")})' if n == str(default) else ''))
                       for n, label in options]
            row['options'] = [{'value': n, 'label': label} for n, label in options]
            row['index'] = next((i for i, (n, _) in enumerate(options) if n == str(value)), 0)
        return row

    # -- output and internal resolution (tlg.ini: iss_native_output, iss_native_percent) ---------------
    def _renderer_v2(self):
        """Same test as infamous_run.renderer_v2, on the states the screen already holds."""
        return backports_table.effective(gfxlogic.item_by_id('TLG_RENDERER'), self.backend.fix_states) == '2'             and tlg_run.V2_PROBE.is_file()

    def _native_row(self, rid):
        output, percent = native_resolution.read(tlg_run.read_ini(tlg_run.user_dir() / 'tlg.ini'))
        t = self.backend.t_
        v2 = self._renderer_v2()
        row = {'kind': 'item', 'id': rid, 'code': '', 'label': '', 'what': '', 'tags': [self._tag(False)],
               'shaderWarn': '', 'changed': False, 'control': 'combo', 'on': False, 'options': [], 'index': 0,
               'lo': 0, 'hi': 0, 'step': 1, 'unit': '', 'value': 0, 'valueText': '', 'disabled': not v2,
               'decimals': 0, 'note': '' if v2 else self.t('nat_needs_v2'), 'info': '', 'action': None}
        if rid == 'n:output':
            options = native_resolution.output_options(t)
            row.update(label=self.t('nat_output'), what=self.t('nat_output_what'), changed=output != 0,
                       options=[{'value': v, 'label': label} for v, label in options], index=output)
            return row
        options = native_resolution.internal_options(t, output, percent)
        current = native_resolution.internal_value(output, percent)
        text, tone = native_resolution.summary(t, output, percent)
        row.update(label=self.t('nat_internal'), what=self.t('nat_internal_what'), changed=percent != 100,
                   options=[{'value': v, 'label': label} for v, label, _ in options],
                   index=next((n for n, (v, _, _) in enumerate(options) if v == current), 0),
                   disabled=not v2 or not output)
        if v2:        # the summary says what the next start does; a warning or a refusal goes in the note
            if output:
                text += ' ' + self.t('nat_restart')
            row['note' if tone != 'info' else 'info'] = text
        return row

    def _set_native(self, rid, value):
        path = tlg_run.user_dir() / 'tlg.ini'
        output, percent = native_resolution.read(tlg_run.read_ini(path))
        if rid == 'n:output':
            new_output = int(value)
            percent = native_resolution.retarget(output, percent, new_output)
            output = new_output
        elif value != 'custom':
            status, best, *_ = native_resolution.pick(output, int(value))
            if status != native_resolution.PICK_OK:      # the game would refuse it: keep what is stored
                self.rebuild()
                return
            percent = best
        native_resolution.save(path, output, percent)
        self.rebuild()

    def _item_row(self, rid):
        item = gfxlogic.item_by_id(rid)
        states = self.backend.fix_states
        value = backports_table.effective(item, states)
        t = self.backend.t_
        tags = [self._tag(item.live)]
        if item.shader:
            tags.append({'text': '⚠ ' + self.t('fixes_shader_warn'), 'tone': 'warn'})
        row = {'kind': 'item', 'id': rid, 'code': '', 'label': t.pair(item.name), 'what': t.pair(item.what),
               'tags': tags, 'shaderWarn': '', 'changed': states.get(rid) is not None, 'control': 'switch',
               'on': value == '1', 'options': [], 'index': 0, 'lo': 0, 'hi': 0, 'step': 1, 'unit': '',
               'value': 0, 'valueText': '', 'disabled': False, 'decimals': 0, 'note': '', 'info': '', 'action': None}
        if item.vtype == 'int':
            row.update(control='slider', lo=item.lo, hi=item.hi, step=STEPS.get(rid, 5), unit=item.unit,
                       value=int(value), valueText=f'{int(value)}{item.unit}')
            if rid == 'menu:rcas_strength':
                row['disabled'] = backports_table.effective(gfxlogic.item_by_id('menu:rcas'), states) != '1'
        if rid == 'TLG_RENDERER' and backports_table.V1_DISABLED:
            row['disabled'] = True          # renderer 1 is off in this build (backports_table.V1_DISABLED)
        if rid == 'TLG_NATIVE_PASSES':
            # Native passes only exist in renderer v2.
            row['disabled'] = backports_table.effective(gfxlogic.item_by_id('TLG_RENDERER'), states) != '2'
        elif item.vtype == 'float':
            step = backports_table.FLOAT_STEPS.get(item.key, 0.05)
            decimals = 0 if step >= 1 else 1 if step >= 0.1 else 2 if step >= 0.01 else 3
            number = float(value)
            row.update(control='slider', lo=item.lo, hi=item.hi, step=step, unit=item.unit, value=number,
                       decimals=decimals, valueText=f'{number:.{decimals}f}{item.unit}')
        elif rid in SWITCH_CHOICES:
            _, on_value, label_key = SWITCH_CHOICES[rid]
            row.update(label=self.t(label_key), on=value == on_value)
        elif item.vtype == 'choice':
            options = [(v, t.pair((en, pt)) + (f' ({self.t("exp_default")})' if v == item.default else ''))
                       for v, en, pt in item.choices]
            row.update(control='combo', options=[{'value': v, 'label': label} for v, label in options],
                       index=next((n for n, (v, _) in enumerate(options) if v == value), 0))
        return row

    def rebuild(self):
        self._cache = self.read_cache_bytes()
        rows = []
        for title, ids in SECTIONS:
            rows.append({'kind': 'header', 'id': 'hdr-' + title, 'title': self.t(title), 'note': ''})
            seen = set()
            for rid in ids:
                if rid in seen:
                    continue
                seen.add(rid)
                rows.append(self._setting_row(rid) if rid.startswith('s:') else
                            self._native_row(rid) if rid.startswith('n:') else self._item_row(rid))
        self.model.set_rows(rows)
        self.changed.emit()

    # -- writing ----------------------------------------------------------------------------------------
    @Slot(str, str)
    def setValue(self, rid, value):
        if rid.startswith('n:'):
            self._set_native(rid, value)
        elif rid in SETTING_ROWS:
            key, control, _ = SETTING_ROWS[rid]
            if control == 'switch':
                self.settings[key] = value == '1'
            elif control == 'slider':
                self.settings[key] = min(max(int(float(value)), gfxlogic.BUDGET_MIN), gfxlogic.BUDGET_MAX)
            else:
                self.settings[key] = value
            tlg_run.save_settings(self.settings)
            self.rebuild()
        else:
            if rid in SWITCH_CHOICES:
                off_value, on_value, _ = SWITCH_CHOICES[rid]
                value = on_value if value == '1' else off_value
            self.backend.setFix(rid, value)      # saves tlg.ini, refreshes the chips; rebuild follows

    @Slot(str)
    def applyPreset(self, preset_id):
        self._apply_targets(gfxlogic.preset_targets(preset_id))

    def _apply_targets(self, targets):
        gfxlogic.apply_targets(self.settings, self.backend.fix_states, targets)
        self.backend._save_fixes()               # tlg.ini + settings.json (one write)
        self.backend.fixes.refresh_all()
        self.backend.refresh()                   # chips; profileChanged -> rebuild

    @Slot(str, str)
    def runAction(self, rid, action):
        if action == 'suggest':
            self.useSuggestedBudget()

    @Slot()
    def useSuggestedBudget(self):
        if self.suggested:
            self.setValue('s:vram_budget_mb', str(self.suggested))

    # -- presets -----------------------------------------------------------------------------------------
    @Property('QVariantList', notify=changed)
    def presets(self):
        current = gfxlogic.matching_preset(self.settings, self.backend.fix_states)
        result = []
        for pid, _, _ in gfxlogic.PRESETS:
            preset = gfxlogic.FULL_PRESETS.get(pid, {})
            result.append({'id': pid, 'label': self.t('gfx_preset_' + pid),
                           'what': self.t('gfx_preset_%s_what' % pid),
                           'credit': preset.get('credit', ''), 'creditUrl': preset.get('credit_url', ''),
                           'active': pid == current, 'standard': pid == gfxlogic.DEFAULT_PRESET})
        return result

    def _imported_name(self):
        """The profile name of the imported file while the options still equal it."""
        if not self._imported:
            return ''
        name, (settings, stored) = self._imported
        same = all(gfxlogic.same_setting(k, self.settings.get(k), v) for k, v in settings.items()) and \
            all(self.backend.fix_states.get(i) == v for i, v in stored.items())
        return name if same else ''

    @Property(str, notify=changed)
    def presetText(self):
        current = gfxlogic.matching_preset(self.settings, self.backend.fix_states)
        if current:
            return self.t('gfx_preset_now', name=self.t('gfx_preset_' + current))
        name = self._imported_name()
        return self.t('ps_profile_imported', name=name) if name else self.t('gfx_preset_custom')

    # -- share: export and import a configuration file -------------------------------------------------------
    @Property(str, notify=changed)
    def exportName(self):
        """The name the export dialog starts with: the current profile."""
        current = gfxlogic.matching_preset(self.settings, self.backend.fix_states)
        return self.t('gfx_preset_' + current) if current else self._imported_name()

    @Property(str, notify=changed)
    def exportFile(self):
        return preset_share.default_filename(self.exportName)

    @Property(str, notify=changed)
    def shareMessage(self):
        return self._share_msg

    @Property(str, notify=changed)
    def shareTone(self):
        return self._share_tone

    def _port_version(self):
        if 'version' in self.probes:
            return self.probes['version']()
        try:
            return report.current_version((tlg_run.PORT_DIR, tlg_run.REPO_DIR), tlg_run.LOG_DIR)
        except Exception:
            return ''

    @staticmethod
    def _local(url):
        return QUrl(url).toLocalFile() if str(url).startswith('file:') else str(url)

    @Slot(str, str, str)
    def exportPreset(self, name, what, url):
        path = self._local(url)
        if not path:
            return
        preset = preset_share.export_preset(self.settings, self.backend.fix_states, name, what, self._port_version())
        try:
            final = preset_share.write_preset(preset, path)
        except (OSError, ValueError) as error:
            self._share_msg, self._share_tone = self.t('ps_export_failed', detail=str(error)), 'bad'
        else:
            self._share_msg, self._share_tone = self.t('ps_exported', path=str(final)), 'good'
        self.changed.emit()

    @Slot(str)
    def importPreset(self, url):
        path = self._local(url)
        if not path:
            return
        preset, problems = preset_share.read_preset(path)
        title = self.t('ps_import_title')
        close = [('close', self.t('ps_close'), 'ghost')]
        if preset is None:
            self._ask(title, '', close, {}, [preset_share.problem_text(p, self.t) for p in problems])
            return
        changes = preset_share.diff(preset, self.settings, self.backend.fix_states)
        body, lines = preset_share.summary_lines(preset, problems, changes, self.t, self.backend.t_.pt())
        if not changes:
            self._ask(title, body, close, {}, lines)
            return
        self._ask(title, body, [('apply', self.t('ps_apply'), 'primary'), ('cancel', self.t('ps_cancel'), 'ghost')],
                  {'apply': lambda: self._apply_imported(preset, len(changes))}, lines)

    def _apply_imported(self, preset, count):
        targets = gfxlogic.custom_targets(preset)
        self._imported = (preset['profile'], targets)
        self._apply_targets(targets)
        self._share_msg, self._share_tone = self.t('ps_imported', name=preset['profile'], n=count), 'good'
        self.changed.emit()

    def _ask(self, title, body, buttons, handlers, lines=()):
        self._handlers = handlers
        self._dialog = {'open': True, 'title': title, 'body': body, 'lines': list(lines),
                        'buttons': [{'id': i, 'label': l, 'tone': t} for i, l, t in buttons]}
        self.dialogChanged.emit()

    @Property('QVariantMap', notify=dialogChanged)
    def dialog(self):
        return self._dialog

    @Slot(str)
    def answer(self, button):
        handler = self._handlers.get(button)
        self._dialog, self._handlers = {}, {}
        self.dialogChanged.emit()
        if handler:
            handler()

    @Property(QObject, constant=True)
    def rows(self):
        return self.model

    # -- VRAM ----------------------------------------------------------------------------------------------
    @property
    def suggested(self):
        return gfxlogic.suggested_budget_mb(self._vram_mb / 1024) if self._vram_mb else 0

    @Property(str, notify=changed)
    def vramLine(self):
        gpu = self._gpu_name()
        if not self._vram_done:
            return self.t('gfx_vram_checking')
        if not self._vram_mb:
            return self.t('gfx_vram_unknown') + ' ' + self.t('vram_hint')
        gb = self._vram_mb / 1024
        size = f'{gb:.0f}' if abs(gb - round(gb)) < 0.15 else f'{gb:.1f}'
        return self.t('gfx_vram_line', gpu=gpu or self.t('gfx_gpu_generic'), gb=size, budget=self.suggested)

    @Property(str, notify=changed)
    def vramWarn(self):
        budget = int(self.settings.get('vram_budget_mb', 0))
        if self._vram_mb and gfxlogic.budget_is_too_high(budget, self._vram_mb):
            return self.t('gfx_vram_high', budget=budget, vram=self._vram_mb)
        return ''

    @Property(int, notify=changed)
    def vramSuggested(self):
        return self.suggested

    @Property(bool, notify=changed)
    def canSuggest(self):
        return bool(self.suggested) and int(self.settings.get('vram_budget_mb', 0)) != self.suggested

    # -- NVIDIA DLSS (dlss_setup.py): status of the two DLLs and the download ---------------------------------
    def _dlss_configured(self):
        return str(tlg_run.read_ini(tlg_run.user_dir() / 'tlg.ini').get('dlss_dll_dir', ''))

    def _dlss_state(self):
        """The last check (None until the first one ends): the signature check starts PowerShell, so it runs
        on a worker thread (_check_dlss) and the properties only read its result."""
        return self._dlss_checked

    def _check_dlss(self):
        def work():
            state = self.probes['dlss_status']() if 'dlss_status' in self.probes else dlss_setup.status(
                tlg_run.user_dir(), self._dlss_configured(), tlg_run.V2_PROBE.parent)
            self.dlssChecked.emit(state)
        threading.Thread(target=work, daemon=True).start()

    def _on_dlss_checked(self, state):
        self._dlss_checked = state
        self.changed.emit()

    def _dlss_target(self):
        configured = self._dlss_configured()
        return Path(configured) if configured else dlss_setup.default_folder(tlg_run.user_dir())

    @Property(str, notify=changed)
    def dlssHint(self):
        return self.t('dlss_what')

    @Property(str, notify=changed)
    def dlssDllLine(self):
        state = self._dlss_state()
        return dlss_setup.dll_line(self.t, state['dll']) if state else self.t('dlss_checking')

    @Property(str, notify=changed)
    def dlssBridgeLine(self):
        state = self._dlss_state()
        return dlss_setup.bridge_line(self.t, state['bridge']) if state else ''

    @Property(str, notify=changed)
    def dlssReadyLine(self):
        state = self._dlss_state()
        return self.t('dlss_ready') if state and dlss_setup.usable(state) else ''

    @Property(bool, notify=changed)
    def dlssHasDll(self):
        state = self._dlss_state()
        return bool(state and state['dll'].ok)

    @Property(bool, notify=changed)
    def dlssConfirming(self):
        return self._dlss_confirm

    @Property(bool, notify=changed)
    def dlssBusy(self):
        return self._dlss_busy

    @Property(str, notify=changed)
    def dlssConfirmText(self):
        return self.t('dlss_confirm', mb=dlss_setup.NOMINAL_MB, url=dlss_setup.SOURCE_URL, repo=dlss_setup.REPO_URL)

    @Property(str, notify=changed)
    def dlssProgressText(self):
        mb = 1024 * 1024
        if self._dlss_total:
            return self.t('dlss_progress', done=self._dlss_done // mb, total=self._dlss_total // mb)
        return self.t('dlss_progress_unknown', done=self._dlss_done // mb)

    @Property(float, notify=changed)
    def dlssProgress(self):
        return self._dlss_done / self._dlss_total if self._dlss_total else 0.0

    @Property(str, notify=changed)
    def dlssMessage(self):
        return self._dlss_msg

    @Property(str, notify=changed)
    def dlssMessageTone(self):
        return self._dlss_tone

    @Slot()
    def askDlssDownload(self):
        if not self._dlss_busy:
            self._dlss_confirm, self._dlss_msg = True, ''
            self.changed.emit()

    @Slot()
    def cancelDlssAsk(self):
        self._dlss_confirm = False
        self.changed.emit()

    @Slot()
    def startDlssDownload(self):
        """The confirmed download, on a worker thread; progress and the end come back as signals."""
        if self._dlss_busy:
            return
        self._dlss_confirm, self._dlss_busy = False, True
        self._dlss_cancel.clear()
        self._dlss_done = self._dlss_total = 0
        target = self._dlss_target()
        download = self.probes.get('dlss_download', dlss_setup.download)

        def work():
            try:
                result = download(target, progress=self.dlssProgressed.emit, cancel=self._dlss_cancel)
            except dlss_setup.DlssError as error:
                result = error
            except Exception as error:      # never leave the screen stuck on "downloading"
                result = dlss_setup.DlssError('network', error)
            self.dlssFinished.emit(result)
        threading.Thread(target=work, daemon=True).start()
        self.changed.emit()

    @Slot()
    def cancelDlssDownload(self):
        self._dlss_cancel.set()

    def _on_dlss_progress(self, done, total):
        self._dlss_done, self._dlss_total = done, total
        self.changed.emit()

    def _on_dlss_finished(self, result):
        self._dlss_busy = False
        if isinstance(result, dlss_setup.DlssError):
            self._dlss_msg = self.t('dlss_err_' + result.key, detail=result.detail)
            self._dlss_tone = 'info' if result.key == 'cancelled' else 'bad'
        else:
            version = '.'.join((result.version or '?').split('.')[:3])
            self._dlss_msg, self._dlss_tone = self.t('dlss_done', version=version, path=str(result.path)), 'good'
        self.changed.emit()
        if not isinstance(result, dlss_setup.DlssError):
            self._check_dlss()

    # -- shader cache ----------------------------------------------------------------------------------------
    def read_cache_bytes(self):
        return self.backend.cache_bytes()   # measured off the UI thread (Backend.measure_cache)

    def cache_bytes(self):
        return self._cache

    def _on_cache_measured(self, _size):
        self._cache = self.read_cache_bytes()
        self.changed.emit()

    @Property(str, notify=changed)
    def cacheText(self):
        size = self.cache_bytes()
        return self.t('gfx_cache_size', size=shader_cache.human_size(size)) if size else self.t('chip_cache_empty')

    @Property(bool, notify=changed)
    def gameRunning(self):
        return self.backend.saves.running_now()

    @Property(bool, notify=changed)
    def canClear(self):
        return bool(self.cache_bytes()) and not self.backend.saves.running_now()

    @Property(bool, notify=changed)
    def confirming(self):
        return self._confirm

    @Property(str, notify=changed)
    def confirmText(self):
        return self.t('clear_cache_ask', size=shader_cache.human_size(self.cache_bytes()))

    @Property(str, notify=changed)
    def message(self):
        return self._msg

    @Property(str, notify=changed)
    def messageTone(self):
        return self._tone

    @Slot()
    def askClear(self):
        if self.canClear:
            self._confirm, self._msg = True, ''
            self.changed.emit()

    @Slot()
    def cancelClear(self):
        self._confirm = False
        self.changed.emit()

    @Slot()
    def clearCache(self):
        self._confirm = False
        if self.backend.saves.is_running():
            self._msg, self._tone = self.t('gfx_cache_running'), 'bad'
        else:
            freed, failures = (self.probes['clear']() if 'clear' in self.probes
                               else tlg_run.clear_shader_cache(self.settings))
            human = shader_cache.human_size(freed)
            if failures:
                self._msg, self._tone = self.t('clear_cache_failed', size=human, count=len(failures)), 'bad'
            else:
                self._msg, self._tone = self.t('clear_cache_done', size=human), 'good'
        self.backend.refresh()                  # the Home chip shows the new size; rebuild reads the cache again
