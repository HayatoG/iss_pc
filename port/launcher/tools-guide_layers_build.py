"""Builds the controller guides' hover layers (2026-11-23), from the shipped guide images and
qt/data/guide_spots.json (tools-guide_spots_build.py):
  layers/<image>-clean.png   the guide with every numbered marker erased (filled from the pixels around)
  layers/<image>-<n>.png     only number n's markers, transparent elsewhere
and adds 'clean' / 'layers' to guide_spots.json. Run with the launcher's --script:
  "inFAMOUS Second Son.exe" --script tools-guide_layers_build.py <controller_guides folder> <guide_spots.json>"""
import sys, json, os
from PySide6.QtGui import QImage

guides, spots_path = sys.argv[-2], sys.argv[-1]
table = json.load(open(spots_path, encoding='utf-8'))
out_dir = os.path.join(guides, 'layers')
os.makedirs(out_dir, exist_ok=True)

def hexrgb(h):
    return int(h[1:3], 16), int(h[3:5], 16), int(h[5:7], 16)

for name, info in table.items():
    img = QImage(os.path.join(guides, name)).convertToFormat(QImage.Format_ARGB32)
    w, h = img.width(), img.height()
    data = bytearray(img.constBits().tobytes())          # B G R A per pixel
    stride = img.bytesPerLine()
    panel = info['panel']
    cols = [hexrgb(c) for c in info['colors']]

    def px(x, y):
        i = y * stride + 4 * x
        return data[i + 2], data[i + 1], data[i]

    owner = {}                                            # (x, y) -> set of numbers whose layer holds it
    for n in range(1, 8):
        r0, g0, b0 = cols[n - 1]
        for b in info['spots'].get(str(n), []):
            x0, y0, x1, y1 = b
            bw, bh = x1 - x0, y1 - y0
            cx, cy = (x0 + x1) / 2, (y0 + y1) / 2
            badge = bw <= 32 and bh <= 32
            pr, pg, pb = px(int(cx), int(cy))
            # a label (RT, ZR, R2): a box on the white page; its corners are page white
            corners = [px(min(panel - 1, max(0, xx)), min(h - 1, max(0, yy))) for xx, yy in ((x0 - 3, y0 - 3), (x1 + 2, y0 - 3), (x0 - 3, y1 + 2), (x1 + 2, y1 + 2))]
            inside = [px(xx, yy) for yy in range(y0 + 2, y1 - 1, 3) for xx in range(x0 + 2, min(panel, x1 - 1), 3)]
            label = not badge and bw < 140 and bh < 100 and sum(min(c) > 230 for c in inside) > 0.3 * max(1, len(inside))
            ring = not badge and not label and bw >= 60 and bh >= 60
            pad = 12 if badge else 4
            for y in range(max(0, y0 - pad), min(h, y1 + pad)):
                for x in range(max(0, x0 - pad), min(panel, x1 + pad)):
                    if badge:
                        rad = max(bw, bh) / 2 + 3.5
                        d2 = (x - cx) ** 2 + (y - cy) ** 2
                        take = d2 <= rad * rad
                        if not take and d2 <= (rad + 14) ** 2:
                            # a badge half hidden under another: its white rim and digit reach further
                            r, g, bb = px(x, y)
                            take = min(r, g, bb) > 205 and max(r, g, bb) - min(r, g, bb) < 30 and x > 0
                            if take:   # but not the page background around the controller
                                take = any(sum(px(min(panel - 1, max(0, x + ox)), min(h - 1, max(0, y + oy)))) < 300
                                           for ox, oy in ((7, 0), (-7, 0), (0, 7), (0, -7)))
                    elif label:
                        take = True
                    else:
                        r, g, bb = px(x, y)
                        take = (r - r0) ** 2 + (g - g0) ** 2 + (bb - b0) ** 2 < 70 ** 2
                        if take and ring:
                            # only the ring's own band: a stick's coloured light inside it stays (8BitDo)
                            d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
                            take = d >= min(bw, bh) / 2 - 12
                    if take:
                        r, g, bb = px(x, y)
                        mine = (r - r0) ** 2 + (g - g0) ** 2 + (bb - b0) ** 2 < 70 ** 2
                        other = any((r - c[0]) ** 2 + (g - c[1]) ** 2 + (bb - c[2]) ** 2 < 70 ** 2
                                    for m, c in enumerate(cols, 1) if m != n)
                        if other and not mine:   # another number's badge over this one: erase, not in this layer
                            owner.setdefault((x, y), set())
                        else:
                            owner.setdefault((x, y), set()).add(n)
    # white rims of badges hidden under other badges: pure white blobs on the controller near a marker.
    # Erased, but in no number's layer (they belong to no visible badge).
    for n in range(1, 8):
        for b in info['spots'].get(str(n), []):
            x0, y0, x1, y1 = b
            for y in range(max(0, y0 - 16), min(h, y1 + 16)):
                for x in range(max(0, x0 - 16), min(panel, x1 + 16)):
                    if (x, y) in owner:
                        continue
                    r, g, bb = px(x, y)
                    if min(r, g, bb) > 235:
                        dark = sum(min(px(min(panel - 1, max(0, x + ox)), min(h - 1, max(0, y + oy)))) < 200
                                   for ox, oy in ((18, 0), (-18, 0), (0, 18), (0, -18), (13, 13), (-13, 13), (13, -13), (-13, -13)))
                        if dark >= 7:   # surrounded by the controller or other badges, not the page at its edge
                            owner[(x, y)] = set()
    # grow every marker by 2 px (anti-aliased edges) for the erase mask and the layers
    for _ in range(2):
        grown = dict(owner)
        for (x, y), ns in owner.items():
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                q = (x + dx, y + dy)
                if 0 <= q[0] < panel and 0 <= q[1] < h and q not in owner:   # into free pixels only, never onto another marker
                    grown.setdefault(q, set()).update(ns)
        owner = grown

    # layers: original pixels of each number's markers
    for n in range(1, 8):
        layer = bytearray(len(data))
        for (x, y), ns in owner.items():
            if n in ns:
                i = y * stride + 4 * x
                layer[i:i + 4] = data[i:i + 4]
        q = QImage(bytes(layer), w, h, stride, QImage.Format_ARGB32).copy()
        q.save(os.path.join(out_dir, f'{name[:-4]}-{n}.png'))

    # clean: erase every marker, filling from the outside in (each pass averages the known neighbours)
    clean = bytearray(data)
    unknown = set(owner)
    while unknown:
        done = {}
        for (x, y) in unknown:
            acc = [0, 0, 0]; k = 0
            for dx in (-1, 0, 1):
                for dy in (-1, 0, 1):
                    q = (x + dx, y + dy)
                    if (dx or dy) and q not in unknown and 0 <= q[0] < w and 0 <= q[1] < h:
                        i = q[1] * stride + 4 * q[0]
                        acc[0] += clean[i]; acc[1] += clean[i + 1]; acc[2] += clean[i + 2]; k += 1
            if k >= 3:
                done[(x, y)] = (acc[0] // k, acc[1] // k, acc[2] // k)
        if not done:
            break
        for (x, y), (b0, g0, r0) in done.items():
            i = y * stride + 4 * x
            clean[i], clean[i + 1], clean[i + 2], clean[i + 3] = b0, g0, r0, 255
        unknown -= set(done)
    QImage(bytes(clean), w, h, stride, QImage.Format_ARGB32).copy().save(os.path.join(out_dir, f'{name[:-4]}-clean.png'))
    info['clean'] = f'layers/{name[:-4]}-clean.png'
    info['layers'] = {str(n): f'layers/{name[:-4]}-{n}.png' for n in range(1, 8)}
    print(name, 'marker pixels', len(owner), flush=True)
json.dump(table, open(spots_path, 'w', encoding='utf-8'), indent=1)
