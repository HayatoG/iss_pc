# SPDX-License-Identifier: GPL-2.0-or-later
"""Keyboard, mouse and touchpad-mode settings of the game, without Qt (the Controls screen uses it;
infamous_run.build_env hands the environment ones to the game).

Every name here was checked in the game's code:
  - port/src/runtime_pad.c       key_map (default keyboard), ps_names, apply_button_map (TLG_KEY_MAP:
                                 "<ps4 button>=<key>[|<key>...]", comma separated, "none"), the mouse
                                 sources, load_pad_mode (TLG_MOUSE_PAD, _MODE, _SENS, _DRAG, _KEY)
  - port/gpu/shim/tlg_settings.cpp  the tlg.ini keys mouse_enabled, mouse_sensitivity, mouse_sensitivity_y, mouse_invert_y,
                                 mouse_smoothing, mouse_accel, mouse_deadzone (and their ranges)
The mouse code is on the feat/mouse-camera branch; with a game build that does not have it the keys
are written to tlg.ini and the variables are exported, and the game ignores them.
The game reads ISS_* like TLG_* (the probe copies one to the other), so the launcher exports ISS_*.
"""

# ------------------------------------------------------------------------- default keyboard map
# (PS4 button id, SDL scancode constant in runtime_pad.c, key name as the player types it)
KEYBOARD_BUTTONS = (
    ('cross', 'SPACE', 'Space'), ('circle', 'LSHIFT', 'Left Shift'), ('square', 'E', 'E'),
    ('triangle', 'Q', 'Q'), ('l1', '1', '1'), ('r1', '3', '3'), ('l2', 'R', 'R'), ('r2', 'F', 'F'),
    ('l3', 'Z', 'Z'), ('r3', 'C', 'C'), ('options', 'RETURN', 'Enter'),
    ('up', 'I', 'I'), ('left', 'J', 'J'), ('down', 'K', 'K'), ('right', 'L', 'L'),
    ('touchpad_left', 'TAB', 'Tab'), ('touchpad_right', 'BACKSPACE', 'Backspace'),
)
# Keys that are not remappable (read straight from the keyboard state in runtime_pad.c):
# (id, keys). The sensor keys T/G/Y, the sticks and the extra touchpad gestures.
FIXED_KEYS = (
    ('left_stick', 'W A S D'), ('right_stick', '← ↑ → ↓'),
    ('sensor_right', 'T'), ('sensor_left', 'G'), ('sensor_shake', 'Y'),
)
PS4_BUTTONS = ('cross', 'circle', 'square', 'triangle', 'l1', 'r1', 'l2', 'r2', 'l3', 'r3', 'options',
               'touchpad', 'touchpad_left', 'touchpad_right', 'up', 'down', 'left', 'right')
MOUSE_SOURCES = ('Mouse Left', 'Mouse Middle', 'Mouse Right', 'Mouse X1', 'Mouse X2',
                 'Wheel Up', 'Wheel Down', 'Wheel Left', 'Wheel Right')
# Key names SDL3 knows (SDL_GetScancodeName); the check is case-insensitive in the game. Not
# exhaustive: a name outside this list is only a warning, the game decides with SDL itself.
KEY_NAMES = frozenset(
    [chr(c) for c in range(ord('A'), ord('Z') + 1)] + [str(n) for n in range(10)] +
    [f'F{n}' for n in range(1, 25)] + [f'Keypad {n}' for n in range(10)] +
    ['Return', 'Escape', 'Backspace', 'Tab', 'Space', '-', '=', '[', ']', '\\', '#', ';', "'", '`', ',', '.',
     '/', 'CapsLock', 'PrintScreen', 'ScrollLock', 'Pause', 'Insert', 'Home', 'PageUp', 'Delete', 'End',
     'PageDown', 'Right', 'Left', 'Down', 'Up', 'Numlock', 'Keypad /', 'Keypad *', 'Keypad -', 'Keypad +',
     'Keypad Enter', 'Keypad .', 'Keypad =', 'Menu', 'Application',
     'Left Ctrl', 'Left Shift', 'Left Alt', 'Left GUI', 'Right Ctrl', 'Right Shift', 'Right Alt', 'Right GUI'])
_KEY_NAMES_LOWER = {name.lower() for name in KEY_NAMES}
MAX_SPEC = 1023        # runtime_pad.c copies the value into char copy[1024]
MAX_SOURCES = 8        # int sources[8] per entry


def parse_key_map(spec):
    """Checks a TLG_KEY_MAP value the way apply_button_map reads it.
    Returns (entries, errors, warnings): entries = [(ps4 button, [keys] or None for "none")];
    errors = [(code, detail)] that make the game ignore an entry or the whole value; warnings are
    key names this launcher cannot confirm (the game asks SDL). `code` is a Qt string suffix."""
    entries, errors, warnings = [], [], []
    spec = spec or ''
    if len(spec) > MAX_SPEC:
        errors.append(('keymap_err_long', str(MAX_SPEC)))
        return entries, errors, warnings
    for raw in spec.split(','):
        entry = raw.strip()
        if not entry:
            continue
        if '=' not in entry:
            errors.append(('keymap_err_format', entry))
            continue
        button, _, names = entry.partition('=')
        button, names = button.strip().lower(), names.strip()
        if button not in PS4_BUTTONS:
            errors.append(('keymap_err_button', button))
            continue
        if names.lower() == 'none':
            entries.append((button, None))
            continue
        keys = []
        for name in names.split('|'):
            name = name.strip()
            if not name:
                continue
            if name.lower() not in _KEY_NAMES_LOWER and name.lower() not in {m.lower() for m in MOUSE_SOURCES}:
                warnings.append(('keymap_warn_key', name))
            keys.append(name)
        if not keys:
            errors.append(('keymap_err_empty', button))
        elif len(keys) > MAX_SOURCES:
            errors.append(('keymap_err_many', button))
        else:
            entries.append((button, keys))
    return entries, errors, warnings


def effective_keys(spec):
    """{ps4 button: [keys]} for the remappable buttons: the default keyboard with the (valid)
    entries of `spec` applied; an empty list is a button left without any key ("none")."""
    keys = {button: [name] for button, _, name in KEYBOARD_BUTTONS}
    keys['touchpad'] = keys['touchpad_left']          # alias in ps_names
    entries, _, _ = parse_key_map(spec)
    for button, names in entries:
        if button == 'touchpad':
            button = 'touchpad_left'
        keys[button] = list(names or [])
    return keys


# ------------------------------------------------------------------------------- mouse (tlg.ini)
# key, kind, default text, hard range of the game, range of the sliders
MOUSE_KEYS = (
    ('mouse_enabled', 'bool', '1', None, None),
    ('mouse_sensitivity', 'float', '1.00', (0.05, 10.0), (0.1, 10.0)),
    ('mouse_sensitivity_y', 'float', '1.00', (0.25, 4.0), (0.25, 4.0)),
    ('mouse_invert_y', 'bool', '0', None, None),
    ('mouse_smoothing', 'float', '0.50', (0.0, 0.95), (0.0, 0.9)),
    ('mouse_accel', 'float', '0.00', (0.0, 2.0), (0.0, 2.0)),
    ('mouse_deadzone', 'float', '0.10', (0.0, 0.5), (0.0, 0.5)),
)
MOUSE_SPEC = {row[0]: row for row in MOUSE_KEYS}


def mouse_value(ini, key):
    """The value the game will use for a mouse key of tlg.ini: bool for the switches, float otherwise."""
    _, kind, default, hard, _ = MOUSE_SPEC[key]
    text = str(ini.get(key, default)).strip()
    try:
        if kind == 'bool':
            return int(float(text)) != 0
        return min(max(float(text), hard[0]), hard[1])
    except ValueError:
        return mouse_value({}, key)


def mouse_update(key, value):
    """{key: text or None} to hand to backports_table.write_ini: None removes the key (default again)."""
    _, kind, default, hard, _ = MOUSE_SPEC[key]
    if kind == 'bool':
        text = '1' if value else '0'
    else:
        text = f'{min(max(float(value), hard[0]), hard[1]):.2f}'
    return {key: None if text == default else text}


# ------------------------------------------------------- touchpad mode and key map (environment)
PAD_DEFAULTS = {'mouse_pad': True, 'mouse_pad_key': '', 'mouse_pad_mode': 'hold',
                'mouse_pad_sens': 1.0, 'mouse_pad_drag': 80}
PAD_DEFAULT_KEY = 'Left Ctrl|Mouse X1'     # runtime_pad.c load_pad_mode
PAD_KEY_CHOICES = ('', 'Left Ctrl', 'Mouse X1', 'Mouse X2', 'Right Ctrl|Mouse X1')   # '' = default
PAD_MODES = ('hold', 'toggle')
PAD_SENS_RANGE = (0.1, 10.0)
PAD_DRAG_RANGE = (0, 200)    # 0 = a drag never releases the click


def environment(settings):
    """ISS_* variables for the settings that differ from the game's defaults ({} when none does)."""
    env = {}
    if settings.get('mouse_pad', True) is False:
        env['ISS_MOUSE_PAD'] = '0'
    key = str(settings.get('mouse_pad_key', '') or '').strip()
    if key and key != PAD_DEFAULT_KEY:
        env['ISS_MOUSE_PAD_KEY'] = key
    if settings.get('mouse_pad_mode', 'hold') == 'toggle':
        env['ISS_MOUSE_PAD_MODE'] = 'toggle'
    try:
        sens = float(settings.get('mouse_pad_sens', 1.0))
        drag = int(settings.get('mouse_pad_drag', 80))
    except (TypeError, ValueError):
        sens, drag = 1.0, 80
    if abs(sens - 1.0) > 1e-6:
        env['ISS_MOUSE_PAD_SENS'] = f'{min(max(sens, PAD_SENS_RANGE[0]), PAD_SENS_RANGE[1]):.2f}'
    if drag != 80:
        env['ISS_MOUSE_PAD_DRAG'] = str(min(max(drag, PAD_DRAG_RANGE[0]), PAD_DRAG_RANGE[1]))
    key_map = str(settings.get('key_map', '') or '').strip()
    if key_map:
        _, errors, _ = parse_key_map(key_map)
        if not errors:        # a value the game would reject stays in the box but is not exported
            env['ISS_KEY_MAP'] = key_map
    return env


def pad_kind(name):
    """'playstation' | 'xbox' | 'switch' | 'other' | 'none' from the label launcher/qt/logic.detect_pad gives."""
    if not name:
        return 'none'
    if name in ('DualSense', 'DualShock 4'):
        return 'playstation'
    return {'Xbox': 'xbox', 'Switch Pro': 'switch'}.get(name, 'other')
