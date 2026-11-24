# SPDX-License-Identifier: GPL-2.0-or-later
"""Output and internal resolution of the game's "native rendering" (renderer v2, update 1.07).

The game reads two keys of tlg.ini: iss_native_output (0 = off, 1..7 = a display preset) and
iss_native_percent (25..200, the 3D scene as a percent of that output). The player thinks in
"render 720p, show 1440p", so the launchers offer the internal resolution as a height and store
the percent that gives it.

This file is the same arithmetic as src/iss_native_resolution.h (iss_native_plan,
iss_native_pick, iss_native_step_for_percent, iss_native_retarget). tests/native_resolution_cases.txt
feeds both implementations and tests/test_native_resolution_launcher.py also checks the tables
against the header, so they cannot drift apart. Both launchers (Tk and Qt) call only this module.
The in-game menu (tlg_overlay.cpp, NativeRenderingTab) calls the header.

Texts (TEXT, English and Portuguese) live here too: the Tk table includes them and the Qt
translator falls back to the Tk table, so one entry serves both.
"""
import backports_table

# Index = value of iss_native_output. Index 0 is "off".
OUTPUTS = ((0, 0), (1920, 1080), (2560, 1440), (3440, 1440), (3840, 2160), (5120, 1440),
           (2560, 1080), (3840, 1080))   # 6, 7: the 1080-line ultrawides (2026-11-05), added last
OUTPUT_LABELS = ('', '1080p · 1920×1080', '1440p · 2560×1440', '3440×1440 (21:9)', '4K · 3840×2160',
                 '5120×1440 (32:9)', '2560×1080 (21:9)', '3840×1080 (32:9)')
# Internal resolution steps: scene heights offered to the player.
STEPS = (540, 720, 900, 1080, 1440, 2160)
PERCENT_MIN, PERCENT_MAX = 25, 200
PIXEL_BUDGET = 3840 * 2160      # the game refuses a larger scene at startup

PICK_OK, PICK_BUDGET, PICK_RANGE, PICK_OUTPUT = 1, 0, -1, -2


def scale(dimension, percent):
    """One output dimension scaled by percent, rounded to an even pixel count."""
    return ((dimension * percent + 100) // 200) * 2


def plan(output, percent):
    """(scene width, scene height, output width, output height), or None when the game would
    refuse it (output off or unknown, percent out of range, scene over the pixel budget)."""
    if not 0 < output < len(OUTPUTS) or not PERCENT_MIN <= percent <= PERCENT_MAX:
        return None
    out_w, out_h = OUTPUTS[output]
    width, height = scale(out_w, percent), scale(out_h, percent)
    if not width or not height or width * height > PIXEL_BUDGET:
        return None
    return width, height, out_w, out_h


def pick(output, target_height):
    """(status, percent, width, height, error): the percent whose scene height is closest to
    target_height (the lowest percent on a tie). error = height - target_height, so 0 is exact.
    status: PICK_OK; PICK_BUDGET (the result is over the pixel budget: nothing to store);
    PICK_RANGE (no percent from 25 to 200 gets near); PICK_OUTPUT (output off or unknown)."""
    if not 0 < output < len(OUTPUTS):
        return PICK_OUTPUT, 0, 0, 0, 0
    out_w, out_h = OUTPUTS[output]
    if target_height * 100 < out_h * PERCENT_MIN or target_height * 100 > out_h * PERCENT_MAX:
        return PICK_RANGE, 0, 0, 0, 0
    best = min(range(PERCENT_MIN, PERCENT_MAX + 1), key=lambda p: (abs(scale(out_h, p) - target_height), p))
    width, height = scale(out_w, best), scale(out_h, best)
    status = PICK_OK if plan(output, best) else PICK_BUDGET
    return status, best, width, height, height - target_height


def step_for_percent(output, percent):
    """Index into STEPS that a stored percent stands for on this output, or -1 (hand-edited ini)."""
    for index, height in enumerate(STEPS):
        status, best, *_ = pick(output, height)
        if status in (PICK_OK, PICK_BUDGET) and best == percent:
            return index
    return -1


def retarget(old_output, percent, new_output):
    """The percent to store when the output changes, keeping the same internal height (720p stays
    720p). A percent that is no step, or a result that cannot be used, leaves it as it was."""
    index = step_for_percent(old_output, percent)
    if index < 0:
        return percent
    status, best, *_ = pick(new_output, STEPS[index])
    return best if status == PICK_OK else percent


def exact_outputs(height):
    """[(output, percent)] of the outputs that reach `height` exactly, within the pixel budget."""
    found = []
    for output in range(1, len(OUTPUTS)):
        status, best, _, _, error = pick(output, height)
        if status == PICK_OK and error == 0:
            found.append((output, best))
    return found


# ------------------------------------------------------------------------------------------ tlg.ini
def read(ini):
    """(output, percent) from the keys of tlg.ini (name -> text). A bad value reads as the default
    (off, 100%); the game itself refuses a bad file and runs unchanged."""
    def number(key, low, high, default):
        try:
            value = int(str(ini.get(key, default)).strip())
        except ValueError:
            return default
        return value if low <= value <= high else default
    return (number('iss_native_output', 0, len(OUTPUTS) - 1, 0),
            number('iss_native_percent', PERCENT_MIN, PERCENT_MAX, 100))


def save(path, output, percent):
    """Writes the two keys the game reads and keeps every other line."""
    backports_table.write_ini(path, {'iss_native_output': str(int(output)),
                                     'iss_native_percent': str(int(percent))})


# ------------------------------------------------------------------------------------------ UI model
def output_options(t):
    """[(value text, label)] of the Output choice."""
    return [('0', t('nat_out_off'))] + [(str(n), OUTPUT_LABELS[n]) for n in range(1, len(OUTPUTS))]


def internal_options(t, output, percent):
    """[(value text, label, selectable)] of the Internal resolution choice for this output.
    The value is the scene height; 'custom' stands for a percent that is no step. An entry the
    game would refuse stays in the list, marked, and cannot be chosen."""
    options = []
    for height in STEPS:
        status, best, width, scene_h, error = pick(output, height)
        if status == PICK_OUTPUT:
            label, selectable = f'{height}p', False
        elif status == PICK_RANGE:
            label, selectable = t('nat_step_range', h=height), False
        elif status == PICK_BUDGET:
            label, selectable = t('nat_step_budget', h=height, w=width, sh=scene_h), False
        else:
            key = 'nat_step_exact' if error == 0 else 'nat_step_near'
            label, selectable = t(key, h=height, w=width, sh=scene_h, pct=best), True
        options.append((str(height), label, selectable))
    if output and step_for_percent(output, percent) < 0:
        options.append(('custom', t('nat_step_custom', pct=percent), False))
    return options


def internal_value(output, percent):
    """The value text of the current Internal resolution choice."""
    index = step_for_percent(output, percent) if output else -1
    return str(STEPS[index]) if index >= 0 else 'custom'


def summary(t, output, percent):
    """(text, tone) describing what the next start will do: tone is 'info', 'warn' or 'bad'."""
    if not output:
        return t('nat_sum_off'), 'info'
    out_w, out_h = OUTPUTS[output]
    result = plan(output, percent)
    if result is None:
        width, height = scale(out_w, percent), scale(out_h, percent)
        return t('nat_sum_budget', w=width, sh=height, ow=out_w, oh=out_h, pct=percent), 'bad'
    width, height, _, _ = result
    text = t('nat_sum_ok', w=width, sh=height, ow=out_w, oh=out_h, pct=percent)
    index = step_for_percent(output, percent)
    if index >= 0:
        _, _, _, scene_h, error = pick(output, STEPS[index])
        if error:
            others = exact_outputs(STEPS[index])[:2]
            hint = ', '.join(f'{OUTPUT_LABELS[o]} {p}%' for o, p in others)
            text += ' ' + t('nat_sum_near', h=STEPS[index], err=f'{error:+d}')
            if hint:
                text += ' ' + t('nat_sum_exact_on', h=STEPS[index], where=hint)
            return text, 'warn'
    return text, 'info'


TEXT = {
    'nat_section': ('Output and internal resolution', 'Saída e resolução interna'),
    'nat_output': ('Output', 'Saída'),
    'nat_output_what': (
        'The size of the image shown on screen. "Off" keeps the original game (1080p inside the game, scaled by the window). '
        'Needs renderer v2 and the US update 1.07.',
        'O tamanho da imagem mostrada na tela. "Desligado" mantém o jogo original (1080p dentro do jogo, ampliado pela janela). '
        'Precisa do renderer v2 e da atualização 1.07 americana.'),
    'nat_internal': ('Internal resolution', 'Resolução interna'),
    'nat_internal_what': (
        'The size the 3D scene is drawn at before it is shown at the output size. Lower is faster: for example 720p '
        'inside a 1440p output. The menu and HUD are drawn at the output size.',
        'O tamanho em que a cena 3D é desenhada antes de ser mostrada no tamanho da saída. Menor é mais rápido: por exemplo, '
        '720p dentro de uma saída 1440p. O menu e o HUD são desenhados no tamanho da saída.'),
    'nat_out_off': ('Off (original game)', 'Desligado (jogo original)'),
    'nat_step_exact': ('{h}p · {w}×{sh}', '{h}p · {w}×{sh}'),
    'nat_step_near': ('{h}p · about {w}×{sh} ({pct}%)', '{h}p · cerca de {w}×{sh} ({pct}%)'),
    'nat_step_custom': ('Custom ({pct}%)', 'Personalizada ({pct}%)'),
    'nat_step_range': ('{h}p · out of range for this output', '{h}p · fora do alcance desta saída'),
    'nat_step_budget': ('{h}p · {w}×{sh} is over the 4K limit', '{h}p · {w}×{sh} passa do limite de 4K'),
    'nat_sum_off': ('Original game rendering. Pick an output to render natively.',
                    'Renderização do jogo original. Escolha uma saída para renderizar nativamente.'),
    'nat_sum_ok': ('Scene {w}×{sh} → output {ow}×{oh} ({pct}%).',
                   'Cena {w}×{sh} → saída {ow}×{oh} ({pct}%).'),
    'nat_sum_near': ('Not exact: {h}p is not a whole percent of this output ({err} px).',
                     'Não é exato: {h}p não é um percentual inteiro desta saída ({err} px).'),
    'nat_sum_exact_on': ('Exactly {h}p on: {where}.', 'Exatamente {h}p em: {where}.'),
    'nat_sum_budget': (
        'Scene {w}×{sh} ({pct}% of {ow}×{oh}) is over the 4K pixel limit, so the game would refuse it and start unchanged. '
        'Choose a lower internal resolution.',
        'A cena {w}×{sh} ({pct}% de {ow}×{oh}) passa do limite de pixels de 4K; o jogo a recusaria e abriria sem mudança. '
        'Escolha uma resolução interna menor.'),
    'nat_restart': ('Takes effect the next time the game starts.', 'Vale na próxima vez que o jogo iniciar.'),
    'nat_needs_v2': (
        'Needs renderer v2 (the default on Windows): choose it under Renderer. With this port\'s renderer the game keeps its original resolution.',
        'Precisa do renderer v2 (o padrão no Windows): escolha-o em Renderer. Com o renderer deste port o jogo mantém a resolução original.'),
    'nat_refused': ('{h}p cannot be used with this output.', '{h}p não pode ser usado com esta saída.'),
}
