# SPDX-License-Identifier: GPL-2.0-or-later
"""Trophies for the launcher's Play tab (Second Son 2026-11-11, First Light 2026-11-12): the icons from
the player's own game (sce_sys/trophy/trophy00.trp, extracted once into <data folder>/trophies/<title>/
icons), which ones are earned (the game's trophy record the port keeps: all-saves.json, else
earned.json) and the names and requirements of port/src/trophy_catalogue.h (the in-game menu's text)."""
import json
import struct
from pathlib import Path

TITLE = 'CUSA00223'
COUNT = 48

# inFAMOUS First Light (CUSA00575): port/src/trophy_catalogue.h FirstLightCatalogue.
FIRST_LIGHT_CATALOGUE = (
    ("Noble Achievement", "Earn every other trophy."),
    ("Lost", "Suffer a loss (story)."),
    ("Cracked", "Endure a deception (story)."),
    ("Betrayed", "Experience a double-cross (story)."),
    ("She's Ready", "Complete the story."),
    ("Illuminated", "Collect every Neon Lumen."),
    ("Blackout", "Destroy every police drone."),
    ("Lickety Pronto", "Complete every Lumen Race."),
    ("That's So Fetch", "Complete every Neon Graffiti."),
    ("Unleashed", "Buy every upgrade."),
    ("Novice", "Complete 25% of the challenges."),
    ("Professional", "Complete 50% of the challenges."),
    ("Expert", "Complete 75% of the challenges."),
    ("Perfectionist", "Complete every challenge."),
    ("Alpha Rescue Diamond", "Score 500,000 points in Alpha Rescue."),
    ("Alpha Survival Diamond", "Score 500,000 points in Alpha Survival."),
    ("Beta Rescue Diamond", "Score 500,000 points in Beta Rescue."),
    ("Beta Survival Diamond", "Score 500,000 points in Beta Survival."),
    ("Gamma Survival Diamond", "Score 500,000 points in Gamma Survival."),
    ("Turret Syndrome", "Overload 25 turrets."),
    ("Great Responsibility", "Save 50 hostages."),
    ("Top of the Class", "Earn 10,000,000 points in total."),
    ("Event Horizon", "Defeat 100 enemies with a Neon Singularity."),
    ("...Bada Boom!", "Defeat 20 enemies in a row with Homing Missiles."),
    ("Fish in a Barrel", "Get 25 Stasis + weak point kills."),
)

# (name, requirement) by zero-based trophy ID: port/src/trophy_catalogue.h.
CATALOGUE = (
    ("Enjoy Your Powers", "Earn every other trophy."),
    ("Unstoppable", "Complete the campaign on Expert."),
    ("Sacrifice", "Choose to protect others despite personal risk."),
    ("Ruthless", "Put your own safety first."),
    ("Temperance", "Choose restraint."),
    ("Malice", "Choose aggression over restraint."),
    ("Courage", "Choose to defend vulnerable people."),
    ("Contempt", "Choose to prey on the vulnerable."),
    ("Mercy", "Choose forgiveness over revenge."),
    ("Revenge", "Choose retaliation over forgiveness."),
    ("Justice", "Choose to reveal the powerful's wrongdoing."),
    ("Dominance", "Choose domination over justice."),
    ("Reconciliation", "Complete the Good-karma story ending."),
    ("Rebellion", "Complete the Evil-karma story ending."),
    ("Everybody Out", "Free ten suspicion-pen prisoners."),
    ("Spin-off", "Beat up ten sign twirlers."),
    ("Everyone's a Critic", "Defeat ten street musicians."),
    ("Just Say No", "Stop ten drug dealers."),
    ("Freedom of Speech", "Break up ten activist gatherings."),
    ("What Did You Call Me?", "Kill ten taunting Akuran gangsters."),
    ("Someone Your Own Size", "Save ten escaped conduits from mobs."),
    ("Walk It Off", "Treat ten injured civilians."),
    ("Kicking and Screaming", "Execute ten injured enemies or civilians."),
    ("True Hero", "Reach Good karma rank five."),
    ("Infamous", "Reach Evil karma rank five."),
    ("Bweep! Bweep! Bweep!", "Collect 50% of Blast Shards."),
    ("Shardcore", "Collect every Blast Shard."),
    ("Nothing To See Here", "Break 50% of D.U.P. security cameras."),
    ("Blinded", "Break every D.U.P. security camera."),
    ("Inside Track", "Find every informant recording."),
    ("Blown Cover", "Eliminate every D.U.P. secret agent."),
    ("Big Brother", "Break every D.U.P. hidden camera."),
    ("Coloring Inside the Lines", "Finish every stencil artwork."),
    ("Prank Call", "Win one District Showdown."),
    ("Hello Operator", "Win showdowns across half the districts."),
    ("Final Showdown", "Win every District Showdown."),
    ("One Down", "Completely liberate one district."),
    ("Home Field Advantage", "Completely liberate half the districts."),
    ("Clean Sweep", "Completely liberate Seattle."),
    ("Total CARnage", "Wreck five vehicles within five seconds."),
    ("Drop Everything", "Perform a Comet Drop exceeding 170 metres."),
    ("Keep It Up", "Remain airborne beyond 45 seconds."),
    ("Put That Anywhere", "Kill ten enemies with exploding ammunition crates."),
    ("Hat Trick", "Defeat one enemy using three power types."),
    ("Double Smoke Stack Attack", "Smoke Dash through two red vents without landing; Comet Drop an enemy."),
    ("Cough Drop", "Comet Drop onto three coughing enemies."),
    ("Make Fetch Proud", "Strike three weak points using Neon Beam within six seconds."),
    ("Excessive Force", "Kill exactly one enemy with a Karmic Streak."),
)


# Portuguese (Brazil) requirement lines (2026-11-24), same order as the catalogues above. The trophy
# names stay the game's own names.
FIRST_LIGHT_REQUIREMENTS_PT = (
    "Conquiste todos os outros troféus.",
    "Sofra uma perda (história).",
    "Passe por uma traição (história).",
    "Sofra uma traição dupla (história).",
    "Termine a história.",
    "Colete todos os Lúmens de Neon.",
    "Destrua todos os drones da polícia.",
    "Termine todas as Corridas de Lúmen.",
    "Termine todos os Grafites de Neon.",
    "Compre todas as melhorias.",
    "Complete 25% dos desafios.",
    "Complete 50% dos desafios.",
    "Complete 75% dos desafios.",
    "Complete todos os desafios.",
    "Faça 500.000 pontos no Resgate Alfa.",
    "Faça 500.000 pontos na Sobrevivência Alfa.",
    "Faça 500.000 pontos no Resgate Beta.",
    "Faça 500.000 pontos na Sobrevivência Beta.",
    "Faça 500.000 pontos na Sobrevivência Gama.",
    "Sobrecarregue 25 torretas.",
    "Salve 50 reféns.",
    "Faça 10.000.000 de pontos no total.",
    "Derrote 100 inimigos com uma Singularidade de Neon.",
    "Derrote 20 inimigos seguidos com Mísseis Teleguiados.",
    "Faça 25 abates com Stasis + ponto fraco.",
)

REQUIREMENTS_PT = (
    "Conquiste todos os outros troféus.",
    "Termine a campanha no Expert.",
    "Escolha proteger os outros mesmo correndo risco.",
    "Coloque a sua própria segurança em primeiro lugar.",
    "Escolha a moderação.",
    "Escolha a agressão em vez da moderação.",
    "Escolha defender pessoas indefesas.",
    "Escolha atacar os indefesos.",
    "Escolha o perdão em vez da vingança.",
    "Escolha a retaliação em vez do perdão.",
    "Escolha revelar os crimes dos poderosos.",
    "Escolha a dominação em vez da justiça.",
    "Termine o final da história com karma bom.",
    "Termine o final da história com karma mau.",
    "Liberte dez prisioneiros das celas de suspeitos.",
    "Espanque dez rodadores de placa.",
    "Derrote dez músicos de rua.",
    "Pare dez traficantes de drogas.",
    "Desfaça dez reuniões de ativistas.",
    "Mate dez gângsteres Akurans que provocam você.",
    "Salve dez condutores fugitivos das multidões.",
    "Socorra dez civis feridos.",
    "Execute dez inimigos ou civis feridos.",
    "Chegue ao nível cinco de karma bom.",
    "Chegue ao nível cinco de karma mau.",
    "Colete 50% dos Fragmentos de Explosão.",
    "Colete todos os Fragmentos de Explosão.",
    "Quebre 50% das câmeras de segurança do D.U.P.",
    "Quebre todas as câmeras de segurança do D.U.P.",
    "Encontre todas as gravações de informantes.",
    "Elimine todos os agentes secretos do D.U.P.",
    "Quebre todas as câmeras escondidas do D.U.P.",
    "Termine todas as artes de estêncil.",
    "Vença um Confronto de Distrito.",
    "Vença confrontos em metade dos distritos.",
    "Vença todos os Confrontos de Distrito.",
    "Liberte completamente um distrito.",
    "Liberte completamente metade dos distritos.",
    "Liberte Seattle por completo.",
    "Destrua cinco veículos em cinco segundos.",
    "Faça uma Queda de Cometa de mais de 170 metros.",
    "Fique no ar por mais de 45 segundos.",
    "Mate dez inimigos com caixas de munição explosivas.",
    "Derrote um inimigo usando três tipos de poder.",
    "Faça o Smoke Dash por duas saídas vermelhas sem pousar; depois uma Queda de Cometa num inimigo.",
    "Faça uma Queda de Cometa em três inimigos tossindo.",
    "Acerte três pontos fracos com o Raio de Neon em seis segundos.",
    "Mate exatamente um inimigo com uma Sequência Kármica.",
)

assert len(REQUIREMENTS_PT) == len(CATALOGUE) and len(FIRST_LIGHT_REQUIREMENTS_PT) == len(FIRST_LIGHT_CATALOGUE)

REQUIREMENTS_BY_LANG = {'pt': {'second_son': REQUIREMENTS_PT, 'first_light': FIRST_LIGHT_REQUIREMENTS_PT}}


def requirement(game, index, english, portuguese=False):
    """The requirement line in the launcher's language (English when there is no translation)."""
    table = REQUIREMENTS_BY_LANG['pt'].get(game) if portuguese else None
    return table[index] if table and 0 <= index < len(table) else english


def _trp_icons(data):
    """{id: PNG bytes} of the TROPnnn.PNG entries of a trophy archive (same checks as trophy_store.cpp)."""
    if (len(data) < 96 or struct.unpack_from('>I', data, 0)[0] != 0xdca24d00 or
            struct.unpack_from('>Q', data, 8)[0] != len(data)):
        raise ValueError('unsupported or damaged trophy archive')
    count, stride = struct.unpack_from('>II', data, 16)
    if not 0 < count <= 1024 or stride != 64 or count > (len(data) - 96) // 64:
        raise ValueError('invalid trophy entry table')
    icons = {}
    for i in range(count):
        p = 96 + i * 64
        name = data[p:p + 32].split(b'\0', 1)[0].decode('ascii', 'replace')
        offset, size = struct.unpack_from('>QQ', data, p + 32)
        if offset > len(data) or size > len(data) - offset:
            raise ValueError('trophy payload outside the archive')
        if name.startswith('TROP') and name.endswith('.PNG') and name[4:7].isdigit() and len(name) == 11:
            png = data[offset:offset + size]
            if png[:8] == b'\x89PNG\r\n\x1a\n':
                icons[int(name[4:7])] = png
    return icons


GAMES = {   # launcher game key -> (trophy ledger title, trophy count, names)
    'second_son': (TITLE, COUNT, CATALOGUE),
    'first_light': ('CUSA00575', 25, FIRST_LIGHT_CATALOGUE),
}


def icon_files(game_dir, user_dir, title=TITLE, count=COUNT):
    """[path of the icon of trophy 0..count-1] extracted from the game's archive into the data folder
    (done again only when the archive changes); [] when the game has no readable archive."""
    archive = Path(game_dir) / 'sce_sys' / 'trophy' / 'trophy00.trp'
    folder = Path(user_dir) / 'trophies' / title / 'icons'
    try:
        stamp = f'{archive.stat().st_size}:{archive.stat().st_mtime_ns}'
    except OSError:
        return []
    paths = [folder / f'TROP{i:03d}.PNG' for i in range(count)]
    marker = folder / 'source.txt'
    try:
        if marker.read_text(encoding='utf-8') == stamp and all(p.is_file() for p in paths):
            return paths
    except OSError:
        pass
    try:
        icons = _trp_icons(archive.read_bytes())
    except (OSError, ValueError, struct.error):
        return []
    if any(i not in icons for i in range(count)) or count + 1 in icons or count in icons:
        return []   # not this game's trophy set
    folder.mkdir(parents=True, exist_ok=True)
    for i, path in enumerate(paths):
        path.write_bytes(icons[i])
    marker.write_text(stamp, encoding='utf-8')
    return paths


def earned(user_dir, title=TITLE, count=COUNT):
    """({id: unix time}, {recovered ids}) from the trophy record: all-saves.json (every save counts,
    the in-game menu's view) when it exists, else earned.json. Nothing earned when unreadable."""
    folder = Path(user_dir) / 'trophies' / title
    for name in ('all-saves.json', 'earned.json'):
        try:
            data = json.loads((folder / name).read_text(encoding='utf-8'))
        except (OSError, ValueError):
            continue
        if not isinstance(data, dict) or data.get('title_id') != title:
            continue
        times = {}
        for row in data.get('earned') or []:
            if isinstance(row, dict) and isinstance(row.get('id'), int) and 0 <= row['id'] < count:
                times[row['id']] = int(row.get('time') or 0)
        recovered = {i for i in data.get('recovered') or [] if isinstance(i, int)}
        return times, recovered
    return {}, set()


def showcase(game_dir, user_dir, game='second_son'):
    """[{'id', 'name', 'requirement', 'earned', 'time', 'recovered', 'icon'}] for the game's trophies,
    or [] when the icons cannot be read from the game."""
    title, count, names = GAMES[game]
    icons = icon_files(game_dir, user_dir, title, count)
    if not icons:
        return []
    times, recovered = earned(user_dir, title, count)
    return [{'id': i, 'name': names[i][0], 'requirement': names[i][1], 'earned': i in times,
             'time': times.get(i, 0), 'recovered': i in recovered, 'icon': str(icons[i])}
            for i in range(count)]
