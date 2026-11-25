"""Builds qt/data/guide_spots.json: for each controller guide image, the legend rows (right panel) and
the marker regions of each number (left picture), found by colour. Run with the launcher's --script."""
import sys, json
from collections import deque
from PySide6.QtGui import QImage
base = r'D:\games\test\verify-sunny-beta-11-22b\infamous-windows-x86_64\_internal\qt\assets\controller_guides'
S, PANEL = 2, 1080
def near(a, b, d):
    return not (a[2] + d < b[0] or b[2] + d < a[0] or a[3] + d < b[1] or b[3] + d < a[1])
res = {}
for kind in ('xbox', 'switch', '8bitdo'):
    for lang in ('en-US', 'pt-BR'):
        name = f'inFAMOUS-controle-{kind}-{lang}.png'
        img = QImage(f'{base}\\{name}').convertToFormat(QImage.Format_RGB32)
        w, h = img.width(), img.height()
        rows = []
        for bx in range(PANEL + 30, PANEL + 100, 2):
            ys = [y for y in range(h) if img.pixelColor(bx, y).saturation() > 110 and img.pixelColor(bx, y).value() > 150]
            runs = []
            if ys:
                start = prev = ys[0]
                for y in ys[1:]:
                    if y - prev > 3: runs.append((start, prev)); start = y
                    prev = y
                runs.append((start, prev))
            runs = [r for r in runs if 15 < r[1] - r[0] < 60]
            if len(runs) == 7:
                rows = [(bx, a, b) for a, b in runs]; break
        assert len(rows) == 7, name
        cols = [img.pixelColor(bx, (a + b) // 2 - 8) for bx, a, b in rows]
        cols = [(c.red(), c.green(), c.blue()) for c in cols]
        gw, gh = PANEL // S, h // S
        px = [[img.pixelColor(x * S, y * S) for x in range(gw)] for y in range(gh)]
        spots = {}
        for n, (r, g, b) in enumerate(cols, 1):
            mask = [[(c.red() - r) ** 2 + (c.green() - g) ** 2 + (c.blue() - b) ** 2 < 55 ** 2 for c in row] for row in px]
            seen = [[False] * gw for _ in range(gh)]; comps = []
            for y in range(gh):
                for x in range(gw):
                    if mask[y][x] and not seen[y][x]:
                        q = deque([(x, y)]); seen[y][x] = True; xs = []; ys2 = []
                        while q:
                            cx, cy = q.popleft(); xs.append(cx); ys2.append(cy)
                            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                                nx, ny = cx + dx, cy + dy
                                if 0 <= nx < gw and 0 <= ny < gh and mask[ny][nx] and not seen[ny][nx]:
                                    seen[ny][nx] = True; q.append((nx, ny))
                        if len(xs) >= 10:
                            comps.append([min(xs) * S, min(ys2) * S, max(xs) * S + S, max(ys2) * S + S, len(xs)])
            # the number badges (about 28 px circles) anchor a marker; rings and lines joined to them belong to it
            kept = [c for c in comps if c[2] - c[0] <= 30 and c[3] - c[1] <= 30 and c[2] - c[0] >= 18 and c[4] >= 70]
            # a ring (a big circle around a stick or button) is a marker even when its badge is hidden under another
            kept += [c for c in comps if c not in kept and c[2] - c[0] >= 60 and c[3] - c[1] >= 60 and c[4] >= 250]
            changed = True
            while changed:
                changed = False
                for c in comps:
                    if c not in kept and any(near(c, k, 25) for k in kept):
                        kept.append(c); changed = True
            spots[str(n)] = [c[:4] for c in kept]
        res[name] = {'size': [w, h], 'panel': PANEL,
                     'rows': [{'n': i + 1, 'y0': a, 'y1': b} for i, (_, a, b) in enumerate(rows)],
                     'colors': ['#%02x%02x%02x' % c for c in cols], 'spots': spots}
        print(name, {k: len(v) for k, v in spots.items()})
json.dump(res, open(sys.argv[-1], 'w', encoding='utf-8'), indent=1)
