# SPDX-License-Identifier: GPL-2.0-or-later
"""Russian plural forms. Strings carry the three forms separated by `|` (one|few|many):
"{n} сохранение|{n} сохранения|{n} сохранений". English and Portuguese keep their own one/many
keys and have no `|`, so pick() returns them unchanged."""


def form(n):
    """0 = one, 1 = few, 2 = many, for the Russian counting rules."""
    n = abs(int(n))
    if n % 10 == 1 and n % 100 != 11:
        return 0
    if 2 <= n % 10 <= 4 and not 12 <= n % 100 <= 14:
        return 1
    return 2


def pick(text, n):
    parts = text.split('|')
    return parts[form(n)] if len(parts) == 3 else text
