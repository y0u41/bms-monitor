"""Global worst-case cross-net copper gap, exact-enough Euclidean, no dilation games.

Scan every copper run against runs in the next L rows (L shrinks as the best gap
shrinks), and report the closest pairs whose islands belong to DIFFERENT nets.
Run on the full CAM raster (B = pads+tracks+relief+fill+pour border) and on the
border-free raster (F) so the pour outline's bite is measured, not assumed.

Usage: python i30_gapglobal.py
"""
import json, math, os, re, sys
from PIL import Image, ImageDraw
import i28_cc as C
import i28_arcs as A
import i28_stage_arc as S

L0 = 14


def raster(CL, pour, k):
    rg, tr, rl, po, fl = CL[k]
    img = Image.new("L", (C.W, C.H), 0)
    d = ImageDraw.Draw(img)
    for g in (tr, rl, po if pour else []):
        for a, x1, y1, x2, y2 in g:
            d.line([(x1, y1), (x2, y2)], fill=255, width=max(1, int(round(C.ap_params(a)[1]))))
    for r in rg:
        d.polygon(r, fill=255)
    for a, x, y in fl:
        C.draw_aperture(d, a, x, y, 255)
    return img


def scan(lab, attrib, k):
    """lab = (rows, parent, starts) for that layer; attrib keyed (layer, island) -> nets."""
    rows, parent, _ = lab
    Wl, Hl = C.W, C.H
    net = {}
    for y in range(Hl):
        for (s, e, l) in rows[y]:
            r = C.find(parent, l)
            v = attrib.get((k, r))
            if v and len(v) == 1:
                net[(y, s)] = (next(iter(v)), r)
    best = []
    L = L0
    for y in range(Hl):
        cur = [(s, e, C.find(parent, l)) for (s, e, l) in rows[y]]
        for dy in range(0, L + 1):
            yy = y + dy
            if yy >= Hl:
                break
            for (s, e, r1) in cur:
                n1 = net.get((y, s))
                if not n1:
                    continue
                for (s2, e2, r2) in rows[yy]:
                    if r2 == r1:
                        continue
                    n2 = net.get((yy, s2))
                    if not n2 or n2[0] == n1[0]:
                        continue
                    dx = 0 if max(s, s2) <= min(e, e2) else min(abs(s - e2), abs(e - s2))
                    d = math.hypot(dx, dy)
                    if d < L:
                        L = max(2, int(d) - 1)
                    if d <= 10.0:
                        best.append((round(d, 2), n1[0], n2[0], (s + e) // 2, y))
    best.sort()
    seen = set()
    uniq = []
    for d, a, b, x, y in best:
        key = (min(a, b), max(a, b), x // 40, y // 40)
        if key in seen:
            continue
        seen.add(key)
        uniq.append((d, a, b, x, y))
        if len(uniq) >= 14:
            break
    return uniq


def main():
    only = sys.argv[1] if len(sys.argv) > 1 else "BF"
    CL = {k: S.classify(k, "A") for k in S.ENTRY}
    for tag, pour in (("B full CAM", True), ("F no border", False)):
        if tag[0] not in only:
            continue
        if tag[0] not in only:
            continue
        print("\n===== %s =====" % tag)
        for k in S.ENTRY:
            if len(sys.argv) > 2 and sys.argv[2] not in k:
                continue
            img = raster(CL, pour, k)
            lab = C.label(img)
            rows, parent, starts = lab
            attrib = {}
            for p in S.pads:
                if (("GTL" if p["layer"] in (1, 12) else "GBL") != k):
                    continue
                shp = p.get("shape") or []
                w = shp[1] if len(shp) > 1 else 10
                h = shp[2] if len(shp) > 2 else 10
                if p.get("rot") in (90, 270):
                    w, h = h, w
                w, h = w * 0.3, h * 0.3
                for yy in range(int(p["y"] - h), int(p["y"] + h) + 1):
                    for xx in range(int(p["x"] - w), int(p["x"] + w) + 1):
                        r = C.label_at(rows, parent, xx, yy, starts)
                        if r is not None:
                            attrib.setdefault((k, r), set()).add(p["net"] or "<none>")
            uniq = scan(lab, attrib, k)
            print("-- %s: closest cross-net pairs (mil / mm)" % k)
            for d, a, b, x, y in uniq:
                print("     %6.2f mil  %.3f mm   %-9s <-> %-9s  at (%d,%d)" %
                      (d, d / C.MM2MIL, a, b, x, y))
            if uniq:
                print("   WORST = %.2f mil = %.3f mm" % (uniq[0][0], uniq[0][0] / C.MM2MIL))


if __name__ == "__main__":
    main()
