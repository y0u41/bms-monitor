"""Stage analysis with TRUE arc rendering (i28_stage used chords, which cut 4..75 mil
into every clearance ring and could fake a short).

Usage: python i28_stage_arc.py A
"""
import json, os, re, sys, zipfile
from PIL import Image, ImageDraw
import i28_cc as C
import i28_arcs as A

HERE = C.HERE
pads = json.load(open(os.path.join(HERE, "_pads2.json"), encoding="utf-8"))
doc = json.load(open(os.path.join(HERE, "_copper.json"), encoding="utf-8"))
ENTRY = {"GTL": ("Gerber_TopLayer.GTL", 1), "GBL": ("Gerber_BottomLayer.GBL", 2)}


def trackkeys(layer):
    s = set()
    for t in doc:
        if t["layer"] != layer:
            continue
        a = (round(t["x1"], 1), round(t["y1"], 1))
        b = (round(t["x2"], 1), round(t["y2"], 1))
        s.add((a, b)); s.add((b, a))
    return s


TK = {1: trackkeys(1), 2: trackkeys(2)}
HOLES = []
_z = zipfile.ZipFile(C.ZIP)
for e in ("Drill_PTH_Through.DRL", "Drill_PTH_Through_Via.DRL"):
    for line in _z.read(e).decode("ascii", "replace").splitlines():
        m = re.search(r"X(-?[\d.]+)Y(-?[\d.]+)", line)
        if m:
            HOLES.append((float(m.group(1)) * C.MM2MIL, float(m.group(2)) * C.MM2MIL))


def classify(lyr, mode):
    ap, regions, strokes, flashes = A.read_layer(ENTRY[lyr][0], mode)
    real, relief, pour = [], [], []
    for a, x1, y1, x2, y2 in strokes:
        w = C.ap_params(a)[1]
        p1 = (round(x1, 1), round(y1, 1)); p2 = (round(x2, 1), round(y2, 1))
        if (p1, p2) in TK[ENTRY[lyr][1]] or w > 11.0:
            real.append((a, x1, y1, x2, y2))
        elif w > 9.0:
            relief.append((a, x1, y1, x2, y2))
        else:
            pour.append((a, x1, y1, x2, y2))
    return regions, real, relief, pour, flashes


def build(mode="A"):
    return {k: classify(k, mode) for k in ENTRY}


def make_images(CL, relief=1, pour=1, regions=1):
    imgs = {}
    for k in ENTRY:
        rg, tr, rl, po, fl = CL[k]
        img = Image.new("L", (C.W, C.H), 0)
        d = ImageDraw.Draw(img)
        for g in [tr] + ([rl] if relief else []) + ([po] if pour else []):
            for a, x1, y1, x2, y2 in g:
                d.line([(x1, y1), (x2, y2)], fill=255,
                       width=max(1, int(round(C.ap_params(a)[1]))))
        if regions:
            for r in rg:
                d.polygon(r, fill=255)
        for a, x, y in fl:
            C.draw_aperture(d, a, x, y, 255)
        imgs[k] = img
    return imgs


def net_attribution(lab):
    """(layer, island) -> set(nets), plus the pads that found no copper."""
    attrib = {}
    orphan = 0
    for p in pads:
        lyr = "GTL" if p["layer"] in (1, 12) else "GBL"
        rows, parent, starts = lab[lyr]
        shp = p.get("shape") or []
        w = shp[1] if len(shp) > 1 else 10
        h = shp[2] if len(shp) > 2 else 10
        if p.get("rot") in (90, 270):
            w, h = h, w
        w, h = w * 0.6 / 2, h * 0.6 / 2
        found = set()
        for yy in range(int(p["y"] - h), int(p["y"] + h) + 1):
            for xx in range(int(p["x"] - w), int(p["x"] + w) + 1):
                r = C.label_at(rows, parent, xx, yy, starts)
                if r is not None:
                    found.add(r)
        if not found:
            orphan += 1
        for r in found:
            attrib.setdefault((lyr, r), set()).add(p["net"] or "<none>")
    return attrib, orphan


def stitch(lab, attrib):
    gp = {}
    def gf(k):
        gp.setdefault(k, k)
        while gp[k] != k:
            gp[k] = gp[gp[k]]; k = gp[k]
        return k
    for x, y in HOLES:
        a = C.label_at(lab["GTL"][0], lab["GTL"][1], x, y, lab["GTL"][2])
        b = C.label_at(lab["GBL"][0], lab["GBL"][1], x, y, lab["GBL"][2])
        if a is not None and b is not None:
            ra, rb = gf(("GTL", a)), gf(("GBL", b))
            if ra != rb:
                gp[rb] = ra
    merged = {}
    for k, v in attrib.items():
        merged.setdefault(gf(k), set()).update(v)
    return merged


def report(attrib, orphan, tag):
    multi = {k: v for k, v in attrib.items() if len(v) > 1}
    net_islands = {}
    for k, v in attrib.items():
        for n in v:
            net_islands.setdefault(n, set()).add(k)
    unrouted = {n: ks for n, ks in net_islands.items() if len(ks) > 1 and not (ks & set(multi))}
    print("\n--- %s ---" % tag)
    print("  islands with a net=%d  pads with no copper=%d" % (len(attrib), orphan))
    print("  SHORT islands=%d" % len(multi))
    for k, v in sorted(multi.items(), key=lambda r: -len(r[1]))[:4]:
        print("     %s: %d nets -> %s" % (k, len(v), ", ".join(sorted(v))))
    print("  UNROUTED nets=%d" % len(unrouted))
    for n, ks in sorted(unrouted.items())[:14]:
        print("     %s on %d islands" % (n, len(ks)))
    return multi, unrouted


def analyse(lab, tag):
    attrib, orphan = net_attribution(lab)
    attrib = stitch(lab, attrib)
    return report(attrib, orphan, tag) + (attrib,)



def run(mode):
    CL = {k: classify(k, mode) for k in ENTRY}
    for k in ENTRY:
        r, tr, rl, po, fl = CL[k]
        print("%s/%s tracks=%d relief=%d poursegs=%d flashes=%d regions=%d"
              % (k, mode, len(tr), len(rl), len(po), len(fl), len(r)))

    def stage(relief, pour, regions):
        lab = {}
        for k in ENTRY:
            rg, tr, rl, po, fl = CL[k]
            img = Image.new("L", (C.W, C.H), 0)
            d = ImageDraw.Draw(img)
            for g in [tr] + ([rl] if relief else []) + ([po] if pour else []):
                for a, x1, y1, x2, y2 in g:
                    d.line([(x1, y1), (x2, y2)], fill=255,
                           width=max(1, int(round(C.ap_params(a)[1]))))
            if regions:
                for r in rg:
                    d.polygon(r, fill=255)
            for a, x, y in fl:
                C.draw_aperture(d, a, x, y, 255)
            lab[k] = C.label(img)
        return lab

    analyse(stage(0, 0, 0), "S1 pads + real tracks only")
    analyse(stage(1, 0, 0), "S2 + relief spokes")
    analyse(stage(1, 1, 0), "S3 + pour outline strokes")
    analyse(stage(0, 0, 1), "S5 + pour solid regions, no outline")
    lab = stage(1, 1, 1)
    analyse(lab, "S4 full CAM picture")
    for k, im in lab.items():
        im.save(os.path.join(HERE, "_i28_%sS4_%s.png" % (mode, k)))


if __name__ == "__main__":
    run(sys.argv[1] if len(sys.argv) > 1 else "A")
