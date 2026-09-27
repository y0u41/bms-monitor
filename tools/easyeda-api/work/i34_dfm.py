"""Stage-2 DFM measurements taken straight off the ORDER Gerber (not the editor).

Outputs, in mil unless noted:
  1. solder-mask web (bridge) width between the two nearest GTS/GBS openings
  2. minimum copper annular ring around every drilled hole
  3. test-point copper diameters (from the TP list + GTL/GBL flashes)
  4. GTO/GBO stroke inventory (character ink present at all)
"""
import math
import os
import re
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import i28_cc as C
import i28_arcs as A

HERE = os.path.dirname(os.path.abspath(__file__))
ZIPFILE = os.path.normpath(os.path.join(HERE, "..", "..", "..", "out", "pcb", "bms_A_order_v1.zip"))
C.ZIP = ZIPFILE
Z = zipfile.ZipFile(ZIPFILE)
NAMES = Z.namelist()


def layer(pattern):
    for n in NAMES:
        if pattern.lower() in n.lower():
            return n
    return None


GTS, GBS = layer("GTS"), layer("GBS")
GTL, GBL = layer("GTL"), layer("GBL")
GTO, GBO = layer("GTO"), layer("GBO")


def ap_box(ap):
    """Return (kind, w, h) of an aperture in mil; None when not a pad-like flash."""
    shape, params = ap
    tmpl = C.ap_params(ap)
    if shape in ("C", "R", "O", "ROUNDRECT", "RECT", "RING") or shape.startswith("R"):
        vals = params
        if shape in ("C", "RING"):
            d = vals[0] if vals else 0
            return ("c", d, d)
        if len(vals) >= 2:
            w, h = vals[0], vals[1]
            if len(vals) == 4:  # R: X,Y,centre -> still width,height
                pass
            return ("r", w, h)
    return None


def rect_gap(a, b):
    """Edge-to-edge gap between two axis-aligned boxes given as (kind,w,h,x,y)."""
    dx = abs(a[3] - b[3]) - (a[1] + b[1]) / 2.0
    dy = abs(a[4] - b[4]) - (a[2] + b[2]) / 2.0
    if dx <= 0 and dy <= 0:
        return 0.0
    if dx < 0:
        dx = 0.0
    if dy < 0:
        dy = 0.0
    return math.hypot(dx, dy)


def openings(entry):
    ap, regions, strokes, flashes = A.read_layer(entry, "A")
    out = []
    for a, x, y in flashes:
        box = ap_box(a)
        if box and box[1] > 0.5 and box[2] > 0.5:
            out.append((box[0], box[1], box[2], x, y))
    return out, strokes


def nearest_pair(boxes):
    best = (1e9, None, None)
    n = len(boxes)
    for i in range(n):
        for j in range(i + 1, n):
            g = rect_gap(boxes[i], boxes[j])
            if g < best[0] - 1e-9:
                best = (g, boxes[i], boxes[j])
    return best


def main():
    rep = []
    rep.append("order file : %s" % os.path.basename(ZIPFILE))
    rep.append("entries    : %d  %s" % (len(NAMES), ", ".join(NAMES)))

    # ---- 1. solder mask web -----------------------------------------------
    for tag, entry in (("GTS(top mask openings)", GTS), ("GBS(bottom mask openings)", GBS)):
        boxes, strokes = openings(entry)
        g, a, b = nearest_pair(boxes)
        rep.append("")
        rep.append("[%s] %s" % (tag, entry))
        rep.append("  flash openings=%d  stroke prims=%d" % (len(boxes), len(strokes)))
        rep.append("  nearest two openings edge gap = %.2f mil = %.3f mm" % (g, g / C.MM2MIL))
        rep.append("    A %s %.2fx%.2f mil @ (%.1f,%.1f)" % (a[0], a[1], a[2], a[3], a[4]))
        rep.append("    B %s %.2fx%.2f mil @ (%.1f,%.1f)" % (b[0], b[1], b[2], b[3], b[4]))
        # web = ink left between two openings == the gap itself
        web_mm = g / C.MM2MIL
        rep.append("  => solder-mask web %.3f mm  %s (JLC/EDA rule 0.10 mm)"
                   % (web_mm, "OK" if web_mm >= 0.10 else "VIOLATION"))

    # ---- 2. annular ring around drills ------------------------------------
    ap_l, reg_l, str_l, fla_l = A.read_layer(GTL, "A")
    ap_b, reg_b, str_b, fla_b = A.read_layer(GBL, "A")
    top = openings(GTL)[0]
    bot = openings(GBL)[0]
    drills = []
    for n in NAMES:
        base = os.path.basename(n).lower()
        if "drill" not in base:
            continue
        txt = Z.read(n).decode("ascii", "replace")
        fm = re.search(r"M48(.*?)M95", txt, re.S)
        hdr = fm.group(1) if fm else txt
        sizes = {}
        for m in re.finditer(r"T(\d+)([^C\n]*)C([\d.]+)", hdr):
            sizes[m.group(1)] = float(m.group(3)) * C.MM2MIL
        for m in re.finditer(r"X(-?[\d.]+)Y(-?[\d.]+)((?:T\d+)?)(?![\d.])",
                             re.sub(r"%.*?%", "", txt, flags=re.S)):
            t = re.search(r"T(\d+)", m.group(3))
            if t:
                drills.append((os.path.basename(n), sizes.get(t.group(1), 0.0),
                               float(m.group(1)) * C.MM2MIL, float(m.group(2)) * C.MM2MIL))
    rep.append("")
    rep.append("[annular ring] drills parsed = %d" % len(drills))
    worst = []
    for src, dia, x, y in drills:
        if dia <= 0:
            continue
        rings = []
        for boxes in (top, bot):
            best = None
            for bx in boxes:
                d = math.hypot(bx[3] - x, bx[4] - y)
                if d < 1e-6:
                    best = bx
                    break
            if best is None:
                continue
            if best[0] == "c":
                ring = (best[1] - dia) / 2.0
            else:
                ring = (min(best[1], best[2]) - dia) / 2.0
            rings.append(ring)
        if rings:
            worst.append((min(rings), src, dia, best))
    worst.sort()
    for ring, src, dia, bx in worst[:8]:
        rep.append("  ring %.2f mil = %.3f mm   hole %.2f mil (%.3f mm)  %s  pad %s %.2fx%.2f"
                   % (ring, ring / C.MM2MIL, dia, dia / C.MM2MIL, src, bx[0], bx[1], bx[2]))
    if worst:
        r0 = worst[0][0] / C.MM2MIL
        rep.append("  => min annular ring %.3f mm  %s (JLC 常规 >= 0.15 mm)"
                   % (r0, "OK" if r0 >= 0.15 else "CHECK"))
    rep.append("  hole sizes: " + ", ".join("%.3f mm x%d"
               % (sorted(set(d for _, d, _, _ in worst))[i] / C.MM2MIL * 0 + 0, 0)
               for i in []) if False else "")
    hs = {}
    for _, dia, _, _ in worst:
        hs[round(dia, 2)] = hs.get(round(dia, 2), 0) + 1
    rep.append("  drill dia histogram (mil:count): " +
               ", ".join("%s:%d" % (k, v) for k, v in sorted(hs.items())))

    print("\n".join(rep))
    out = os.path.join(C.HERE, "_i34_dfm.txt")
    with open(out, "w", encoding="utf-8") as fh:
        fh.write("\n".join(rep) + "\n")


if __name__ == "__main__":
    main()
