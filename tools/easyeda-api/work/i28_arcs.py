"""Gerber reader with true arc expansion (the chord shortcut in i28_cc could bridge a
pour's clearance rings and fake a short).

Gerber G02/G03 give a sweep direction in the *file's* coordinate frame.  Whether that
frame is Y-up (Gerber spec) or Y-down (how this file's numbers match the EDA document)
is not knowable from the header, and the two choices bulge in opposite ways, so both
modes are supported and the crop images decide.

Usage:  python i28_arcs.py both            -> write crops for mode A/B
        python i28_arcs.py stages <A|B>    -> run the S1..S5 stage analysis
"""
import json, math, os, re, sys, zipfile
from array import array
from PIL import Image, ImageDraw
import i28_cc as C

MM = C.MM2MIL
STEP = math.radians(4.0)


def read_layer(entry, mode="A"):
    txt = zipfile.ZipFile(C.ZIP).read(entry).decode("ascii", "replace")
    ap = {}
    for m in re.finditer(r"%ADD(\d+)(\w+),?([^*]*)\*%", txt):
        ap[m.group(1)] = (m.group(2), [float(v) * MM for v in re.findall(r"-?[\d.]+", m.group(3))])
    cur = ("C", [0.0])
    x = y = 0.0
    in_region = False
    poly = []
    regions, strokes, flashes = [], [], []
    for line in txt.splitlines():
        s = line.strip()
        if not s or s.startswith("G04") or s.startswith("%"):
            continue
        m = re.search(r"G54D(\d+)", s)
        if m:
            cur = ap.get(m.group(1), ("C", [0.0]))
            continue
        if s == "G36*":
            in_region, poly = True, []
            continue
        if s == "G37*":
            if len(poly) > 2:
                regions.append(list(poly))
            in_region, poly = False, []
            continue
        mm = re.search(r"X(-?\d+)Y(-?\d+)", s)
        if not mm:
            continue
        nx = int(mm.group(1)) / 1e6 * MM
        ny = int(mm.group(2)) / 1e6 * MM
        d = re.search(r"D0(\d)", s)
        kind = "line"
        if s.startswith("G02"):
            kind = "cw"
        elif s.startswith("G03"):
            kind = "ccw"
        ij = re.search(r"I(-?\d+)J(-?\d+)", s)
        pts = [(x, y), (nx, ny)]
        if kind in ("cw", "ccw") and ij and (x, y) != (nx, ny):
            cx = x + int(ij.group(1)) / 1e6 * MM
            cy = y + int(ij.group(2)) / 1e6 * MM
            r = math.hypot(nx - cx, ny - cy)
            ccw = (kind == "ccw")
            if mode == "B":
                ccw = not ccw
            a0 = math.atan2(y - cy, x - cx)
            a1 = math.atan2(ny - cy, nx - cx)
            da = a1 - a0
            if ccw:
                while da <= -1e-9:
                    da += 2 * math.pi
                if da < 1e-9:
                    da = 2 * math.pi
            else:
                while da >= 1e-9:
                    da -= 2 * math.pi
                if da > -1e-9:
                    da = -2 * math.pi
            n = max(2, int(abs(da) / STEP) + 1)
            pts = [(cx + r * math.cos(a0 + da * i / n), cy + r * math.sin(a0 + da * i / n))
                   for i in range(n + 1)]
        if d and d.group(1) == "1":
            if in_region:
                poly.extend(pts[1:])
            else:
                for i in range(len(pts) - 1):
                    strokes.append((cur, pts[i][0], pts[i][1], pts[i + 1][0], pts[i + 1][1]))
        elif d and d.group(1) == "2":
            if in_region:
                poly.append((nx, ny))
        elif d and d.group(1) == "3":
            flashes.append((cur, nx, ny))
        x, y = nx, ny
    return ap, regions, strokes, flashes


def wdt(a):
    return C.ap_params(a)[1]


def render(entry, mode, want):
    """want: subset predicate on stroke width -> ('track','relief','pour')"""
    ap, regions, strokes, flashes = read_layer(entry, mode)
    groups = {"track": [], "relief": [], "pour": []}
    for st in strokes:
        w = wdt(st[0])
        k = "track" if w > 11 else ("relief" if w > 9 else "pour")
        groups[k].append(st)
    img = Image.new("L", (C.W, C.H), 0)
    d = ImageDraw.Draw(img)
    for k in want:
        for a, x1, y1, x2, y2 in groups[k]:
            d.line([(x1, y1), (x2, y2)], fill=255, width=max(1, int(round(wdt(a)))))
    img.save(os.path.join(C.HERE, "_i28_arc_%s_%s.png" % (mode, "_".join(want))))
    return img, {k: len(v) for k, v in groups.items()}, len(regions), len(flashes)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "both"
    if cmd == "both":
        for mode in ("A", "B"):
            print(mode, render("Gerber_TopLayer.GTL", mode, ["pour"]))
        for mode in ("A", "B"):        # zoom crops around a bottom-left pad cluster
            im = Image.open(os.path.join(C.HERE, "_i28_arc_%s_pour.png" % mode))
            for box, name in (((280, 100, 420, 260), "j1"), ((700, 1600, 900, 1800), "mid")):
                im.crop(box).resize(((box[2] - box[0]) * 4, (box[3] - box[1]) * 4), Image.NEAREST) \
                  .save(os.path.join(C.HERE, "_i28_arccrop_%s_%s.png" % (mode, name)))
            print("crop written", mode)
    else:
        mode = sys.argv[2] if len(sys.argv) > 2 else "A"
        import i28_stage_arc as S
        S.run(mode)
