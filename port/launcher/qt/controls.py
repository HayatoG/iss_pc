# SPDX-License-Identifier: GPL-2.0-or-later
"""The QObject behind the Controls screen: stick dead zone and graffiti aiming speed, the detected
controller, the mouse (tlg.ini keys the game reads) with the touchpad mode, and the keyboard map.
The logic and every key name live in padsettings.py (checked against the game's code)."""
from PySide6.QtCore import Property, QObject, Signal, Slot

import padsettings

from . import gfxlogic
from .logic import backports_table, tlg_run
from .rowsmodel import RowsModel

# ids: 'c:' launcher setting, 'm:' tlg.ini mouse key, 'p:' touchpad-mode setting, item ids of the table.
MOUSE_ROWS = ('mouse_sensitivity', 'mouse_sensitivity_y', 'mouse_invert_y', 'mouse_smoothing', 'mouse_accel', 'mouse_deadzone')
PAD_KEY_LABELS = {'': 'ctl_key_default', 'Left Ctrl': 'ctl_key_lctrl', 'Mouse X1': 'ctl_key_x1',
                  'Mouse X2': 'ctl_key_x2', 'Right Ctrl|Mouse X1': 'ctl_key_rctrl_x1'}


class Controls(QObject):
    changed = Signal()
    draftChanged = Signal()
    scrollRequested = Signal(int)   # used by the screenshot tool

    def __init__(self, backend):
        super().__init__(backend)
        self.backend = backend
        self.pad_model, self.mouse_model, self.touch_model = RowsModel(self), RowsModel(self), RowsModel(self)
        self._draft = str(backend.settings.get('key_map', '') or '')
        backend.profileChanged.connect(self.rebuild)
        backend.langChanged.connect(self.rebuild)
        backend.chipsChanged.connect(self.changed)          # a controller was plugged in
        self.rebuild()

    # -- helpers --------------------------------------------------------------------------------------
    def t(self, key, **fmt):
        return self.backend.t_(key, **fmt)

    @property
    def settings(self):
        return self.backend.settings

    def ini_path(self):
        return tlg_run.user_dir() / 'tlg.ini'

    def ini(self):
        return tlg_run.read_ini(self.ini_path())

    def mouse_on(self):
        return padsettings.mouse_value(self.ini(), 'mouse_enabled')

    # -- rows ---------------------------------------------------------------------------------------------
    def _row(self, rid, label, what, control, live, changed, **extra):
        row = {'kind': 'item', 'id': rid, 'code': '', 'label': label, 'what': what,
               'tags': [{'text': self.t('exp_live' if live else 'exp_restart'), 'tone': 'info'}],
               'shaderWarn': '', 'changed': changed, 'control': control, 'on': False, 'options': [], 'index': 0,
               'lo': 0, 'hi': 0, 'step': 1, 'unit': '', 'value': 0, 'valueText': '', 'disabled': False,
               'decimals': 0, 'note': '', 'info': '', 'action': None}
        row.update(extra)
        return row

    def _pad_rows(self):
        zone = int(self.settings.get('stick_deadzone', 10))
        rows = [self._row('c:stick_deadzone', self.t('deadzone'), self.t('deadzone_hint'), 'slider', False,
                          zone != tlg_run.DEFAULT_SETTINGS['stick_deadzone'], lo=0, hi=40, step=1, unit='%',
                          value=zone, valueText=f'{zone}%')]
        item = gfxlogic.item_by_id('menu:sensor_aim')
        value = int(backports_table.effective(item, self.backend.fix_states))
        rows.append(self._row(item.id, self.backend.t_.pair(item.name), self.backend.t_.pair(item.what), 'slider',
                              True, self.backend.fix_states.get(item.id) is not None, lo=item.lo, hi=item.hi,
                              step=5, unit=item.unit, value=value, valueText=f'{value}{item.unit}'))
        return rows

    def _mouse_rows(self):
        ini, off = self.ini(), not self.mouse_on()
        rows = []
        for key in MOUSE_ROWS:
            _, kind, default, _, slide = padsettings.MOUSE_SPEC[key]
            value = padsettings.mouse_value(ini, key)
            what = self.t('ctl_what_' + key)
            label = self.t('ctl_' + key)
            changed = key in ini and padsettings.mouse_update(key, value)[key] is not None
            if kind == 'bool':
                rows.append(self._row('m:' + key, label, what, 'switch', True, changed, on=value, disabled=off))
            else:
                value = min(max(value, slide[0]), slide[1])
                rows.append(self._row('m:' + key, label, what, 'slider', True, changed, lo=slide[0], hi=slide[1],
                                      step=0.05, value=value,
                                      valueText=f'{value:.2f}x' if key == 'mouse_sensitivity_y' else f'{value:.2f}', disabled=off, decimals=2))
        return rows

    def _touch_rows(self):
        s = self.settings
        key = str(s.get('mouse_pad_key', '') or '')
        keys = list(padsettings.PAD_KEY_CHOICES)
        options = [{'value': k, 'label': self.t(PAD_KEY_LABELS[k])} for k in keys]
        if key not in keys:
            options.append({'value': key, 'label': self.t('ctl_key_custom', keys=key)})
        sens = float(s.get('mouse_pad_sens', 1.0))
        drag = int(s.get('mouse_pad_drag', 80))
        on = s.get('mouse_pad', True) is not False
        d = padsettings.PAD_DEFAULTS
        modes = [{'value': m, 'label': self.t('ctl_mode_' + m)} for m in padsettings.PAD_MODES]
        off = not self.mouse_on()
        return [
            self._row('p:mouse_pad', self.t('ctl_mouse_pad'), self.t('ctl_what_mouse_pad'), 'switch', False,
                      on != d['mouse_pad'], on=on, disabled=off),
            self._row('p:mouse_pad_key', self.t('ctl_mouse_pad_key'), self.t('ctl_what_mouse_pad_key'), 'combo',
                      False, key != d['mouse_pad_key'], options=options,
                      index=next(i for i, o in enumerate(options) if o['value'] == key), disabled=off or not on),
            self._row('p:mouse_pad_mode', self.t('ctl_mouse_pad_mode'), self.t('ctl_what_mouse_pad_mode'), 'combo',
                      False, s.get('mouse_pad_mode', 'hold') != d['mouse_pad_mode'], options=modes,
                      index=1 if s.get('mouse_pad_mode') == 'toggle' else 0, disabled=off or not on),
            self._row('p:mouse_pad_sens', self.t('ctl_mouse_pad_sens'), self.t('ctl_what_mouse_pad_sens'), 'slider',
                      False, abs(sens - 1.0) > 1e-6, lo=0.1, hi=5.0, step=0.1, value=min(max(sens, 0.1), 5.0),
                      valueText=f'{sens:.1f}', disabled=off or not on, decimals=1),
            self._row('p:mouse_pad_drag', self.t('ctl_mouse_pad_drag'), self.t('ctl_what_mouse_pad_drag'), 'slider',
                      False, drag != d['mouse_pad_drag'], lo=0, hi=200, step=10, value=drag,
                      valueText=self.t('ctl_drag_never') if drag == 0 else str(drag), disabled=off or not on),
        ]

    def rebuild(self):
        self.pad_model.set_rows(self._pad_rows())
        self.mouse_model.set_rows(self._mouse_rows())
        self.touch_model.set_rows(self._touch_rows())
        self.changed.emit()

    # -- writing --------------------------------------------------------------------------------------------
    @Slot(str, str)
    def setValue(self, rid, value):
        s = self.settings
        if rid == 'c:stick_deadzone':
            s['stick_deadzone'] = min(max(int(float(value)), 0), 40)
            tlg_run.save_settings(s)
        elif rid.startswith('m:'):
            key = rid[2:]
            kind = padsettings.MOUSE_SPEC[key][1]
            update = padsettings.mouse_update(key, value in ('1', 'true') if kind == 'bool' else float(value))
            backports_table.write_ini(self.ini_path(), update)
        elif rid.startswith('p:'):
            key = rid[2:]
            if key == 'mouse_pad':
                s[key] = value == '1'
            elif key == 'mouse_pad_sens':
                s[key] = round(min(max(float(value), 0.1), 5.0), 2)
            elif key == 'mouse_pad_drag':
                s[key] = min(max(int(float(value)), 0), 200)
            else:
                s[key] = value
            tlg_run.save_settings(s)
        else:
            self.backend.setFix(rid, value)       # the graffiti aiming speed: same path as every table item
            return
        self.rebuild()

    @Slot()
    def toggleMouse(self):
        on = self.mouse_on()
        backports_table.write_ini(self.ini_path(), padsettings.mouse_update('mouse_enabled', not on))
        self.rebuild()

    # -- controller ------------------------------------------------------------------------------------------------
    @Property(str, notify=changed)
    def padName(self):
        return self.backend._pad or self.t('ctl_pad_none')

    @Property(str, notify=changed)
    def padKind(self):
        return padsettings.pad_kind(self.backend._pad)

    @Property(str, notify=changed)
    def padKindText(self):
        kind = self.padKind
        return self.t('ctl_kind_' + kind)

    @Property(str, notify=changed)
    def padHint(self):
        return self.t('ctl_hint_' + self.padKind) if self.padFound else ''

    @Property(bool, notify=changed)
    def padFound(self):
        return bool(self.backend._pad)

    @Property(QObject, constant=True)
    def padRows(self):
        return self.pad_model

    # -- mouse ---------------------------------------------------------------------------------------------------------
    @Property(QObject, constant=True)
    def mouseRows(self):
        return self.mouse_model

    @Property(QObject, constant=True)
    def touchRows(self):
        return self.touch_model

    @Property(bool, notify=changed)
    def mouseEnabled(self):
        return self.mouse_on()

    @Property(str, notify=changed)
    def mouseButtonText(self):
        return self.t('ctl_mouse_turn_on' if not self.mouse_on() else 'ctl_mouse_turn_off')

    # -- keyboard --------------------------------------------------------------------------------------------------------
    @Property('QVariantList', notify=changed)
    def keyRows(self):
        keys = padsettings.effective_keys(str(self.settings.get('key_map', '') or ''))
        default = padsettings.effective_keys('')
        rows = [{'name': self.t('pad_left_stick'), 'keys': 'W A S D', 'fixed': True, 'changed': False},
                {'name': self.t('pad_right_stick'), 'keys': '← ↑ → ↓', 'fixed': True, 'changed': False}]
        for button, _, _ in padsettings.KEYBOARD_BUTTONS:
            names = keys[button]
            rows.append({'name': self.t('pad_btn_' + button), 'keys': ' | '.join(names) if names else self.t('ctl_key_none'),
                         'fixed': False, 'changed': names != default[button]})
        rows += [{'name': self.t('pad_sensor_right'), 'keys': 'T', 'fixed': True, 'changed': False},
                 {'name': self.t('pad_sensor_left'), 'keys': 'G', 'fixed': True, 'changed': False},
                 {'name': self.t('pad_sensor_shake'), 'keys': 'Y', 'fixed': True, 'changed': False}]
        return rows

    @Property(str, notify=changed)
    def savedKeyMap(self):
        return str(self.settings.get('key_map', '') or '')

    def draft_info(self):
        entries, errors, warnings = padsettings.parse_key_map(self._draft)
        messages = [{'tone': 'bad', 'text': self.t(code, detail=detail)} for code, detail in errors]
        messages += [{'tone': 'warn', 'text': self.t(code, detail=detail)} for code, detail in warnings]
        return entries, errors, messages

    @Property(str, notify=draftChanged)
    def draftText(self):
        return self._draft

    @Slot(str)
    def setDraft(self, text):
        if text != self._draft:
            self._draft = text
            self.draftChanged.emit()

    @Property('QVariantList', notify=draftChanged)
    def draftMessages(self):
        entries, errors, messages = self.draft_info()
        if not errors and self._draft.strip():
            messages.insert(0, {'tone': 'good', 'text': self.t('keymap_ok', n=len(entries))})
        return messages

    @Property(bool, notify=draftChanged)
    def draftValid(self):
        return not self.draft_info()[1]

    @Property(bool, notify=draftChanged)
    def draftDirty(self):
        return self._draft.strip() != self.savedKeyMap

    @Slot()
    def applyKeyMap(self):
        if self.draftValid:
            text = self._draft.strip()
            if text:
                self.settings['key_map'] = text
            else:
                self.settings.pop('key_map', None)
            tlg_run.save_settings(self.settings)
            self.draftChanged.emit()
            self.changed.emit()

    @Slot()
    def clearKeyMap(self):
        self._draft = ''
        self.settings.pop('key_map', None)
        tlg_run.save_settings(self.settings)
        self.draftChanged.emit()
        self.changed.emit()
