# SPDX-License-Identifier: GPL-2.0-or-later
"""Source integration helper for qt.backend.Backend.gameOptions.

Keep the final build's original game options and translated labels. Pass them to
enrich_game_options together with self.settings, the existing run module
(called tlg_run in the tested backend), and the existing trophies module.
This helper reads unlocks; it never writes saves, settings, or trophy files.
"""
from pathlib import Path
from PySide6.QtCore import QUrl

RANKS = {
    'second_son': {
        'gold': {12, 13},
        'silver': {1, 23, 24, 26, 28, 29, 30, 31, 32, 35, 38},
    },
    'first_light': {
        'gold': {3, 4, 9, 12, 13, 20, 21},
        'silver': {2, 5, 7, 11, 14, 15, 16, 17, 18, 19, 23},
    },
}


def enrich_game_options(options, settings, run_module, trophy_module):
    """Return copies of original option dictionaries with UI-only metadata.

    run_module supplies PORT_DIR, userdata.user_dir and GAMES[key]['user_sub'].
    trophy_module supplies GAMES[key]=(title_id,total,names) and earned().
    The active folder lives in settings['game_dir']; inactive folders in
    settings['game_dirs']. Each game's unlock IDs are counted independently.
    """
    base = run_module.userdata.user_dir(run_module.PORT_DIR)
    folders = settings.get('game_dirs') or {}
    active = settings.get('game', 'second_son')
    result = []
    for original in options:
        option = dict(original)
        key = option['value']
        folder = settings.get('game_dir', '') if key == active else folders.get(key, '')
        icon = Path(folder) / 'sce_sys' / 'icon0.png' if folder else None
        try:
            option['iconSource'] = QUrl.fromLocalFile(str(icon.resolve())).toString() if icon and icon.is_file() else ''
        except OSError:
            option['iconSource'] = ''
        title, total, _names = trophy_module.GAMES[key]
        subfolder = run_module.GAMES[key]['user_sub']
        user = base / subfolder if subfolder else base
        medals = dict(platinum=0, gold=0, silver=0, bronze=0)
        try:
            times, _recovered = trophy_module.earned(user, title, total)
            for index in range(total):
                if index not in times:
                    continue
                grade = ('platinum' if index == 0 else
                         'gold' if index in RANKS[key]['gold'] else
                         'silver' if index in RANKS[key]['silver'] else 'bronze')
                medals[grade] += 1
            count = sum(medals.values())
            platinum = bool(medals['platinum'])
            percent = 100 if platinum else min(99, int(count * 100 / total)) if total else 0
            available = True
        except Exception:
            count, platinum, percent, available = 0, False, 0, False
            medals = dict(platinum=0, gold=0, silver=0, bronze=0)
        option.update(trophyEarned=count, trophyTotal=total, trophyPlatinum=platinum,
                      trophyPercent=percent, trophyAvailable=available, trophyMedals=medals)
        result.append(option)
    return result
