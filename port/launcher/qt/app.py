# SPDX-License-Identifier: GPL-2.0-or-later
"""Qt Quick application: fonts, the hero image provider, the QML engine and the window."""
import math
import sys
from pathlib import Path

from PySide6.QtCore import QPointF, QRectF, QSize, Qt, QUrl
from PySide6.QtGui import (QBrush, QColor, QFontDatabase, QGuiApplication, QIcon, QImage, QLinearGradient, QPainter,
                           QPen, QRadialGradient)
from PySide6.QtQml import QQmlApplicationEngine
from PySide6.QtQuick import QQuickImageProvider

from . import logic
from .backend import Backend
from .controls import Controls
from .graphics import Graphics

HERE = Path(__file__).resolve().parent
QML_ROOT = HERE / 'qml'
FONT_DISPLAY = 'Big Shoulders Stencil Display'
FONT_TEXT = 'Barlow'
# Cyrillic families for the Russian interface (Barlow and Big Shoulders have no Cyrillic glyphs).
FONT_DISPLAY_RU = 'PT Sans Narrow'
FONT_TEXT_RU = 'PT Sans'


def load_fonts():
    """Loads the OFL fonts shipped in qt/fonts; returns (display, text, display_ru, text_ru),
    falling back to the system's sans-serif when a font file is missing or does not load."""
    families = {}
    for path in sorted((HERE / 'fonts').glob('*.ttf')):
        ident = QFontDatabase.addApplicationFont(str(path))
        if ident >= 0:
            for name in QFontDatabase.applicationFontFamilies(ident):
                families[name] = True
    display = FONT_DISPLAY if FONT_DISPLAY in families else 'sans-serif'
    text = FONT_TEXT if FONT_TEXT in families else 'sans-serif'
    return (display, text,
            FONT_DISPLAY_RU if FONT_DISPLAY_RU in families else display,
            FONT_TEXT_RU if FONT_TEXT_RU in families else text)


class HeroProvider(QQuickImageProvider):
    """image://hero/bg: Seattle at night in neon and rain, used when the player's pic1.png is
    absent (and under it while it loads). Drawn here so it needs no art file."""

    def __init__(self):
        super().__init__(QQuickImageProvider.ImageType.Image)

    def requestImage(self, ident, size, requested):
        w, h = (requested.width(), requested.height()) if requested.isValid() else (884, 392)
        w, h = max(w, 1), max(h, 1)
        if requested.isValid() is False:
            w, h = 884, 392
        img = QImage(w, h, QImage.Format.Format_ARGB32_Premultiplied)
        img.fill(Qt.transparent)
        p = QPainter(img)
        p.setRenderHint(QPainter.Antialiasing)
        base = QLinearGradient(0, 0, 0, h)
        base.setColorAt(0, QColor('#171a21'))
        base.setColorAt(1, QColor('#0c0d10'))
        p.fillRect(0, 0, w, h, base)
        self.glow(p, w, h, 0.72 * w, 0.30 * h, 0.60 * w, 0.70 * h, QColor(255, 106, 43), 0.55)
        self.glow(p, w, h, 0.20 * w, 0.90 * h, 0.50 * w, 0.60 * h, QColor(25, 227, 255), 0.22)
        # rain: 1px lines every 11px, tilted 14 degrees from the vertical (104deg gradient)
        p.setOpacity(0.7)
        p.setPen(QPen(QColor(255, 255, 255, 12), 1))
        slope = math.tan(math.radians(14))
        step = 11 / math.cos(math.radians(14))
        x = -h * slope
        while x < w + h * slope:
            p.drawLine(QPointF(x, 0), QPointF(x + h * slope, h))
            x += step
        p.end()
        return img

    @staticmethod
    def glow(p, w, h, cx, cy, rx, ry, color, alpha):
        """Elliptical radial glow: alpha at the center, fading out at 62% of the radius."""
        p.save()
        p.translate(cx, cy)
        p.scale(1.0, ry / rx)
        gradient = QRadialGradient(QPointF(0, 0), rx)
        c0, c1 = QColor(color), QColor(color)
        c0.setAlphaF(alpha)
        c1.setAlphaF(0)
        gradient.setColorAt(0, c0)
        gradient.setColorAt(0.62, c1)
        p.fillRect(QRectF(-w * 2, -h * 4, w * 4, h * 8), QBrush(gradient))
        p.restore()


class App:
    """Everything the window needs; `show()` for the real launcher, `grab()` for screenshots."""

    def __init__(self, backend=None, argv=None):
        self.qt = QGuiApplication.instance() or QGuiApplication(argv or sys.argv)
        self.qt.setApplicationName('infamous-launcher')
        self.qt.setDesktopFileName('infamous-pc')
        self.backend = backend or Backend()
        self.backend.fonts = load_fonts()
        icon = HERE.parent / 'infamous.png'
        if icon.is_file():
            self.qt.setWindowIcon(QIcon(str(icon)))
        self.graphics = Graphics(self.backend, self.backend.probes.get('graphics'))
        self.controls = Controls(self.backend)
        self.engine = QQmlApplicationEngine()
        self.engine.addImageProvider('hero', HeroProvider())
        self.engine.addImportPath(str(QML_ROOT))
        self.engine.rootContext().setContextProperty('Backend', self.backend)
        self.engine.rootContext().setContextProperty('Saves', self.backend.saves)
        self.engine.rootContext().setContextProperty('Graphics', self.graphics)
        self.engine.rootContext().setContextProperty('PadControls', self.controls)
        self.engine.rootContext().setContextProperty('Support', self.backend.support)
        self.engine.rootContext().setContextProperty('Updates', self.backend.updates)
        self.engine.loadFromModule('Launcher', 'Window')
        if not self.engine.rootObjects():
            raise RuntimeError('the QML interface did not load (see the messages above)')
        self.window = self.engine.rootObjects()[0]

    def go(self, index):
        self.window.setProperty('current', index)

    def grab(self, path):
        """PNG of the current window contents (QQuickWindow.grabWindow)."""
        from PySide6.QtCore import QEventLoop, QTimer
        loop = QEventLoop()                       # let transitions (<= 250 ms) finish
        QTimer.singleShot(450, loop.quit)
        loop.exec()
        image = self.window.grabWindow()
        return image.save(str(path))

    def run(self):
        self.backend.start_probes()
        self.graphics.start()
        code = self.qt.exec()
        self.backend.shutdown()
        return code
