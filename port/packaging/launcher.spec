# -*- mode: python ; coding: utf-8 -*-
# PyInstaller spec shared by packaging/linux/package.sh and packaging/windows/package.sh: one folder
# with two launchers that share their libraries: "$PORT_NAME" (Qt Quick, the default) and
# "$PORT_NAME (classic)" (the Tk launcher, the fallback). The scripts pass native paths in the
# environment; PyInstaller options are fixed here because a .spec file ignores most of the CLI.
#   ISS_SPEC_NAME   executable name           ISS_SPEC_ROOT   the port/ folder
#   ISS_SPEC_ICON   .ico (Windows only)       ISS_SPEC_HIDDEN space-separated modules for --script
#   ISS_SPEC_QT     launcher script (Qt)      ISS_SPEC_TK     launcher script (Tk)
import os
import sys
from pathlib import Path

name = os.environ['ISS_SPEC_NAME']
root = Path(os.environ['ISS_SPEC_ROOT'])
icon = os.environ.get('ISS_SPEC_ICON') or None
hidden = os.environ['ISS_SPEC_HIDDEN'].split()
paths = [str(root / 'scripts'), str(root / 'launcher')]

# Qt modules the launcher never imports (it uses QtCore, QtGui, QtQml, QtQuick, QtQuickControls2
# through QML). Keeping them out saves most of the size; the QML runtime still pulls in what it needs.
qt_unused = ['QtWebEngineCore', 'QtWebEngineWidgets', 'QtWebEngineQuick', 'QtWebChannel', 'QtWebSockets',
             'QtMultimedia', 'QtMultimediaWidgets', 'Qt3DCore', 'Qt3DRender', 'Qt3DInput', 'Qt3DLogic',
             'Qt3DAnimation', 'Qt3DExtras', 'QtQuick3D', 'QtPdf', 'QtPdfWidgets', 'QtSql', 'QtCharts',
             'QtDataVisualization', 'QtGraphs', 'QtBluetooth', 'QtNfc', 'QtSensors', 'QtSerialPort',
             'QtSerialBus', 'QtPositioning', 'QtLocation', 'QtRemoteObjects', 'QtScxml', 'QtStateMachine',
             'QtSpatialAudio', 'QtTest', 'QtHelp', 'QtDesigner', 'QtUiTools', 'QtOpenGLWidgets',
             'QtTextToSpeech', 'QtHttpServer', 'QtNetworkAuth', 'QtVirtualKeyboard', 'QtWidgets',
             'QtPrintSupport', 'QtSvgWidgets', 'QtXml', 'QtConcurrent', 'QtDBus', 'QtAxContainer']
qt_excludes = ['PySide6.' + module for module in qt_unused] + ['PySide6.scripts', 'PySide6.support']

# Runtime files of the Qt launcher, next to its modules (qt/app.py reads Path(__file__).parent / 'qml',
# 'fonts', ...): everything under launcher/qt that is not code, so a new asset folder is picked up.
qt_dir = root / 'launcher' / 'qt'
qt_datas = []
for path in sorted(qt_dir.rglob('*')):
    if not path.is_file() or '__pycache__' in path.parts or path.suffix in ('.py', '.pyc') \
            or path.name == 'requirements.txt':
        continue
    qt_datas.append((str(path), str(Path('qt') / path.relative_to(qt_dir).parent)))
qt_datas.append((str(root / 'launcher' / 'infamous.png'), '.'))   # window icon (qt/app.py: qt/../infamous.png)


def analysis(script, datas=(), excludes=()):
    return Analysis([str(root / 'launcher' / script)], pathex=paths, datas=list(datas),
                    hiddenimports=hidden, excludes=list(excludes), noarchive=False)


# Files of the PySide6 wheel (and the system libraries PyInstaller pulled in for them) the launcher
# never loads: other Qt Quick Controls styles (we use Basic), Qt's own translations, QML debugger
# plugins, platform plugins for embedded devices, the GTK3 platform theme (and the GTK stack it
# drags in on Linux), the Wayland compositor and QtTest. Matched on the path inside the bundle.
drop = ('/translations/', '/plugins/qmltooling/', '/plugins/egldeviceintegrations/', '/plugins/generic/',
        '/plugins/platformthemes/libqgtk3', '/plugins/networkinformation/libqconnman',
        '/plugins/platforms/libqeglfs', '/plugins/platforms/libqlinuxfb', '/plugins/platforms/libqminimalegl',
        '/plugins/platforms/libqvkkhr', '/plugins/platforms/libqvnc', '/plugins/platforms/qdirect2d',
        '/plugins/imageformats/libqicns', '/plugins/imageformats/libqtga', '/plugins/imageformats/libqtiff',
        '/plugins/imageformats/libqwbmp', '/plugins/imageformats/libqwebp',
        '/plugins/imageformats/qicns', '/plugins/imageformats/qtga', '/plugins/imageformats/qtiff',
        '/plugins/imageformats/qwbmp', '/plugins/imageformats/qwebp',
        '/qml/QtQuick/Controls/Material', '/qml/QtQuick/Controls/Universal', '/qml/QtQuick/Controls/Fusion',
        '/qml/QtQuick/Controls/Imagine', '/qml/QtQuick/Controls/FluentWinUI3', '/qml/QtQuick/Controls/Windows',
        '/qml/QtQuick/Controls/designer', '/qml/Qt/labs/', '/qml/QtTest', '/qml/QtWayland', '/qml/QtQuick/Particles',
        '/qml/QtQuick/tooling', '/qml/QtQuick/VirtualKeyboard', '/qml/QtQuick/NativeStyle',
        'Qt6QuickControls2Material', 'Qt6QuickControls2Universal', 'Qt6QuickControls2Fusion',
        'Qt6QuickControls2Imagine', 'Qt6QuickControls2FluentWinUI3', 'Qt6LabsStyleKit',
        'Qt6QuickControls2WindowsStyleImpl', 'Qt6QuickParticles', 'Qt6Test',
        'Qt6WaylandCompositor', 'Qt6QuickControls2MaterialStyleImpl',
        'Qt6QuickControls2UniversalStyleImpl', 'Qt6QuickControls2FusionStyleImpl',
        'Qt6QuickControls2ImagineStyleImpl', 'Qt6QuickControls2FluentWinUI3StyleImpl',
        'Qt6Test', 'Qt6QuickControls2Material', 'Qt6QuickControls2Universal',
        'Qt6QuickControls2Fusion', 'Qt6QuickControls2Imagine', 'Qt6QuickControls2FluentWinUI3',
        'Qt6LabsStyleKit', 'Qt6QuickParticles', 'Qt6WaylandCompositor',
        'Qt6Pdf', 'qpdf', 'Qt6VirtualKeyboard', 'libgtk-3.so', 'libgdk-3.so', 'libgdk_pixbuf-2.0.so', 'libglycin', 'libatk-', 'libatspi',
        'libcloudproviders', 'libepoxy', 'libcairo', 'libpango', 'libpangocairo', 'libpangoft2', 'libthai',
        'libdatrie')


# Exceptions to `drop`, matched the same way: dropping a whole folder would take these with it.
# Qt.labs.folderlistmodel is what the non-native FolderDialog falls back to when the platform has
# no native folder dialog (Linux without a portal); without it the dialog never opens (issue #122).
keep_anyway = ('/qml/Qt/labs/folderlistmodel',)


def keep(entry):
    path = '/' + entry[0].replace('\\', '/')
    if sys.platform == 'win32' and path.rsplit('/', 1)[-1].lower() == 'icuuc.dll':
        return False

    def matches(marks):
        return any(mark in path or path.endswith('/' + mark) or path.startswith('/' + mark)
                   for mark in marks)

    return matches(keep_anyway) or not matches(drop)


qt_a = analysis(os.environ['ISS_SPEC_QT'], qt_datas, qt_excludes)
qt_a.binaries = [entry for entry in qt_a.binaries if keep(entry)]
qt_a.datas = [entry for entry in qt_a.datas if keep(entry)]
tk_a = analysis(os.environ['ISS_SPEC_TK'], excludes=['PySide6', 'shiboken6'])


def program(a, exe_name):
    return EXE(PYZ(a.pure), a.scripts, [], exclude_binaries=True, name=exe_name, console=False, icon=icon,
               debug=False, strip=False, upx=False,
               contents_directory=os.environ.get('ISS_SPEC_CONTENTS', '_internal'))


qt_exe = program(qt_a, name)
tk_exe = program(tk_a, name + ' (classic)')
COLLECT(qt_exe, qt_a.binaries, qt_a.datas, tk_exe, tk_a.binaries, tk_a.datas, strip=False, upx=False, name=name)
