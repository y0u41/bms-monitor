"""Gerber connected-component short finder (rasterised, so it cannot be fooled by
polygon winding / pour bridges the way a point-in-polygon test was).

Pipeline, exactly like a CAM viewer:
  1. rasterise each copper layer at 1 px = 1 mil (union of flashes + strokes + regions)
  2. label 8-connected copper runs -> islands
  3. attribute an island to a net by the copper of the pad that owns it (search inside
     the pad's own footprint, because the pad centre may be a drill hole)
  4. stitch TOP/BOTTOM islands that share a drilled hole (component pins + vias)
  5. report any island carrying more than one net = a real short

Usage: python i28_cc.py
"""
import io, json, math, os, re, sys, zipfile, bisect
from array import array
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
ZIP = os.path.normpath(os.path.join(HERE, "..", "..", "..", "out", "pcb", "bms_a.gbr.zip"))
MM2MIL = 39.3700787401575
W, H = 2760, 1990


# ---------------------------------------------------------------- gerber reading
def read_layer(entry):
    txt = zipfile.ZipFile(ZIP).read(entry).decode("ascii", "replace")
    # the file declares %MOMM*%: coordinates AND aperture dimensions are millimetres,
    # so both must be scaled to mil. Forgetting the second half draws every pad 39x small.
    ap = {}
    for m in re.finditer(r"%ADD(\d+)(\w+),?([^*]*)\*%", txt):
        ap[m.group(1)] = (m.group(2), [float(v) * MM2MIL for v in re.findall(r"-?[\d.]+", m.group(3))])
    cur = ("C", [0.0])
    x = y = 0.0
    in_region = False
    poly = []
    regions, strokes, flashes = [], [], []
    for line in txt.splitlines():
        if line.startswith("G04") or line.startswith("%"):
            continue
        m = re.search(r"G54D(\d+)", line)
        if m:
            cur = ap.get(m.group(1), ("C", [0.0]))
        s = line.strip()
        px, py = x, y
        mm = re.search(r"X(-?\d+)Y(-?\d+)", line)
        if mm:
            x, y = int(mm.group(1)) / 1e6 * MM2MIL, int(mm.group(2)) / 1e6 * MM2MIL
        if s == "G36*":
            in_region, poly = True, []
            continue
        if s == "G37*":
            if len(poly) > 2:
                regions.append(list(poly))
            in_region, poly = False, []
            continue
        if s.endswith("D02*"):
            if in_region:
                poly.append((x, y))
            continue
        if s.endswith("D01*"):
            if in_region:
                poly.append((x, y))
            else:
                strokes.append((cur, px, py, x, y))
            continue
        if s.endswith("D03*"):
            flashes.append((cur, x, y))
    return ap, regions, strokes, flashes


def ap_params(ap):
    shape, n = ap
    if not n:
        return shape, 0.0, 0.0, 0.0
    if shape == "C":
        return shape, n[0], n[0], 0.0
    if shape == "O":
        return shape, n[0], (n[1] if len(n) > 1 else n[0]), (n[2] if len(n) > 2 else 0.0)
    if shape == "Rect" or shape == "R":
        return "R", n[0], (n[1] if len(n) > 1 else n[0]), (n[2] if len(n) > 2 else 0.0)
    if shape == "RoundRect":
        return "RR", abs(n[3] - n[1]), abs(n[4] - n[2]), (n[5] if len(n) > 5 else 0.0)
    return shape, n[0], (n[1] if len(n) > 1 else n[0]), 0.0


def draw_aperture(d, ap, cx, cy, col):
    shape, a, b, rot = ap_params(ap)
    if shape == "C":
        d.ellipse([cx - a / 2, cy - a / 2, cx + a / 2, cy + a / 2], fill=col)
        return
    if shape in ("O", "R", "RR"):
        pts = [(-a / 2, -b / 2), (a / 2, -b / 2), (a / 2, b / 2), (-a / 2, b / 2)]
        r = math.radians(rot)
        out = []
        for ux, uy in pts:
            out.append((cx + ux * math.cos(r) - uy * math.sin(r),
                        cy + ux * math.sin(r) + uy * math.cos(r)))
        d.polygon(out, fill=col)
        return
    d.ellipse([cx - a / 2, cy - a / 2, cx + a / 2, cy + a / 2], fill=col)


def raster(entry, tag):
    ap, regions, strokes, flashes = read_layer(entry)
    img = Image.new("L", (W, H), 0)
    d = ImageDraw.Draw(img)
    for a, x1, y1, x2, y2 in strokes:
        w = max(1.0, ap_params(a)[1])
        d.line([(x1, y1), (x2, y2)], fill=255, width=int(round(w)))
    for r in regions:
        d.polygon(r, fill=255)
    for a, x, y in flashes:
        draw_aperture(d, a, x, y, 255)
    img.save(os.path.join(HERE, "_i28_%s.png" % tag))
    print("%s: regions=%d strokes=%d flashes=%d" % (entry, len(regions), len(strokes), len(flashes)))
    return img


# ---------------------------------------------------------------- labelling
def label(img):
    """8-connected run labelling. Returns (runs_per_row, parent) where a pixel's island
    is find(parent[label])."""
    raw = img.tobytes()          # values are already 0/255 -> regex straight on bytes
    rows = []
    parent = []
    for y in range(H):
        row = raw[y * W:(y + 1) * W]
        runs = []
        for m in re.finditer(b"\xff+", row):
            s, e = m.start(), m.end() - 1
            lab = len(parent)
            parent.append(lab)
            runs.append([s, e, lab])
        # union with previous row (8-connectivity: spans touching diagonally count)
        if rows:
            prev = rows[-1]
            i = 0
            for s, e, lab in runs:
                while i < len(prev) and prev[i][1] < s - 1:
                    i += 1
                j = i
                while j < len(prev) and prev[j][0] <= e + 1:
                    union(parent, lab, prev[j][2])
                    j += 1
        rows.append(runs)
    starts = [[r[0] for r in runs] for runs in rows]
    return rows, parent, starts


def union(parent, a, b):
    ra, rb = find(parent, a), find(parent, b)
    if ra != rb:
        parent[max(ra, rb)] = min(ra, rb)


def find(parent, a):
    while parent[a] != a:
        parent[a] = parent[parent[a]]
        a = parent[a]
    return a


def label_at(rows, parent, x, y, starts=None):
    """Island root at a pixel, or None when background."""
    y = int(round(y))
    x = int(round(x))
    if not (0 <= y < H and 0 <= x < W):
        return None
    if starts is not None:
        i = bisect.bisect_right(starts[y], x) - 1
        if i < 0:
            return None
        s, e, lab = rows[y][i]
        return find(parent, lab) if x <= e else None
    for s, e, lab in rows[y]:
        if s <= x <= e:
            return find(parent, lab)
    return None


# ---------------------------------------------------------------- pad footprints
def pad_rects(entry_pads):
    """ComponentPad records from the live dump, deduplicated."""
    out = {}
    for p in entry_pads:
        k = (p["des"], p.get("pn"), round(p["x"], 1), round(p["y"], 1), p["ptype"])
        out[k] = p
    return list(out.values())


def main():
    pads = json.load(io.open(os.path.join(HERE, "_pads2.json"), encoding="utf-8"))
    layers = {"GTL": raster("Gerber_TopLayer.GTL", "gtl"),
              "GBL": raster("Gerber_BottomLayer.GBL", "gbl")}
    lab = {}
    for k, img in layers.items():
        lab[k] = label(img)
        print("%s islands=%d" % (k, len(set(find(lab[k][1], i) for i in range(len(lab[k][1]))))))

    # net attribution: for each pad, sample copper pixels inside its own footprint
    attrib = {}     # (layer, island) -> set(nets)
    unmatched = []
    for p in pads:
        lyr = "GTL" if p["layer"] in (1, 12) else "GBL"
        rows, parent, starts = lab[lyr]
        shp = p.get("shape") or []
        w = (shp[1] if len(shp) > 1 else 10)
        h = (shp[2] if len(shp) > 2 else 10)
        if p.get("rot") in (90, 270):
            w, h = h, w
        # sample the inner 60% of the pad: guaranteed to be the pad's own copper,
        # never the neighbouring pour (which can only be outside the pad outline)
        w, h = w * 0.6 / 2.0, h * 0.6 / 2.0
        found = set()
        for yy in range(int(p["y"] - h), int(p["y"] + h) + 1):
            for xx in range(int(p["x"] - w), int(p["x"] + w) + 1):
                r = label_at(rows, parent, xx, yy, starts)
                if r is not None:
                    found.add(r)
        if not found:
            unmatched.append(p)
            continue
        for r in found:
            attrib.setdefault((lyr, r), set()).add(p["net"] or "<none>")
    print("pads without any copper under them: %d" % len(unmatched))
    for p in unmatched[:10]:
        print("   ", p["des"], p["net"], p["x"], p["y"], p["ptype"])

    # cross-layer stitching through drilled holes (proper union-find, transitive)
    z = zipfile.ZipFile(ZIP)
    holes = []
    for e in ("Drill_PTH_Through.DRL", "Drill_PTH_Through_Via.DRL"):
        t = z.read(e).decode("ascii", "replace")
        x = y = 0.0
        for line in t.splitlines():
            # drill files are METRIC,LZ,00.000000 -> coordinates carry a decimal point
            m = re.search(r"X(-?[\d.]+)Y(-?[\d.]+)", line)
            if m:
                x, y = float(m.group(1)) * MM2MIL, float(m.group(2)) * MM2MIL
                holes.append((x, y))
    gp = {}

    def gf(k):
        gp.setdefault(k, k)
        while gp[k] != k:
            gp[k] = gp[gp[k]]
            k = gp[k]
        return k

    def gu(a, b):
        ra, rb = gf(a), gf(b)
        if ra != rb:
            gp[rb] = ra

    stitched = 0
    for x, y in holes:
        a = label_at(lab["GTL"][0], lab["GTL"][1], x, y, lab["GTL"][2])
        b = label_at(lab["GBL"][0], lab["GBL"][1], x, y, lab["GBL"][2])
        if a is not None and b is not None:
            gu(("GTL", a), ("GBL", b))
            stitched += 1
    print("drilled holes: %d, cross-layer links: %d" % (len(holes), stitched))

    merged = {}
    for k, v in attrib.items():
        merged.setdefault(gf(k), set()).update(v)
    attrib = merged
    multi = {k: v for k, v in attrib.items() if len(v) > 1}
    print("\nislands carrying a net: %d | MULTI-NET (short): %d" % (len(attrib), len(multi)))
    for key, nets in sorted(multi.items(), key=lambda r: -len(r[1])):
        print("   !! %s: %s" % (key, ", ".join(sorted(nets))))
    json.dump({"%s/%d" % k: sorted(v) for k, v in attrib.items()},
              io.open(os.path.join(HERE, "_i28_islands.json"), "w", encoding="utf-8"),
              ensure_ascii=False, indent=1)


if __name__ == "__main__":
    main()
