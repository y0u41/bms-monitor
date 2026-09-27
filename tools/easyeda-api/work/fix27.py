"""Round E: repair the 12 net-name silkscreen labels.

pcb_PrimitiveString.create() is (layer, x, y, text, ...).  fin19/fin19b passed
(text, layer, x, y), so those strings landed on a layer called "CELL1" with the text set
to a number - they do not print at all.  §5-7 and §5-11 both ask for legible labels, so
delete them and re-emit with the right signature, choosing for each label the first
offset direction whose text box keeps 6 mil away from every piece of copper.

  python fix27.py check     # dry run: which site each label would take
  python fix27.py do         # delete broken + create good
  python fix27.py drc        # re-run DRC
  python fix27.py save
"""
import io, json, os, sys
import v4

HERE = os.path.dirname(os.path.abspath(__file__))
GEO = os.path.join(HERE, "_geo26.json")
OPEN = "await eda.dmt_EditorControl.openDocument('%s');" % v4.DOC
CLR = 6.0
FS = 30          # mil, matches the T0..T4 labels already on the board

DUMP = OPEN + """
var G=function(p,n){try{return p['getState_'+n]();}catch(e){return null;}};
var out={pads:[],lines:[],vias:[],strings:[]};
for(const p of await eda.pcb_PrimitivePad.getAll())
  out.pads.push({id:G(p,'PrimitiveId'),x:G(p,'X'),y:G(p,'Y'),l:G(p,'Layer'),net:G(p,'Net'),
    num:G(p,'PadNumber'),pad:G(p,'Pad'),hole:G(p,'Hole')});
for(const l of await eda.pcb_PrimitiveLine.getAll())
  out.lines.push({id:G(l,'PrimitiveId'),net:G(l,'Net'),l:G(l,'Layer'),
    x:G(l,'StartX'),y:G(l,'StartY'),x2:G(l,'EndX'),y2:G(l,'EndY'),w:G(l,'LineWidth')});
for(const v of await eda.pcb_PrimitiveVia.getAll())
  out.vias.push({id:G(v,'PrimitiveId'),net:G(v,'Net'),x:G(v,'X'),y:G(v,'Y'),
    dia:G(v,'Diameter'),hole:G(v,'HoleDiameter')});
for(const s of await eda.pcb_PrimitiveString.getAll())
  out.strings.push({id:G(s,'PrimitiveId'),layer:G(s,'Layer'),x:G(s,'X'),y:G(s,'Y'),text:G(s,'Text')});
return out;"""


def geo():
    if not os.path.isfile(GEO):
        refresh()
    return json.load(io.open(GEO, encoding="utf-8"))


def refresh():
    d = v4.call(DUMP, t=600)
    if not d.get("success"):
        print(json.dumps(d, ensure_ascii=False)[:600])
        return None
    io.open(GEO, "w", encoding="utf-8").write(json.dumps(d["result"], ensure_ascii=False))
    return d["result"]


def copper():
    """rectangles + segments + discs that ink must stay away from"""
    g = geo()
    boxes, segs, discs = [], [], []
    for p in g["pads"]:
        if not p.get("pad"):
            continue
        kind, w, h = p["pad"][0], p["pad"][1], p["pad"][2]
        if p["l"] in (1, 2, 12):
            boxes.append((p["x"] - w / 2.0, p["y"] - h / 2.0, p["x"] + w / 2.0, p["y"] + h / 2.0))
        if p.get("hole") and p["hole"][0] == "ROUND":
            discs.append((p["x"], p["y"], p["hole"][1] / 2.0))
    for l in g["lines"]:
        if l["l"] in (1, 2):
            segs.append((l["x"], l["y"], l["x2"], l["y2"], l["w"] / 2.0))
    for v in g["vias"]:
        discs.append((v["x"], v["y"], v["dia"] / 2.0))
    # ink already on the board that we are keeping must not be overprinted either
    keep = {s["id"] for s in g["strings"] if isinstance(s["layer"], int)}
    for s in g["strings"]:
        if s["id"] not in keep:
            continue
        t = str(s["text"])
        boxes.append((s["x"] - 0.62 * FS * len(t) / 2.0, s["y"] - FS / 2.0,
                      s["x"] + 0.62 * FS * len(t) / 2.0, s["y"] + FS / 2.0))
    return boxes, segs, discs


def box_free(bx, by, bw, bh, items):
    boxes, segs, discs = items
    x0, y0, x1, y1 = bx - bw / 2.0, by - bh / 2.0, bx + bw / 2.0, by + bh / 2.0
    for a, b, c, d in boxes:
        if x0 - CLR < c and x1 + CLR > a and y0 - CLR < d and y1 + CLR > b:
            return False
    for sx, sy, ex, ey, hw in segs:
        if seg_box((sx, sy), (ex, ey), hw + CLR, (x0, y0, x1, y1)):
            return False
    for cx, cy, r in discs:
        if pt_box((cx, cy), r + CLR, (x0, y0, x1, y1)):
            return False
    return True


def seg_box(a, b, r, box):
    """segment (inflated by r) vs axis box"""
    x0, y0, x1, y1 = box
    if pt_box(a, r, box) or pt_box(b, r, box):
        return True
    corners = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
    return any(pt_seg(c, a, b) <= r for c in corners)


def pt_seg(p, a, b):
    dx, dy = b[0] - a[0], b[1] - a[1]
    if dx == dy == 0:
        return ((p[0] - a[0]) ** 2 + (p[1] - a[1]) ** 2) ** 0.5
    t = max(0.0, min(1.0, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / (dx * dx + dy * dy)))
    return ((p[0] - a[0] - t * dx) ** 2 + (p[1] - a[1] - t * dy) ** 2) ** 0.5


def pt_box(p, r, box):
    """disc (centre p, radius r) vs axis box - true when they touch"""
    x0, y0, x1, y1 = box
    nx, ny = max(x0, min(p[0], x1)), max(y0, min(p[1], y1))
    return (p[0] - nx) ** 2 + (p[1] - ny) ** 2 <= r * r


OFFS = [(0, -55), (0, 55), (-95, 0), (95, 0), (0, -110), (0, 110), (-150, 0), (150, 0),
        (0, -165), (0, 165), (-140, -110), (140, -110), (-140, 110), (140, 110),
        (-215, 0), (215, 0)]


def sites():
    """test points and mounting holes on the live board"""
    g = geo()
    out = []
    for p in g["pads"]:
        if p["num"] == "TP":
            out.append((p["net"], p["x"], p["y"]))
        elif p["num"] == "MH":
            out.append(("M3", p["x"], p["y"]))
    return out


SKIP = {"PACK_N"}      # its TP sits in the power strip: the tap lanes are 22-24 mil
                      # apart and a stitch via sits 40 mil above the pad, so no 30-mil
                      # text box anywhere near it keeps 6 mil of clearance.


def plan():
    items = copper()
    rows = []
    for net, x, y in sorted(sites()):
        if net in SKIP:
            continue
        label = {"SRP_F": "SRP", "SRN_F": "SRN", "AFE_ALERT": "ALERT"}.get(net, net)
        w = 0.62 * FS * len(label)
        spot = None
        for dx, dy in OFFS:
            bx, by = x + dx, y + dy
            if bx - w / 2 < 100 or bx + w / 2 > 2656 or by - FS / 2 < 100 or by + FS / 2 > 1880:
                continue
            if box_free(bx, by, w, FS, items):
                spot = (bx, by)
                break
        rows.append((label, x, y, spot, w))
    return rows


def broken():
    return [s["id"] for s in geo()["strings"] if not isinstance(s["layer"], int)]


def do():
    ids = broken()
    js = OPEN
    if ids:
        js += "await eda.pcb_PrimitiveString.delete(%s);" % json.dumps(ids)
    n = 0
    for label, x, y, spot, w in plan():
        if not spot:
            print("   !! no clear site for %s @(%.1f,%.1f)" % (label, x, y))
            continue
        js += ("await eda.pcb_PrimitiveString.create(3,%s,%s,'%s','default',%d,5,5,0,"
               "false,0,false,false);") % (spot[0], spot[1], label, FS)
        n += 1
    js += "return {dropped:%d,created:%d,total:(await eda.pcb_PrimitiveString.getAll()).length};" % (len(ids), n)
    d = v4.call(js, t=600)
    print(json.dumps(d, ensure_ascii=False)[:400])
    refresh()


def drc():
    d = v4.call(OPEN + "var r=await eda.pcb_Drc.check(true,false,true);\nreturn r;", t=900).get("result")
    tot = 0
    for grp in (d if isinstance(d, list) else []):
        for sub in grp.get("list", []):
            if sub.get("list"):
                print("%s / %s : %d" % (grp.get("name"), sub.get("name"), len(sub["list"])))
                tot += len(sub["list"])
    print("TOTAL ERRORS:", tot)


def save():
    print(json.dumps(v4.call(OPEN + "return await eda.pcb_Document.save();"), ensure_ascii=False)[:200])


if __name__ == "__main__":
    m = sys.argv[1] if len(sys.argv) > 1 else "check"
    if m == "check":
        for label, x, y, spot, w in plan():
            print("  %-8s pad(%7.1f,%7.1f) -> ink(%s) w=%.0f %s" %
                  (label, x, y, "%0.1f,%0.1f" % spot if spot else "NONE", w,
                   "OVERLAPS" if not spot else ""))
        print("broken strings to drop:", len(broken()))
    elif m == "do":
        do()
    elif m == "drc":
        drc()
    elif m == "save":
        save()
