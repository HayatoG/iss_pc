#!/usr/bin/env python3
"""Redraws the text panel of the English controller guides in Russian.

Usage: python make_ru.py [folder]
Reads inFAMOUS-controle-<kind>-en-US.png in `folder` (default: this directory) and writes
inFAMOUS-controle-<kind>-ru-RU.png beside it. Deterministic: the panel text, layout and fonts
are fixed here; only the controller art comes from the English image.
"""
import os
import sys
from pathlib import Path

os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')

from PySide6.QtCore import QPointF, QRectF
from PySide6.QtGui import (QColor, QFont, QFontDatabase, QFontMetricsF, QGuiApplication,
                           QImage, QPainter)

HERE = Path(__file__).resolve().parent
FONT_DIR = HERE.parent.parent / 'fonts'
KINDS = ('xbox', 'switch', '8bitdo')
W, H = 1900, 900
PANEL_X, PANEL_BG = 1083, '#121418'   # keep the original antialiased edge at 1079-1082
TX, PANEL_W = 1124, 820
WRAP_W = PANEL_W - 58 - 44 - 10
STEP_Y, STEP_GAP = 214, 86
COL = {1: '#ff6a2b', 2: '#ff3d9a', 3: '#19c8e8', 4: '#4d8bff',
       5: '#f5c400', 6: '#2fc47a', 7: '#b070ff'}
TXT_COL = {'kicker': '#ff6a2b', 'title': '#ffffff', 'sub': '#a3a9b3', 'name': '#f5c400',
           'step': '#ffffff', 'desc': '#b9bec8', 'foot': '#7d838e'}

NAMES = {'xbox': 'Xbox / Xbox Series', 'switch': 'Nintendo Switch Pro', '8bitdo': '8BitDo'}
TXT = {
    'kicker': 'inFAMOUS SECOND SON · PC',
    'title': 'Режим датчика и тачпада',
    'sub': 'Граффити, поглощение способностей, двери и сканер',
    'foot': 'У вашего геймпада нет гироскопа и тачпада: порт имитирует и то, и другое.',
    'steps': {
        1: ('Включить/выключить режим датчика', {
            'xbox': 'Нажмите L3 + R3 (оба стика) или кнопку Share (Xbox Series)',
            'switch': 'Нажмите L3 + R3 (оба стика) или кнопку Capture',
            '8bitdo': 'Нажмите L3 + R3 (оба стика)'}),
        2: ('Встряхнуть баллончик', {
            'xbox': 'Кнопка A (нижняя)', 'switch': 'Нижняя кнопка (B на Switch)',
            '8bitdo': 'Кнопка A (нижняя)'}),
        3: ('Распылить граффити', {
            'xbox': 'Правый триггер (RT)', 'switch': 'Правый триггер (ZR)',
            '8bitdo': 'Правый триггер (R2)'}),
        4: ('Навести баллончик', {
            'xbox': 'Левый стик', 'switch': 'Левый стик', '8bitdo': 'Левый стик'}),
        5: ('Нажать тачпад', {
            'xbox': 'Кнопка View (два квадрата). Удержание = удерживать тачпад (поглотить способность)',
            'switch': 'Кнопка − (минус). Удержание = удерживать тачпад (поглотить способность)',
            '8bitdo': 'Кнопка Select (слева от центра). Удержание = удерживать тачпад'}),
        6: ('Свайп по тачпаду', {
            'xbox': 'Удерживайте кнопку View и двигайте правый стик в нужную сторону («Open Door»)',
            'switch': 'Удерживайте кнопку − и двигайте правый стик в нужную сторону («Open Door»)',
            '8bitdo': 'Удерживайте кнопку Select и двигайте правый стик в нужную сторону («Open Door»)'}),
        7: ('Оба края тачпада', {
            'xbox': 'Удерживайте кнопку View и нажмите RB (поднять машину с Реджи)',
            'switch': 'Удерживайте кнопку − и нажмите R (поднять машину с Реджи)',
            '8bitdo': 'Удерживайте кнопку Select и нажмите R1 (поднять машину с Реджи)'}),
    },
}


def font(family, size, bold=False):
    f = QFont(family)
    f.setPixelSize(size)
    f.setWeight(QFont.Weight.Bold if bold else QFont.Weight.Normal)
    f.setStyleStrategy(QFont.StyleStrategy.PreferAntialias)
    return f


def draw_text(p, x, center_y, text, fnt, color, anchor='lm'):
    fm = QFontMetricsF(fnt)
    baseline = center_y + (fm.ascent() - fm.descent()) / 2.0
    if anchor == 'mm':
        x -= fm.horizontalAdvance(text) / 2.0
    p.setFont(fnt)
    p.setPen(QColor(color))
    p.drawText(QPointF(x, baseline), text)


def wrap(fm, text, width):
    words, lines, cur = text.split(), [], ''
    for word in words:
        trial = (cur + ' ' + word).strip()
        if not cur or fm.horizontalAdvance(trial) <= width:
            cur = trial
        else:
            lines.append(cur)
            cur = word
    if cur:
        lines.append(cur)
    return lines


def load_fonts():
    wanted = ('PT_Sans-Regular.ttf', 'PT_Sans-Bold.ttf',
              'PT_Sans-Narrow-Regular.ttf', 'PT_Sans-Narrow-Bold.ttf')
    for name in wanted:
        if QFontDatabase.addApplicationFont(str(FONT_DIR / name)) < 0:
            raise SystemExit(f'cannot load font {FONT_DIR / name}')


def make(kind, src, out):
    img = QImage(str(src))
    if img.isNull() or (img.width(), img.height()) != (W, H):
        raise SystemExit(f'unexpected source image {src}')
    img = img.convertToFormat(QImage.Format.Format_RGB32)
    p = QPainter(img)
    p.setRenderHint(QPainter.RenderHint.TextAntialiasing, True)
    p.setRenderHint(QPainter.RenderHint.Antialiasing, True)
    p.fillRect(QRectF(PANEL_X, 0, W - PANEL_X, H), QColor(PANEL_BG))

    draw_text(p, TX, 52, TXT['kicker'], font('PT Sans', 18, True), TXT_COL['kicker'])
    draw_text(p, TX, 94, TXT['title'], font('PT Sans', 40, True), TXT_COL['title'])
    draw_text(p, TX, 132, TXT['sub'], font('PT Sans', 20), TXT_COL['sub'])
    draw_text(p, TX, 166, NAMES[kind], font('PT Sans', 20, True), TXT_COL['name'])

    body = font('PT Sans Narrow', 17)
    fm = QFontMetricsF(body)
    y = STEP_Y
    for step in range(1, 8):
        title, descs = TXT['steps'][step]
        p.setPen(QColor(COL[step]))
        p.setBrush(QColor(COL[step]))
        p.drawEllipse(QRectF(TX, y, 40, 40))
        draw_text(p, TX + 20, y + 20, str(step), font('PT Sans', 24, True), '#101010', 'mm')
        draw_text(p, TX + 58, y + 10, title, font('PT Sans', 23, True), TXT_COL['step'])
        yy = y + 44
        for line in wrap(fm, descs[kind], WRAP_W):
            draw_text(p, TX + 58, yy, line, body, TXT_COL['desc'])
            yy += 23
        y = max(y + STEP_GAP, yy + 12)

    draw_text(p, TX, H - 34, TXT['foot'], font('PT Sans Narrow', 16), TXT_COL['foot'])
    p.end()
    if not img.save(str(out), 'PNG'):
        raise SystemExit(f'cannot write {out}')
    return out


def main():
    folder = Path(sys.argv[1]) if len(sys.argv) > 1 else HERE
    app = QGuiApplication.instance() or QGuiApplication([])
    load_fonts()
    for kind in KINDS:
        src = folder / f'inFAMOUS-controle-{kind}-en-US.png'
        out = folder / f'inFAMOUS-controle-{kind}-ru-RU.png'
        print(make(kind, src, out))
    del app


if __name__ == '__main__':
    main()
