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
import perf_report  # noqa: E402  (launcher/perf_report.py: what held the last session's frame rate back)
import preset_share  # noqa: E402  (launcher/preset_share.py; logic put the folder on sys.path)

# (id, kind, live) of the options kept in settings.json. live: also in the Insert menu.
SETTING_ROWS = {
    's:display_mode': ('display_mode', 'combo', True),
    's:vsync': ('vsync', 'switch', True),
    's:allow_hdr': ('allow_hdr', 'switch', False),
    's:show_fps': ('show_fps', 'switch', True),
    's:gpu_readbacks': ('gpu_readbacks', 'combo', False),
    's:eager_readback': ('eager_readback', 'switch', False),
    's:write_merge': ('write_merge', 'switch', False),
    's:precompile_shaders': ('precompile_shaders', 'switch', False),
    's:close_on_play': ('close_on_play', 'switch', True),
    's:reopen_launcher': ('reopen_launcher', 'switch', False),
    's:vram_auto': ('vram_auto', 'switch', False),
    's:vram_budget_mb': ('vram_budget_mb', 'slider', False),
    's:v2_lighting_read_window': ('v2_lighting_read_window', 'combo', False),
    's:v2_amd_native_passes': ('v2_amd_native_passes', 'switch', False),
}
DISPLAY_MODES = ('Windowed', 'Borderless', 'Fullscreen')
OVERLAY_IDS = tuple(i.id for i in backports_table.MENU if i.tab == 'Overlay')
# Two-value choices shown here as a switch: id -> (value when off, value when on, label key).
SWITCH_CHOICES = {'TLG_RENDERER': ('1', '2', 'gfx_renderer_v2')}
SECTIONS = (
    # 2026-11-23: video memory and frame rate right under the resolution; "Upscale with FSR 1" and the
    # FSR 3.1 temporal switch are no longer listed here (the Play tab's Upscaler sets them).
    ('nat_section', ('n:output', 'n:internal')),
    ('gfx_sec_vram', ('s:vram_auto', 's:vram_budget_mb')),
    # The frame rate limit is set on the Play tab (FPS cap) and in the F1 overlay; not repeated here.
    ('gfx_sec_rate', ('menu:frames_queued',)),
    ('gfx_sec_renderer', ('TLG_RENDERER', 'TLG_NATIVE_PASSES', 's:v2_amd_native_passes',
                          's:v2_lighting_read_window')),
    ('gfx_sec_image', ('s:display_mode', 's:vsync', 's:allow_hdr','menu:rcas', 'menu:rcas_strength', 'menu:iss_temporal_mode',
                        'TLG_BARRIER_MERGE')),
    ('gfx_sec_view', tuple(i.id for i in backports_table.MENU if i.tab == 'View' and not i.hidden)),
    # 2026-11-23: read ahead, write without waiting, deferred/stale readbacks, command thread sleep and
    # the shader list are fixed now (infamous_run.LOCKED_GPU_ENV, precompile.decide) and not listed.
    # Asynchronous shader compile and loading pipelines while playing are fixed too (2026-11-23).
    ('gfx_sec_gpu', ('s:gpu_readbacks',
                     's:close_on_play', 's:reopen_launcher')),
    # The performance overlay's options are not shown here (2026-11-11): the in-game overlay (F1) has
    # them all, and F10 shows or hides it. tlg.ini keeps them; nothing about them changes.
)
STEPS = {'menu:overlay_scale': 10, 's:vram_budget_mb': 200}


class Graphics(QObject):
    changed = Signal()
    vramFound = Signal(object)
    dlssProgressed = Signal(int, int)
    dlssChecked = Signal(object)
    dlssFinished = Signal(object)    # a Component, or a DlssError
    dlssAutoDone = Signal(object)    # [problem texts] of the automatic download (empty: ready)
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
        # The tuned presets are fitted to this PC (2026-11-23): RAM now, VRAM as last detected (it is
        # detected again in the background and remembered in settings.json, so the first clicks already fit).
        try:
            from . import report
            ram = self.probes['ram']() if 'ram' in self.probes else report.ram_bytes()
            gfxlogic.HARDWARE['ram_mb'] = int(ram / (1024 * 1024)) if ram else None
        except Exception:
            gfxlogic.HARDWARE['ram_mb'] = None
        remembered = backend.settings.get('detected_vram_mb')
        gfxlogic.HARDWARE['vram_mb'] = int(remembered) if isinstance(remembered, (int, float)) and remembered > 0 else None
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
        self.dlssAutoDone.connect(self._on_dlss_auto_done)
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
        if mb:
            gfxlogic.HARDWARE['vram_mb'] = int(mb)
            if self.settings.get('detected_vram_mb') != int(mb):
                self.settings['detected_vram_mb'] = int(mb)
                tlg_run.save_settings(self.settings)
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
        if key == 'v2_lighting_read_window':
            # 'auto' (2026-11-22): the game picks by the card's video memory (whole table at 7 GB and
            # up, 64 MB below); '0' and '1024' force a value (infamous_run.renderer_v2_env).
            value = value if value in ('auto', '0', '1024') else 'auto'
            row.update(disabled=not self._renderer_v2(), changed=value != 'auto')
            default = 'auto'
        if key == 'v2_amd_native_passes':
            row.update(disabled=not self._renderer_v2(), changed=value is True)
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
            if key == 'v2_lighting_read_window':
                options = [(n, self.t('gfx_lighting_window_' + n)) for n in ('auto', '0', '1024')]
            elif key == 'display_mode':
                options = [(n, self.t(n)) for n in DISPLAY_MODES]
            else:
                options = [(n, self.t('readbacks_' + n)) for n in ('0', '1', '2')]
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
            if key == 'v2_lighting_read_window' and value not in ('auto', '0', '1024'):
                return
            if control == 'switch':
                self.settings[key] = value == '1'
            elif control == 'slider':
                self.settings[key] = min(max(int(float(value)), gfxlogic.BUDGET_MIN), gfxlogic.BUDGET_MAX)
            else:
                self.settings[key] = value
            gfxlogic.remember_personal(self.settings, self.backend.fix_states)   # personal preset: keep it
            tlg_run.save_settings(self.settings)
            self.rebuild()
        else:
            if rid in SWITCH_CHOICES:
                off_value, on_value, _ = SWITCH_CHOICES[rid]
                value = on_value if value == '1' else off_value
            self.backend.setFix(rid, value)      # saves tlg.ini, refreshes the chips; rebuild follows

    @Slot(str)
    def applyPreset(self, preset_id):
        # 2026-11-23: the clicked preset is remembered; while it is the personal one, every graphics change
        # is kept for it (gfxlogic.remember_personal) and clicking it again brings the player's setup back.
        if preset_id == gfxlogic.PERSONAL_ID:
            settings, items = gfxlogic.personal_targets(self.settings, self.backend.fix_states)
            self.settings['graphics_mode'] = gfxlogic.PERSONAL_ID
            self.settings.update(settings)
            self.backend.fix_states.update(items)
            gfxlogic.remember_personal(self.settings, self.backend.fix_states)
            self._apply_targets(({}, {}))
            return
        self.settings['graphics_mode'] = preset_id
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
                           'what': self.t('gfx_preset_%s_what' % pid), 'fit': self._fit_text(pid),
                           'credit': preset.get('credit', ''), 'creditUrl': preset.get('credit_url', ''),
                           'active': pid == current, 'standard': pid == gfxlogic.DEFAULT_PRESET})
        return result

    def _fit_text(self, pid):
        """One line under a tuned preset: the PC it is fitted to and what was lightened, '' otherwise."""
        if pid not in gfxlogic.TUNED_IDS:
            return ''
        hw = gfxlogic.HARDWARE
        parts = []
        if hw['vram_mb']:
            parts.append(self.t('gfx_fit_vram', gb=f"{hw['vram_mb'] / 1024:.0f}"))
        if hw['ram_mb']:
            parts.append(self.t('gfx_fit_ram', gb=f"{hw['ram_mb'] / 1024:.0f}"))
        head = self.t('gfx_fit_pc', what=', '.join(parts)) if parts else self.t('gfx_fit_pc_unknown')
        changes = gfxlogic.fit_notes(pid)
        tail = ', '.join(self.t('gfx_fit_' + key) for key in changes) if changes else self.t('gfx_fit_full')
        return f'{head}: {tail}.'

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

    # -- your last session: what held the frame rate back (2026-11-24) ---------------------------------
    def _perf_report(self):
        """perf_report.read of the newest game log; read again only when the log changed."""
        if 'perf' in self.probes:
            return self.probes['perf']()
        log = perf_report.latest_log(tlg_run.LOG_DIR)
        try:
            stamp = (str(log), log.stat().st_mtime_ns) if log else None
        except OSError:
            stamp = None
        if stamp != getattr(self, '_perf_stamp', 0):
            self._perf_stamp = stamp
            self._perf_cache = perf_report.read(log) if log else None
        return self._perf_cache

    def _perf_log(self):
        """The newest log's path (the session being described), '' without one."""
        if 'perf_log' in self.probes:
            return self.probes['perf_log']()
        stamp = getattr(self, '_perf_stamp', None)
        return stamp[0] if stamp else ''

    def _perf(self):
        """(key, values) of perf_report.verdict for the newest game log."""
        return perf_report.verdict(self._perf_report())

    @Property(str, notify=changed)
    def perfNotes(self):
        """Hitches, video memory nearly full, settings changed mid-session: one short sentence each."""
        return ' '.join(self.t('gfx_perf_note_' + key, **values)
                        for key, values in perf_report.notes(self._perf_report()))

    @Property(str, notify=changed)
    def perfCompare(self):
        """After an Apply, the next session against the one before it; '' until there is one."""
        record = self.settings.get('perf_last_apply')
        report = self._perf_report()
        after = perf_report.summary(report)
        if not isinstance(record, dict) or not after or self._perf_log() in ('', record.get('log')):
            return ''
        if perf_report.verdict(report)[0] in ('none', 'classic', 'short'):
            return ''
        before = record.get('before') or {}
        base_id = record.get('base') or ''
        base = self.t('gfx_preset_' + base_id) if base_id else self.t('gfx_perf_your_setup')
        text = self.t('gfx_perf_compare', base=base, before=before.get('fps', '?'), after=after['fps'])
        if before.get('p99_ms') and after.get('p99_ms'):
            text += ' ' + self.t('gfx_perf_compare_p99', before=before['p99_ms'], after=after['p99_ms'])
        return text

    @Property(str, notify=changed)
    def perfText(self):
        """One plain sentence about the last play session, '' when there is nothing to say."""
        key, values = self._perf()
        if key in ('none', 'classic'):
            return ''
        values = dict(values)
        # With the card's own time per frame (GPU_FRAME_MS, 2026-11-24 builds) the verdict is measured.
        sure = values.pop('sure', False) and key in ('gpu', 'cpu')
        return self.t('gfx_perf_' + key + ('_sure' if sure else ''), **values)

    @Property(str, notify=changed)
    def perfTone(self):
        """good: room to spare; warn: a part of the PC held it back; '' : nothing measured yet."""
        key, _ = self._perf()
        return {'limit': 'good', 'ceiling': 'good', 'gpu': 'warn', 'cpu': 'warn'}.get(key, '')

    @Slot()
    def refreshPerf(self):
        """The Graphics tab calls this when it is shown: a session may have ended since."""
        self.changed.emit()

    # LOD distance steps the suggestions move along (the menu's own range is 0.25-4).
    LOD_STEPS = (1.0, 1.25, 1.5, 2.0, 3.0, 4.0)
    FPS_CAPS = (30, 60, 120, 240)

    def _value(self, rid):
        item = gfxlogic.item_by_id(rid)
        state = self.backend.fix_states.get(rid)
        return item.default if state is None else state

    def _suggestions(self):
        """[(row id, new value)] that answer the last session's verdict, from the current setup:
        less work when the graphics card or the processor held it back, more detail when the PC had
        room to spare. The frame rate cap moves only for the processor (steadier motion)."""
        key, values = self._perf()
        if key not in ('gpu', 'cpu', 'limit', 'ceiling'):
            return []
        changes = []
        lod = float(self._value('menu:lod_scale'))
        floor = int(float(self._value('menu:quality_floor')))
        vram_full = bool((self._perf_report() or {}).get('vram_full'))
        if vram_full or key in ('gpu', 'cpu'):
            # Video memory nearly full: fewer detailed models even when the frame rate had room.
            lower = [s for s in self.LOD_STEPS if s < lod - 1e-6]
            if lower:
                changes.append(('menu:lod_scale', f'{lower[-1]:g}'))
            if floor < 12 and key in ('gpu', 'cpu'):
                changes.append(('menu:quality_floor', str(min(12, floor + 2))))
            if key == 'cpu':
                cap = int(self._value('menu:fps_limit') or 0)
                fits = [c for c in self.FPS_CAPS if c <= values.get('fps', 0) * 0.95 and (cap == 0 or c < cap)]
                if fits:
                    changes.append(('menu:fps_limit', str(fits[-1])))
        else:
            higher = [s for s in self.LOD_STEPS if s > lod + 1e-6 and s <= 2.0]
            if higher:
                changes.append(('menu:lod_scale', f'{higher[0]:g}'))
            if floor > 0:
                changes.append(('menu:quality_floor', '0'))
        return changes

    def _shown(self, rid, value):
        if rid == 'menu:fps_limit' and str(value) == '0':
            return self.t('gfx_perf_unlimited')
        return f'{float(value):g}x' if rid == 'menu:lod_scale' else str(value)

    @Property(bool, notify=changed)
    def perfCanApply(self):
        """A suggestion exists for the newest session and was not applied yet."""
        return bool(self._suggestions()) and getattr(self, '_perf_applied', None) != getattr(self, '_perf_stamp', 0)

    @Property(str, notify=changed)
    def perfMessage(self):
        return getattr(self, '_perf_msg', '')

    def _change_lines(self, changes):
        return [self.t('gfx_perf_change', name=self.t_pair(gfxlogic.item_by_id(rid).name),
                       old=self._shown(rid, self._value(rid)), new=self._shown(rid, value))
                for rid, value in changes]

    @Property(str, notify=changed)
    def perfPlan(self):
        """What "Apply" would change, shown before the click; '' when nothing would."""
        if not self.perfCanApply:
            return ''
        return self.t('gfx_perf_plan', changes='; '.join(self._change_lines(self._suggestions())))

    @Slot()
    def applyPerfSuggestions(self):
        """Saves the current setup plus the suggestions as Renderer V2 - Custom Personal Graphics, selects
        it and says what changed. The preset that was selected is left as it was."""
        changes = self._suggestions()
        if not changes:
            return
        current = gfxlogic.matching_preset(self.settings, self.backend.fix_states)
        base = self.t('gfx_preset_' + current) if current else self.t('gfx_perf_your_setup')
        # The personal preset starts from exactly what is set now; each change below is remembered too.
        self.settings['graphics_mode'] = gfxlogic.PERSONAL_ID
        gfxlogic.remember_personal(self.settings, self.backend.fix_states)
        tlg_run.save_settings(self.settings)
        lines = self._change_lines(changes)
        for rid, value in changes:
            self.setValue(rid, value)
        gfxlogic.remember_personal(self.settings, self.backend.fix_states)
        # Kept to compare the next session with this one (perfCompare).
        self.settings['perf_last_apply'] = {'log': self._perf_log(), 'base': current or '',
                                            'before': perf_report.summary(self._perf_report())}
        tlg_run.save_settings(self.settings)
        self._perf_applied = getattr(self, '_perf_stamp', 0)
        self._perf_msg = self.t('gfx_perf_applied', base=base, changes='; '.join(lines))
        self.rebuild()
        self.changed.emit()

    def t_pair(self, pair):
        return self.backend.t_.pair(pair)

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
        self.settings['graphics_mode'] = 'imported'     # an imported file is not the personal preset
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
    def autoDlss(self):
        """The upscaler was set to NVIDIA DLSS: fetch at once whatever is missing (NVIDIA's DLL from NVIDIA's
        GitHub, the port's bridge from the port's GitHub releases), no button to press."""
        if self._dlss_busy:
            return
        self._dlss_confirm, self._dlss_busy = False, True
        self._dlss_cancel.clear()
        self._dlss_done = self._dlss_total = 0
        self._dlss_msg, self._dlss_tone = self.t('dlss_auto_working'), 'info'
        target, configured, user = self._dlss_target(), self._dlss_configured(), tlg_run.user_dir()
        probes = self.probes

        def work():
            problems = []
            try:
                state = probes['dlss_status']() if 'dlss_status' in probes else dlss_setup.status(
                    user, configured, tlg_run.V2_PROBE.parent)
                steps = []
                if not state['dll'].ok:
                    steps.append(probes.get('dlss_download', dlss_setup.download))
                if not state['bridge'].ok:
                    steps.append(probes.get('dlss_bridge_download', dlss_setup.download_bridge))
                for step in steps:
                    try:
                        step(target, progress=self.dlssProgressed.emit, cancel=self._dlss_cancel)
                    except dlss_setup.DlssError as error:
                        problems.append(self.t('dlss_err_' + error.key, detail=error.detail))
            except Exception as error:      # never leave the screen stuck on "downloading"
                problems.append(self.t('dlss_err_network', detail=error))
            self.dlssAutoDone.emit(problems)
        threading.Thread(target=work, daemon=True).start()
        self.changed.emit()

    def _on_dlss_auto_done(self, problems):
        self._dlss_busy = False
        if problems:
            self._dlss_msg, self._dlss_tone = self.t('dlss_auto_partial', detail=' '.join(problems)), 'bad'
        else:
            self._dlss_msg, self._dlss_tone = self.t('dlss_auto_ready'), 'good'
        self.changed.emit()
        self._check_dlss()

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
