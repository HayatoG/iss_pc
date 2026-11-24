"""Puts a clean controller photo into the guides' clean pictures (2026-11-23): the photo is scaled and
moved so its controller covers the one in the guide (bounding boxes of the dark controller body), so
the number layers (tools-guide_layers_build.py) still sit on the right buttons. A fake "transparent"
checkerboard background (light greys joined to the border) is removed first.
  "inFAMOUS Second Son.exe" --script tools-guide_photo_build.py <controller_guides folder> <kind> <photo.png>"""
import sys, os
from collections import deque
from PySide6.QtGui import QImage, QPainter, QColor
from PySide6.QtCore import QRectF, Qt

guides, kind, photo = sys.argv[-3], sys.argv[-2], sys.argv[-1]
PANEL = 1080

def bbox(img, pred, x1=None):
    xs, ys = [], []
    for y in range(0, img.height(), 2):
        for x in range(0, x1 or img.width(), 2):
            if pred(img.pixelColor(x, y)):
                xs.append(x); ys.append(y)
    return min(xs), min(ys), max(xs), max(ys)

src = QImage(photo).convertToFormat(QImage.Format_ARGB32)
w, h = src.width(), src.height()
# background: light, grey pixels reachable from the border become transparent
light = lambda c: c.alpha() < 20 or (c.lightness() > 195 and c.saturation() < 40)
seen = bytearray(w * h); q = deque()
for x in range(w):
    for y in (0, h - 1):
        q.append((x, y))
for y in range(h):
    for x in (0, w - 1):
        q.append((x, y))
while q:
    x, y = q.popleft()
    i = y * w + x
    if seen[i]:
        continue
    seen[i] = 1
    c = src.pixelColor(x, y)
    if not light(c):
        continue
    src.setPixelColor(x, y, QColor(0, 0, 0, 0))
    for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        nx, ny = x + dx, y + dy
        if 0 <= nx < w and 0 <= ny < h and not seen[ny * w + nx]:
            q.append((nx, ny))

for lang in ('en-US', 'pt-BR'):
    name = f'inFAMOUS-controle-{kind}-{lang}'
    clean_path = os.path.join(guides, 'layers', name + '-clean.png')
    guide = QImage(os.path.join(guides, name + '.png')).convertToFormat(QImage.Format_ARGB32)
    old_clean = QImage(clean_path)
    ob = bbox(old_clean, lambda c: c.lightness() < 150, PANEL)
    nb = bbox(src, lambda c: c.alpha() > 128 and c.lightness() < 150)
    sx = (ob[2] - ob[0]) / (nb[2] - nb[0]); sy = (ob[3] - ob[1]) / (nb[3] - nb[1])
    s = (sx + sy) / 2
    cx_o, cy_o = (ob[0] + ob[2]) / 2, (ob[1] + ob[3]) / 2
    cx_n, cy_n = (nb[0] + nb[2]) / 2, (nb[1] + nb[3]) / 2
    target = QRectF(cx_o - cx_n * s, cy_o - cy_n * s, w * s, h * s)
    out = QImage(guide)                      # the legend panel stays the guide's own
    p = QPainter(out)
    p.setRenderHint(QPainter.SmoothPixmapTransform)
    p.setRenderHint(QPainter.Antialiasing)
    p.fillRect(0, 0, PANEL, out.height(), QColor(255, 255, 255))
    p.drawImage(target, src)
    p.end()
    out.save(clean_path)
    print(name, 'scale %.3f' % s, 'old box', ob, 'photo box', nb)
