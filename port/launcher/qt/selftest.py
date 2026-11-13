# SPDX-License-Identifier: GPL-2.0-or-later
"""`--selftest`: proves a (frozen) launcher can open. Loads fonts, translations and the QML on an
offscreen platform, visits every page, renders one frame, prints 'selftest ok' and returns 0.
It never reads the player's settings or saves, never starts the game and never writes outside a
temporary folder."""
import os
import sys
import tempfile
from pathlib import Path


def say(text, error=False):
    """print() does nothing in a windowed Windows exe (no console), so the result also goes to the
    file named by ISS_SELFTEST_OUT, which the CI reads."""
    stream = sys.stderr if error else sys.stdout
    if stream is not None:
        print(text, file=stream)
    target = os.environ.get('ISS_SELFTEST_OUT')
    if target:
        with open(target, 'a', encoding='utf-8') as handle:
            handle.write(text + '\n')


def run():
    # Everything the launcher could touch is pointed at a throw-away folder before importing it.
    sandbox = tempfile.mkdtemp(prefix='iss-selftest-')
    os.environ['QT_QPA_PLATFORM'] = os.environ.get('QT_QPA_PLATFORM') or 'offscreen'
    os.environ.setdefault('QT_QUICK_CONTROLS_STYLE', 'Basic')
    os.environ.setdefault('QT_QUICK_BACKEND', 'software')  # no GPU or GL driver needed on a CI runner
    os.environ['ISS_USER_DIR'] = os.path.join(sandbox, 'user')
    os.environ['XDG_CONFIG_HOME'] = sandbox
    os.environ['APPDATA'] = sandbox
    os.environ['ISS_QT_REDUCED_MOTION'] = '1'
    problems = []

    def check(ok, text):
        if not ok:
            problems.append(text)
        return ok

    try:
        from PySide6.QtCore import QCoreApplication
        from qt import app as app_mod, logic
        from qt.backend import Backend
        import infamous_run as tlg_run
    except Exception as error:  # a missing Qt module or Python module in the bundle
        say(f'selftest FAILED: import error: {error!r}', True)
        return 2
    here = app_mod.HERE
    check((here / 'qml' / 'Launcher' / 'qmldir').is_file(), f'missing QML module: {here / "qml" / "Launcher" / "qmldir"}')
    check(len(list((here / 'qml' / 'Launcher').glob('*.qml'))) > 5, 'QML files missing')
    for font in ('Barlow-Regular.ttf', 'Barlow-Bold.ttf', 'BigShouldersStencilDisplay.ttf',
                 'PT_Sans-Regular.ttf', 'PT_Sans-Bold.ttf', 'PT_Sans-Narrow-Regular.ttf', 'PT_Sans-Narrow-Bold.ttf'):
        check((here / 'fonts' / font).is_file(), f'missing font: {font}')
    check((here.parent / 'infamous.png').is_file(), 'missing window icon: infamous.png')
    try:
        strings = logic.load_strings()
        for key, entry in strings.items():
            if not (isinstance(entry, dict) and all(entry.get(lang) for lang in ('pt', 'en', 'ru'))):
                check(False, f'translation incomplete: {key}')
                break
        check(len(strings) > 20, 'translations missing (i18n/strings.json)')
        pairs = logic.load_pairs()
        check(len(pairs) > 200, 'Russian pair translations missing (i18n/pairs_ru.json)')
        check(all(text.strip() for text in pairs.values()), 'empty Russian pair translation')
    except Exception as error:
        check(False, f'translations did not load: {error!r}')
    if problems:
        return fail(problems)

    try:
        settings = dict(tlg_run.DEFAULT_SETTINGS, ui_language='en_US', game_dir=os.path.join(sandbox, 'game'))
        probes = {'gpu': lambda: ('selftest', '0'), 'pad': lambda: None, 'problems': lambda: [],
                  'cache': lambda: 0, 'save': lambda: None, 'version': lambda: 'selftest',
                  'save_fixes': lambda s, st: None,
                  'saves': {'running': lambda: False, 'scan': lambda: []}}
        backend = Backend(settings=settings, probes=probes)
        app = app_mod.App(backend=backend)
        check(all(family != 'sans-serif' for family in app.backend.fonts),
              f'fonts did not load: {app.backend.fonts}')
        pages = app.window.property('pages') if app.window.metaObject().indexOfProperty('pages') >= 0 else None
        count = len(pages) if pages else 7
        for index in range(count):
            app.go(index)
            QCoreApplication.processEvents()
        image = None
        app.go(0)
        from PySide6.QtCore import QEventLoop, QTimer
        loop = QEventLoop()
        QTimer.singleShot(300, loop.quit)
        loop.exec()
        image = app.window.grabWindow()
        check(not image.isNull() and image.width() > 100 and image.height() > 100, 'no frame was rendered')
        if not image.isNull():
            colors = {image.pixel(x, y) for x in range(0, image.width(), 37) for y in range(0, image.height(), 29)}
            check(len(colors) > 3, 'the rendered frame is blank')
        backend.shutdown()
    except Exception as error:
        check(False, f'the interface did not open: {error!r}')
    if problems:
        return fail(problems)
    say('selftest ok')
    return 0


def fail(problems):
    for text in problems:
        say(f'selftest FAILED: {text}', True)
    return 1
