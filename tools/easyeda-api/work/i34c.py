"""Drill <-> pad reconciliation + mounting-hole copper exposure, all off the ORDER files.

Uses FlyingProbeTesting.json (297 pins with NET_NAME / HOLE_SIZE / coords in mil)
as the pad list, and the three Excellon files as the drill list.
"""
import json
import math
import os
import re
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import i28_cc as C
import i28_arcs as A

ZIPFILE = os.path.normpath(os.path.join(HERE, "..", "..", "..", "out", "pcb", "bms_A_order_v1.zip"))
C.ZIP = ZIPFILE
Z = zipfile.ZipFile(ZIPFILE)
MM = C.MM2MIL
d = json.loads(Z.read("FlyingProbeTesting.json").decode("utf-8"))
F = d["pins"]["fields"]
pins = [dict(zip(F, r)) for r in d["pins"]["rows"]]
comps = [dict(zip(d["components"]["fields"], r)) for r in d["components"]["rows"]]

out = []
out.append("pins=%d  components=%d" % (len(pins), len(comps)))
out.append("NOTE FlyingProbe HOLE_SIZE is in MIL (1.1mm->43.31), pad sizes in MIL too.")


def parse_drill(name):
    txt = Z.read(name).decode("ascii", "replace")
    sizes, cur, holes = {}, None, []
    for line in txt.splitlines():
        s = line.strip()
        m = re.match(r"T(\d+)C([\d.]+)", s)
        if m:
            sizes[m.group(1)] = float(m.group(2)) * MM
            continue
        m = re.match(r"T(\d+)$", s)
        if m:
            cur = m.group(1)
            continue
        if s.startswith("M"):
            continue
        m = re.match(r"X(-?[\d.]+)Y(-?[\d.]+)", s)
        if m and cur:
            holes.append((cur, sizes.get(cur, 0), float(m.group(1)) * MM, float(m.group(2)) * MM))
    return holes


pth = parse_drill("Drill_PTH_Through.DRL")
npth = parse_drill("Drill_NPTH_Through.DRL")
via = parse_drill("Drill_PTH_Through_Via.DRL")
out.append("drills: PTH=%d  NPTH=%d  VIA-file=%d  (via subset of PTH: %s)"
           % (len(pth), len(npth), len(via),
              all(any(abs(v[2] - p[2]) < .05 and abs(v[3] - p[3]) < .05 for p in pth) for v in via)))

# ---- pins that carry a hole ------------------------------------------------
th = [p for p in pins if p["HOLE_SIZE"] > 0 or p["HOLE_LEN"] > 0]
out.append("pins with a hole in the probe file: %d   (SMD=%d / TH=%d)"
           % (len(th), sum(1 for p in pins if p["PIN_TYPE"] == "SMD"),
              sum(1 for p in pins if p["PIN_TYPE"] != "SMD")))
hs = {}
for p in th:
    hs[(round(p["HOLE_SIZE"], 3), round(p["HOLE_LEN"], 3))] = hs.get((round(p["HOLE_SIZE"], 3), round(p["HOLE_LEN"], 3)), 0) + 1
out.append("  hole-size histogram (mm x mm):count -> " + ", ".join("%s:%d" % (k, v) for k, v in sorted(hs.items())))

# ---- bidirectional reconcile: every drilled hole must sit on a pad-pin -----
padpts = [(p, math.hypot(max(0, p["PAD_SIZEX"] - p["HOLE_SIZE"]), 0) ) for p in pins]


def match(hx, hy, tol=1.0):
    best = None
    for p in pins:
        dd = math.hypot(p["PIN_X"] - hx, p["PIN_Y"] - hy)
        if dd < tol and (best is None or dd < best[0]):
            best = (dd, p)
    return best


out.append("")
out.append("== A. every PTH drill must land on a pad (or a via) ==")
named = [h for h in pth if not any(abs(h[2] - v[2]) < .05 and abs(h[3] - v[3]) < .05 for v in via)]
out.append("  non-via PTH drills = %d" % len(named))
bad = []
for t, dia, x, y in named:
    m = match(x, y)
    if m is None:
        bad.append((dia, x, y))
    elif abs(m[1]["HOLE_SIZE"] - dia) > 2 and abs(m[1]["HOLE_LEN"] - dia) > 2:
        bad.append((dia, x, y, "size-mismatch vs %s" % m[1]["PIN_NAME"]))
out.append("  drills with no pad / wrong size: %d" % len(bad))
for b in bad:
    m = match(b[1], b[2], 40)
    out.append("    dia %.3f mm @ (%.1f, %.1f) mil  nearest pad within 40mil: %s" %
               (b[0], b[1], b[2], ("%s net=%s d=%.1f" % (m[1]["PIN_NAME"], m[1]["NET_NAME"], m[0])) if m else "none"))

out.append("")
out.append("== B. every pad that needs a hole must be drilled ==")
miss = []
for p in pins:
    if p["PIN_TYPE"] == "SMD" or (p["HOLE_SIZE"] <= 0 and p["HOLE_LEN"] <= 0):
        continue
    if not any(abs(h[2] - p["PIN_X"]) < 1 and abs(h[3] - p["PIN_Y"]) < 1 for h in pth):
        miss.append(p)
out.append("  TH pads with no drill within 1 mil: %d" % len(miss))
for p in miss[:20]:
    out.append("    %s net=%s @ (%.1f,%.1f) hole %.2f mil" % (p["PIN_NAME"], p["NET_NAME"], p["PIN_X"], p["PIN_Y"], p["HOLE_SIZE"]))

# ---- outline ---------------------------------------------------------------
ap, regions, strokes, fla = A.read_layer("Gerber_BoardOutlineLayer.GKO", "A")
pts = []
for st in strokes:
    pts += [(st[1], st[2]), (st[3], st[4])]
xs = [q[0] for q in pts]
ys = [q[1] for q in pts]
out.append("")
out.append("== C. board outline (GKO) ==")
out.append("  primitives: strokes=%d regions=%d flashes=%d" % (len(strokes), len(regions), len(fla)))
out.append("  bbox = %.1f x %.1f mil = %.4f x %.4f mm" %
           (max(xs) - min(xs), max(ys) - min(ys), (max(xs) - min(xs)) / MM, (max(ys) - min(ys)) / MM))
out.append("  origin = (%.1f, %.1f) mil" % (min(xs), min(ys)))

# ---- mounting holes --------------------------------------------------------
out.append("")
out.append("== D. NPTH / mounting holes ==")
for t, dia, x, y in npth:
    dx = min(abs(x - min(xs)), abs(max(xs) - x))
    dy = min(abs(y - min(ys)), abs(max(ys) - y))
    edge = min(dx, dy)
    out.append("  hole dia %.3f mm @ (%.1f, %.1f) mil ; distance to outline = %.1f mil = %.3f mm"
               % (dia / MM, x, y, edge, edge / MM))
    near = sorted(((math.hypot(p["PIN_X"] - x, p["PIN_Y"] - y), p) for p in pins), key=lambda q: q[0])[:4]
    for dd, p in near:
        r_in = (dia + p["HOLE_SIZE"] * 0) / 2.0
        out.append("     nearest pad %+.1f mil: %s net=%s %s %.1fx%.1f mil @ (%.1f,%.1f)"
                   % (dd, p["PIN_NAME"], p["NET_NAME"], p["PAD_SHAPE"], p["PAD_SIZEX"], p["PAD_SIZEY"], p["PIN_X"], p["PIN_Y"]))
    # does the 3.2mm bore swallow any copper?
    for dd, p in near:
        half_reach = max(p["PAD_SIZEX"], p["PAD_SIZEY"]) / 2.0
        if dd - half_reach < dia / 2.0:
            out.append("     !! copper of %s (%s) intrudes into the bore by %.1f mil = %.3f mm"
                       % (p["PIN_NAME"], p["NET_NAME"], (dd - half_reach) - dia / 2.0, ((dd - half_reach) - dia / 2.0) / MM * -1))

print("\n".join(out))
open(os.path.join(HERE, "_i34c.txt"), "w", encoding="utf-8").write("\n".join(out) + "\n")
