"""Exact-geometry audit of the A-board: clearance + connectivity, from live EDA data."""
import io, json, math, sys

TOP, BOT, OUTLINE = 1, 2, 11
CLR = 8.0            # signal clearance we hold ourselves to (JLC basic is 5-6mil)
EDGE = 25.0          # copper to board edge
POWER = {"PACK_N", "SW_N"}


def _load():
    pads = json.load(io.open("_pads2.json", encoding="utf-8"))
    cop = json.load(io.open("_copper.json", encoding="utf-8"))
    uniq, out = {}, []
    for p in pads:
        k = (round(p["x"], 1), round(p["y"], 1), p["net"], p["ptype"] == "ComponentPad")
        alt = (round(p["x"], 1), round(p["y"], 1), p["net"], not (p["ptype"] == "ComponentPad"))
        if alt in uniq:
            u = uniq[alt]                      # merge TH mirror rows: keep hole info
            if p.get("hole"):
                u["thru"] = True
            continue
        if k in uniq:
            if p.get("hole"):
                uniq[k]["thru"] = True
            continue
        d = dict(p)
        w, h = p["shape"][1], p["shape"][2]
        if int(round(p.get("rot") or 0)) % 180:
            w, h = h, w                        # getState_Pad() does NOT swap on rotate
        d["w"], d["h"] = w, h
        d["thru"] = bool(p.get("hole")) or p.get("layer") == 12
        uniq[k] = d
        out.append(d)
    return out, cop


def shape(p):
    x, y, w, h = p["x"], p["y"], p["w"], p["h"]
    return ("rect", x - w / 2.0, y - h / 2.0, x + w / 2.0, y + h / 2.0)


def layers_of(p):
    if p["thru"]:
        return (TOP, BOT)
    return (p["layer"],)


def _pt_seg(px, py, x1, y1, x2, y2):
    dx, dy = x2 - x1, y2 - y1
    if dx == dy == 0:
        return math.hypot(px - x1, py - y1)
    t = ((px - x1) * dx + (py - y1) * dy) / (dx * dx + dy * dy)
    t = max(0.0, min(1.0, t))
    return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))


def _seg_seg(a, b):
    (x1, y1), (x2, y2) = a
    (x3, y3), (x4, y4) = b
    if _orient(x1, y1, x2, y2, x3, y3) * _orient(x1, y1, x2, y2, x4, y4) < 0 and \
       _orient(x3, y3, x4, y4, x1, y1) * _orient(x3, y3, x4, y4, x2, y2) < 0:
        return 0.0
    return min(_pt_seg(x1, y1, x3, y3, x4, y4), _pt_seg(x2, y2, x3, y3, x4, y4),
               _pt_seg(x3, y3, x1, y1, x2, y2), _pt_seg(x4, y4, x1, y1, x2, y2))


def _orient(ax, ay, bx, by, cx, cy):
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)


def _rect_rect(a, b):
    if a[0] < b[2] and b[0] < a[2] and a[1] < b[3] and b[1] < a[3]:
        return 0.0
    ex = []
    x1, y1, x2, y2 = a
    ex += [((x1, y1), (x2, y1)), ((x2, y1), (x2, y2)), ((x2, y2), (x1, y2)), ((x1, y2), (x1, y1))]
    x1, y1, x2, y2 = b
    ey = [((x1, y1), (x2, y1)), ((x2, y1), (x2, y2)), ((x2, y2), (x1, y2)), ((x1, y2), (x1, y1))]
    return min(_seg_seg(p, q) for p in ex for q in ey)


def _rect_seg(r, s):
    x1, y1, x2, y2 = r
    edges = [((x1, y1), (x2, y1)), ((x2, y1), (x2, y2)), ((x2, y2), (x1, y2)), ((x1, y2), (x1, y1))]
    if _seg_seg(s, ((x1, y1), (x2, y2))) == 0 or _seg_seg(s, ((x1, y2), (x2, y1))) == 0:
        return 0.0
    # endpoint inside rect?
    for (px, py) in s:
        if r[0] <= px <= r[2] and r[1] <= py <= r[3]:
            return 0.0
    return min(_seg_seg(s, e) for e in edges)


def dist(A, B):
    if A[0] == "rect" and B[0] == "rect":
        return _rect_rect(A[1:], B[1:])
    if A[0] == "rect":
        return _rect_seg(A[1:], B[1:])
    if B[0] == "rect":
        return _rect_seg(B[1:], A[1:])
    return _seg_seg(A[1:], B[1:])


def capsule(t):
    return ("seg", (t["x1"], t["y1"]), (t["x2"], t["y2"]), t["width"] / 2.0)


def gap(a, b):
    """edge-to-edge distance between (kind, geo, halfwidth) tuples."""
    return _gap(a, b)


def _gap(a, b):
    ga, gb = a[1], b[1]
    if ga[0] == "seg" and gb[0] == "seg":
        raw = _seg_seg(ga[1:], gb[1:])
    elif ga[0] == "rect" and gb[0] == "rect":
        raw = _rect_rect(ga[1:], gb[1:])
    elif ga[0] == "rect":
        raw = _rect_seg(ga[1:], gb[1:])
    else:
        raw = _rect_seg(gb[1:], ga[1:])
    return raw - a[2] - b[2]


def items(pads, tracks):
    out = []
    for p in pads:
        r = shape(p)
        for lay in layers_of(p):
            out.append({"kind": "pad", "net": p["net"], "des": p["des"], "pn": p["pn"],
                        "layer": lay, "geo": r, "hw": 0.0, "x": p["x"], "y": p["y"]})
    for t in tracks:
        if t.get("via"):
            for lay in (TOP, BOT):
                out.append({"kind": "via", "net": t["net"], "layer": lay,
                            "geo": ("rect", t["x1"] - t["width"] / 2, t["y1"] - t["width"] / 2,
                                    t["x1"] + t["width"] / 2, t["y1"] + t["width"] / 2),
                            "hw": 0.0, "x": t["x1"], "y": t["y1"]})
        else:
            out.append({"kind": "tr", "net": t["net"], "layer": t["layer"],
                        "geo": ("seg", (t["x1"], t["y1"]), (t["x2"], t["y2"])),
                        "x": (t["x1"] + t["x2"]) / 2.0, "y": (t["y1"] + t["y2"]) / 2.0,
                        "hw": t["width"] / 2.0, "id": t["id"]})
    return out


def violations(pads, copper, box=None):
    """pairwise clearance check -> {kind: [rows]}"""
    its = items(pads, copper)
    viol = {"TR-TR": [], "PAD-TR": [], "PAD-PAD": [], "EDGE": [], "EDGE-TR": []}
    for i in range(len(its)):
        a = its[i]
        for j in range(i + 1, len(its)):
            b = its[j]
            if a["net"] == b["net"] and a["net"]:
                continue
            if a["layer"] != b["layer"]:
                continue
            g = _gap((None, a["geo"], a["hw"]), (None, b["geo"], b["hw"]))
            if g >= CLR:
                continue
            key = "-".join(sorted(("TR" if a["kind"] != "pad" else "PAD",
                                   "TR" if b["kind"] != "pad" else "PAD")))
            viol[key].append((round(g, 2), a["net"], b["net"],
                              "%s.%s" % (a.get("des", a["kind"]), b.get("des", b["kind"])),
                              (round(a["x"], 1), round(a["y"], 1)), (round(b["x"], 1), round(b["y"], 1))))
    if box:
        bx1, bx2, by1, by2 = box
        for a in its:
            r = a["geo"]
            if r[0] == "seg":
                x1, x2 = min(r[1][0], r[2][0]), max(r[1][0], r[2][0])
                y1, y2 = min(r[1][1], r[2][1]), max(r[1][1], r[2][1])
            else:
                x1, y1, x2, y2 = r[1:]
            d = min(x1 - bx1, bx2 - x2, y1 - by1, by2 - y2) - a["hw"]
            if d < EDGE:
                viol["EDGE" if a["kind"] == "pad" else "EDGE-TR"].append(
                    (round(d, 1), a["net"], a.get("des", a["kind"]), (round(a["x"], 1), round(a["y"], 1))))
    return viol


def run():
    pads, tracks = _load()
    copper = [t for t in tracks if t["layer"] in (TOP, BOT) or t.get("via")]
    outline = [t for t in tracks if t["layer"] == OUTLINE]
    xs = [c for t in outline for c in (t["x1"], t["x2"])]
    ys = [c for t in outline for c in (t["y1"], t["y2"])]
    bx1, bx2, by1, by2 = min(xs), max(xs), min(ys), max(ys)
    viol = violations(pads, copper, (bx1, bx2, by1, by2))
    conn = connectivity(pads, copper)
    print("board box mil: (%g,%g)-(%g,%g) = %.2f x %.2f mm" % (bx1, by1, bx2, by2,
          (bx2 - bx1) * 0.0254, (by2 - by1) * 0.0254))
    print("pads %d  tracks %d" % (len(pads), len(copper)))
    for k, v in viol.items():
        print("%-8s %4d" % (k, len(v)))
        for row in sorted(v)[:12]:
            print("        ", row)
    print("disconnected: %d" % len(conn))
    for net, info in conn.items():
        print("   %-12s pads=%d islands=%d" % (net, info[0], info[1]))
    return viol, conn


def _node_layers(n):
    return n[3]


def _pour_rects():
    """Pours are not tracks, so the live polygon is mirrored by hand into
    _pour_rects.json (rectangles only) to let connectivity see the plane."""
    try:
        return json.load(io.open("_pour_rects.json", encoding="utf-8"))
    except Exception:
        return []


def connectivity(pads, copper):
    parent = {}

    def find(a):
        while parent.setdefault(a, a) != a:
            a = parent[a]
        return a

    def union(a, b):
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb
    nodes = []
    for p in pads:
        nodes.append(("pad", p, shape(p), set(layers_of(p))))
    for t in copper:
        if t.get("via"):
            r = t["width"] / 2.0
            nodes.append(("via", t, ("rect", t["x1"] - r, t["y1"] - r, t["x1"] + r, t["y1"] + r),
                          {TOP, BOT}))
        else:
            nodes.append(("tr", t, ("seg", (t["x1"], t["y1"]), (t["x2"], t["y2"])), {t["layer"]}))
    for q in _pour_rects():
        nodes.append(("pour", q, ("rect", q["x1"], q["y1"], q["x2"], q["y2"]), {q["layer"]}))
    bynet = {}
    for idx, n in enumerate(nodes):
        bynet.setdefault(n[1].get("net") or "", []).append(idx)
    res = {}
    for net, idxs in bynet.items():
        if not net:
            continue
        for a in range(len(idxs)):
            for b in range(a + 1, len(idxs)):
                na, nb = nodes[idxs[a]], nodes[idxs[b]]
                if not (_node_layers(na) & _node_layers(nb)):
                    continue
                if _gap((na[2][0], na[2], 0.0), (nb[2][0], nb[2], 0.0)) <= 1.0:
                    union(idxs[a], idxs[b])
        pads_only = [i for i in idxs if nodes[i][0] == "pad"]
        roots = set(find(i) for i in pads_only)
        if len(pads_only) > 1 and len(roots) > 1:
            res[net] = (len(pads_only), len(roots))
    return res


if __name__ == "__main__":
    run()
