"""At the tightest cross-net spots: how much of the gap is the pour BORDER stroke?

GTL rasters differing only by the 8-mil pour outline strokes:
  F = fill regions + real tracks + relief spokes + pad flashes
  B = F + pour outline strokes          (full CAM picture)

In a window around each hot spot, exact pixel distance from the foreign pad island
to the VSS plane island, in both rasters.

  gapF ~= gapB -> border drawn inside the fill (innocent); the rule itself is tight
  gapF  >  gapB -> the border sticks out and eats the plane clearance

Usage: python i30_gap_at.py
"""
import math, os, re
from PIL import Image, ImageDraw
import i28_cc as C
import i28_arcs as A
import i28_stage_arc as S

SPOTS = [(1122, 792, "<none>"), (1122, 1018, "AFE_VBAT"), (1372, 1097, "DSG"),
         (1148, 872, "VC5"), (1300, 1058, "AFE_SDA"), (1247, 1058, "AFE_SCL")]
R = 30


def raster(CL, pour):
    imgs = {}
    for k in S.ENTRY:
        rg, tr, rl, po, fl = CL[k]
        img = Image.new("L", (C.W, C.H), 0)
        d = ImageDraw.Draw(img)
        for g in (tr, rl, po if pour else []):
            for a, x1, y1, x2, y2 in g:
                d.line([(x1, y1), (x2, y2)], fill=255,
                       width=max(1, int(round(C.ap_params(a)[1]))))
        for r in rg:
            d.polygon(r, fill=255)
        for a, x, y in fl:
            C.draw_aperture(d, a, x, y, 255)
        imgs[k] = img
    return imgs


def window_runs(img, lab, x, y, islands):
    """Pixel runs inside the window whose island is in `islands`."""
    rows, parent, starts = lab
    raw = img.tobytes()
    out = []
    for yy in range(y - R, y + R):
        seg = raw[yy * C.W + x - R: yy * C.W + x + R]
        for m in re.finditer(b"\xff+", seg):
            s0, e0 = x - R + m.start(), x - R + m.end() - 1
            root = C.label_at(rows, parent, (s0 + e0) // 2, yy, starts)
            if root in islands:
                out.append((yy, s0, e0))
    return out


def dist(ra, rb):
    best = float(R) * 2
    for (ya, sa, ea) in ra:
        for (yb, sb, eb) in rb:
            dy = abs(ya - yb)
            if dy >= best:
                continue
            dx = 0 if max(sa, sb) <= min(ea, eb) else min(abs(sa - eb), abs(ea - sb))
            d = math.hypot(dx, dy)
            if d < best:
                best = d
    return best


def main():
    CL = {k: S.classify(k, "A") for k in S.ENTRY}
    IF, IB = raster(CL, False), raster(CL, True)
    res = {}
    for tag, imgs in (("F", IF), ("B", IB)):
        lab = {k: C.label(imgs[k]) for k in imgs}
        attrib, orphan = S.net_attribution(lab)
        vss = set(r for (ly, r), v in attrib.items() if v == {"VSS"} and ly == "GTL")
        npl = {}
        for (ly, r), v in attrib.items():
            if ly == "GTL":
                npl.setdefault(frozenset(v), set()).add(r)
        res[tag] = (lab, imgs["GTL"], vss, npl)
        print("%s: VSS islands=%d  largestVssRuns=%s  nets here=%s" %
              (tag, len(vss), "", sorted(n for n in npl if len(n) == 1)[:0]))

    print("\n%-10s %-11s %7s %7s %8s %8s" % ("net", "spot", "gapF", "gapB", "F_mm", "B_mm"))
    for x, y, net in SPOTS:
        row = []
        for tag in ("F", "B"):
            lab, img, vss, npl = res[tag]
            pads_islands = set()
            for p in S.pads:
                if (p["net"] or "<none>") != net or p["layer"] not in (1, 12):
                    continue
                r = C.label_at(lab["GTL"][0], lab["GTL"][1], p["x"], p["y"], lab["GTL"][2])
                if r is not None and r not in vss:
                    pads_islands.add(r)
            a = window_runs(img, lab["GTL"], x, y, pads_islands)
            b = window_runs(img, lab["GTL"], x, y, vss)
            row.append(dist(a, b) if a and b else float("nan"))
        print("%-10s %-11s %7.2f %7.2f %8.3f %8.3f" %
              (net, "%d,%d" % (x, y), row[0], row[1], row[0] / C.MM2MIL, row[1] / C.MM2MIL))


if __name__ == "__main__":
    main()
