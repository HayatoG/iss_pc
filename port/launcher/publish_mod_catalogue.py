# SPDX-License-Identifier: GPL-2.0-or-later
"""Call immediately before launching the active game, after selecting its context."""
import json, os, tempfile
from pathlib import Path

def publish_mod_catalogue(run, settings):
    try:
        game = getattr(run, '_game', settings.get('game', 'second_son'))
        rows = [dict(key=key, name=name or key, note=note or '', active=bool(enabled))
                for key, name, note, enabled in run.list_mods(settings)]
        data = dict(schema=1, game=game, label=run.GAMES[game]['name'],
                    config=str(run.mods_config().resolve()), mods=rows)
        folder = run.user_dir();folder.mkdir(parents=True, exist_ok=True)
        fd, temp = tempfile.mkstemp(prefix='.overlay-mods-', dir=folder)
        try:
            with os.fdopen(fd, 'w', encoding='utf-8') as stream:
                json.dump(data, stream, ensure_ascii=False);stream.flush();os.fsync(stream.fileno())
            os.replace(temp, folder / 'overlay-mods.json')
        finally:
            Path(temp).unlink(missing_ok=True)
    except Exception as error:
        print('Overlay mods: catalogue unavailable:', error, flush=True)
