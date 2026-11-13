# SPDX-License-Identifier: GPL-2.0-or-later
"""Export and import of configuration presets: a .json file players can share (issues, Discord...).

No Qt and no tkinter here: both launchers use it. The file has the layout of a full preset
(presets_current.py) plus a header:

  {"format": "iss-preset", "version": 1, "game": "CUSA00223", "name": "...", "what": "...",
   "exported": "2026-10-16", "port_version": "...", "settings": {...}, "items": {...}}

settings  keys of the launcher's settings.json (EXPORT_SETTINGS)
items     ids of the table in backports_table.py, the value the game will use, as the table stores it

Never exported, because they depend on the machine or are personal: the VRAM budget, window mode,
languages, game folder, paths, saves, and the AMD workarounds (BLOCK1D, BC_TINY, NO_MSAA) left on
"Automatic" (any item without a default is only written when the player chose a value for it).

Applying goes through the same code as the built-in presets (qt/gfxlogic.py: custom_targets and
apply_targets), so an imported file behaves like a full preset.
"""
import json
import math
import time
from collections import namedtuple
from pathlib import Path

import backports_table
from qt import gfxlogic

FORMAT = 'iss-preset'
VERSION = 1
GAME = 'CUSA00223'
SUFFIX = '.iss-preset.json'
MAX_BYTES = 1 << 20

# launcher settings a preset may carry: key -> 'bool' or the allowed values
EXPORT_SETTINGS = {'vsync': 'bool', 'show_fps': 'bool', 'eager_readback': 'bool', 'write_merge': 'bool',
                   'precompile_shaders': 'bool', 'gpu_readbacks': ('0', '1', '2')}
SETTING_DEFAULTS = {'show_fps': False, 'precompile_shaders': True, **gfxlogic.SETTING_DEFAULTS}

# A problem found while reading a file. fatal: nothing can be applied.
Problem = namedtuple('Problem', 'code detail fatal')

TEXT = {
    'ps_export': ('Export configuration…', 'Exportar configuração…'),
    'ps_import': ('Import configuration…', 'Importar configuração…'),
    'ps_export_title': ('Export configuration', 'Exportar configuração'),
    'ps_export_hint': ('Saves your graphics options and fixes in a file you can share. It has nothing of '
                       'your PC (no VRAM budget, window mode, folders or saves).',
                       'Grava suas opções de gráficos e correções num arquivo que você pode compartilhar. Não leva '
                       'nada do seu PC (nem orçamento de VRAM, modo de janela, pastas ou saves).'),
    'ps_name': ('Name', 'Nome'),
    'ps_what': ('Description', 'Descrição'),
    'ps_choose_file': ('Choose the file…', 'Escolher o arquivo…'),
    'ps_cancel': ('Cancel', 'Cancelar'),
    'ps_filter': ('inFAMOUS configuration (*.iss-preset.json *.json)',
                  'Configuração do inFAMOUS (*.iss-preset.json *.json)'),
    'ps_save_title': ('Save the configuration', 'Salvar a configuração'),
    'ps_open_title': ('Choose a configuration file', 'Escolher um arquivo de configuração'),
    'ps_exported': ('Configuration saved: {path}', 'Configuração salva: {path}'),
    'ps_export_failed': ('Could not save the file: {detail}', 'Não foi possível salvar o arquivo: {detail}'),
    'ps_import_title': ('Import configuration', 'Importar configuração'),
    'ps_import_body': ('“{name}”: {n} options will change.', '“{name}”: {n} opções vão mudar.'),
    'ps_import_what': ('Description: {what}', 'Descrição: {what}'),
    'ps_import_nothing': ('“{name}” matches your current options: nothing to change.',
                          '“{name}” é igual às suas opções atuais: nada a mudar.'),
    'ps_apply': ('Apply', 'Aplicar'),
    'ps_close': ('Close', 'Fechar'),
    'ps_imported': ('Configuration “{name}” applied: {n} options changed.',
                    'Configuração “{name}” aplicada: {n} opções mudaram.'),
    'ps_profile_imported': ('Profile: custom ({name})', 'Perfil: personalizado ({name})'),
    'ps_change': ('{label}: {old} → {new}', '{label}: {old} → {new}'),
    'ps_more': ('… and {n} more', '… e mais {n}'),
    'ps_p_unreadable': ('The file cannot be read: {detail}', 'Não foi possível ler o arquivo: {detail}'),
    'ps_p_too_big': ('This is not a configuration file (too big).', 'Isto não é um arquivo de configuração (grande demais).'),
    'ps_p_not_json': ('The file is not valid JSON: {detail}', 'O arquivo não é um JSON válido: {detail}'),
    'ps_p_format': ('This is not an inFAMOUS configuration file (the "format" field should be "iss-preset").',
                    'Isto não é um arquivo de configuração do inFAMOUS (o campo "format" deveria ser "iss-preset").'),
    'ps_p_version': ('Unsupported file version: {detail}.', 'Versão de arquivo sem suporte: {detail}.'),
    'ps_p_newer': ('The file is from a newer version of the port ({detail}). Update the launcher first.',
                   'O arquivo é de uma versão mais nova do port ({detail}). Atualize o launcher antes.'),
    'ps_p_empty': ('The file has no "settings" or "items" to apply.', 'O arquivo não tem "settings" nem "items" para aplicar.'),
    'ps_p_game': ('Warning: the file is for another game ({detail}); this port is for ' + GAME + '.',
                  'Atenção: o arquivo é de outro jogo ({detail}); este port é para ' + GAME + '.'),
    'ps_p_unknown_item': ('Ignored, unknown option: {detail}', 'Ignorada, opção desconhecida: {detail}'),
    'ps_p_unknown_setting': ('Ignored, unknown or machine-specific setting: {detail}',
                             'Ignorada, configuração desconhecida ou da máquina: {detail}'),
    'ps_p_bad_item': ('Refused, invalid value: {detail}', 'Recusada, valor inválido: {detail}'),
    'ps_p_bad_setting': ('Refused, invalid value: {detail}', 'Recusada, valor inválido: {detail}'),
}


# ------------------------------------------------------------------------------------------ export
def _setting_value(settings, key):
    value = settings.get(key, SETTING_DEFAULTS[key])
    return str(value) if key == 'gpu_readbacks' else bool(value)


def export_preset(settings, states, name='', what='', port_version='', exported=None):
    """The preset dict (what goes into the file) for the current launcher settings and fix states.
    `states` as Backend.fix_states: {item id: value or None}. Items left on automatic stay out."""
    items = {}
    for item in backports_table.ITEMS:
        value = backports_table.effective(item, states)
        if value is not None:
            items[item.id] = str(value)
    return {
        'format': FORMAT, 'version': VERSION, 'game': GAME, 'name': str(name).strip(), 'what': str(what).strip(),
        'exported': exported or time.strftime('%Y-%m-%d'), 'port_version': str(port_version or ''),
        'settings': {key: _setting_value(settings, key) for key in EXPORT_SETTINGS},
        'items': items,
    }


def write_preset(preset, path):
    """Writes the file; adds .iss-preset.json when the name has no .json. Returns the final path."""
    path = Path(path)
    if path.suffix.lower() != '.json':
        path = path.with_name(path.name + SUFFIX)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(preset, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    return path


def default_filename(name):
    safe = ''.join(c if c.isalnum() or c in '-_' else '-' for c in str(name).strip().lower()).strip('-')
    return (safe or 'inamous-config') + SUFFIX


# ------------------------------------------------------------------------------------------ import
def _number(value):
    if isinstance(value, bool):
        raise ValueError('bool')
    if isinstance(value, (int, float)):
        number = float(value)
    elif isinstance(value, str):
        number = float(value.strip())
    else:
        raise ValueError('type')
    if not math.isfinite(number):
        raise ValueError('range')
    return number


def canonical_item_value(item, value):
    """The table's string for `value`, or ValueError when the type or range does not fit the item."""
    if item.vtype == 'bool':
        if value in (True, 1, '1'):
            return '1'
        if value in (False, 0, '0'):
            return '0'
        raise ValueError('bool')
    if item.vtype == 'choice':
        if isinstance(value, (str, int)) and not isinstance(value, bool) and str(value) in [c[0] for c in item.choices]:
            return str(value)
        raise ValueError('choice')
    number = _number(value)
    if not item.lo <= number <= item.hi:
        raise ValueError('range')
    if item.vtype == 'int':
        if number != int(number):
            raise ValueError('int')
        return str(int(number))
    return f'{number:g}' if abs(number) < 1e6 else str(int(round(number)))


def _canonical_setting(key, value):
    kind = EXPORT_SETTINGS[key]
    if kind == 'bool':
        if value in (True, 1) and not isinstance(value, str):
            return True
        if value in (False, 0) and not isinstance(value, str):
            return False
        raise ValueError('bool')
    if isinstance(value, (str, int)) and not isinstance(value, bool) and str(value) in kind:
        return str(value)
    raise ValueError('choice')


def read_preset(path):
    """(preset, problems). preset has the layout of presets_current.CURRENT (settings and items
    validated, ready for gfxlogic.custom_targets) or is None when a fatal problem was found.
    Never raises: a broken file gives a fatal Problem."""
    path = Path(path)
    try:
        if path.stat().st_size > MAX_BYTES:
            return None, [Problem('too_big', '', True)]
        data = json.loads(path.read_text(encoding='utf-8'))
    except json.JSONDecodeError as error:
        return None, [Problem('not_json', str(error), True)]
    except (OSError, UnicodeDecodeError, ValueError, RecursionError) as error:
        return None, [Problem('unreadable', str(error) or type(error).__name__, True)]
    return parse_preset(data, path.name[:-len(SUFFIX)] if path.name.lower().endswith(SUFFIX) else path.stem)


def parse_preset(data, profile=''):
    """Same as read_preset for the loaded JSON value. `profile`: the name to show for the profile."""
    if not isinstance(data, dict) or data.get('format') != FORMAT:
        return None, [Problem('format', '', True)]
    version = data.get('version')
    if isinstance(version, bool) or not isinstance(version, int) or version < 1:
        return None, [Problem('version', repr(version), True)]
    if version > VERSION:
        return None, [Problem('newer', str(version), True)]
    problems = []
    game = data.get('game')
    if game != GAME:
        problems.append(Problem('game', str(game), False))
    raw_settings, raw_items = data.get('settings', {}), data.get('items', {})
    if not isinstance(raw_settings, dict) or not isinstance(raw_items, dict) or not (raw_settings or raw_items):
        return None, problems + [Problem('empty', '', True)]
    settings, items = {}, {}
    by_id = {item.id: item for item in backports_table.ITEMS}
    for key, value in raw_settings.items():
        if key not in EXPORT_SETTINGS:
            problems.append(Problem('unknown_setting', str(key), False))
            continue
        try:
            settings[key] = _canonical_setting(key, value)
        except ValueError:
            problems.append(Problem('bad_setting', f'{key} = {_short(value)}', False))
    for item_id, value in raw_items.items():
        item = by_id.get(item_id)
        if item is None:
            problems.append(Problem('unknown_item', str(item_id), False))
            continue
        try:
            items[item_id] = canonical_item_value(item, value)
        except ValueError:
            problems.append(Problem('bad_item', f'{item_id} = {_short(value)}', False))
    name = data.get('name') if isinstance(data.get('name'), str) else ''
    what = data.get('what') if isinstance(data.get('what'), str) else ''
    preset = {'id': 'imported', 'names': {'en': name, 'pt': name}, 'what': {'en': what, 'pt': what},
              'exported': str(data.get('exported', '')), 'game': str(game or ''),
              'port_version': str(data.get('port_version', '')), 'profile': profile or name,
              'settings': settings, 'items': items}
    return preset, problems


def _short(value):
    text = json.dumps(value, ensure_ascii=False) if not isinstance(value, str) else repr(value)
    return text if len(text) <= 40 else text[:37] + '…'


def diff(preset, settings, states):
    """What applying `preset` would change: [(kind, id, old, new)], kind 'setting' | 'item'; values
    are what the game would use (strings; settings as given). Same rule as applying: the targets of
    gfxlogic.custom_targets against the current settings and fix states."""
    want_settings, want_items = gfxlogic.custom_targets(preset)
    changes = []
    for key, wanted in want_settings.items():
        if not gfxlogic.same_setting(key, settings.get(key), wanted):
            changes.append(('setting', key, settings.get(key, SETTING_DEFAULTS.get(key)), wanted))
    by_id = {item.id: item for item in backports_table.ITEMS}
    for item_id, stored in want_items.items():
        if states.get(item_id) != stored:
            item = by_id[item_id]
            changes.append(('item', item_id, backports_table.effective(item, states),
                            backports_table.effective(item, {item_id: stored})))
    return changes


def apply_preset(preset, settings, states):
    """Applies in place through the preset path (gfxlogic.apply_targets); the caller saves."""
    gfxlogic.apply_targets(settings, states, gfxlogic.custom_targets(preset))


# ----------------------------------------------------------------------------------------- texts
def problem_text(problem, t):
    return t('ps_p_' + problem.code, detail=problem.detail)


def _pair(pair, pt, ru=None):
    if ru is not None:
        return ru.get(pair[0], pair[0])
    return pair[1] if pt else pair[0]


def _value_text(item, value, t, pt, ru=None):
    if value is None:
        return t('exp_auto')
    if item is None:
        return t('exp_on') if value is True else t('exp_off') if value is False else str(value)
    if item.vtype == 'bool':
        return t('exp_on') if value == '1' else t('exp_off')
    if item.vtype == 'choice':
        for v, en, pt_name in item.choices:
            if v == value:
                return _pair((en, pt_name), pt, ru)
    return f'{value}{item.unit}'


def change_text(change, t, pt, ru=None):
    kind, key, old, new = change
    if kind == 'setting':
        try:
            label = t('vram' if key == 'vram_budget_mb' else key)
        except KeyError:
            label = key
        if key == 'gpu_readbacks':
            old, new = t('readbacks_' + str(old)), t('readbacks_' + str(new))
            return t('ps_change', label=label, old=old, new=new)
        return t('ps_change', label=label, old=_value_text(None, old, t, pt, ru), new=_value_text(None, new, t, pt, ru))
    item = next(i for i in backports_table.ITEMS if i.id == key)
    label = _pair(item.name, pt, ru)
    return t('ps_change', label=label, old=_value_text(item, old, t, pt, ru), new=_value_text(item, new, t, pt, ru))


def summary_lines(preset, problems, changes, t, pt, limit=8, ru=None):
    """(body, lines) for the import confirmation: counts, the first changes, then every problem."""
    name = preset['profile'] or preset['names']['en'] or '?'
    if changes:
        body = t('ps_import_body', name=name, n=len(changes))
    else:
        body = t('ps_import_nothing', name=name)
    if preset['what']['en']:
        body += '\n' + t('ps_import_what', what=preset['what']['en'])
    lines = [change_text(c, t, pt, ru) for c in changes[:limit]]
    if len(changes) > limit:
        lines.append(t('ps_more', n=len(changes) - limit))
    lines += [problem_text(p, t) for p in problems]
    return body, lines
