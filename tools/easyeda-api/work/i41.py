"""Signal-integrity / return-path / width review off the live PCB dump."""
import json
import math
import os
import sys
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import i28_cc as C
import i28_arcs as A

G = json.load(open(os.path.join(HERE, "_i40_geo.json"), encoding="utf-8"))
vias, lines = G["vias"], G["lines"]
MM = C.MM2MIL

out = []
out.append("vias=%d  tracks=%d  (units: mil)" % (len(vias), len(lines)))

# ---- via inventory ---------------------------------------------------------
bynet = {}
for v in vias:
    bynet.setdefault(v["net"], []).append(v)
out.append("")
out.append("== via count per net ==")
for n in sorted(bynet, key=lambda k: -len(bynet[k])):
    out.append("   %-10s %d" % (n, len(bynet[n])))
sizes = {}
for v in vias:
    sizes[(v["h"], v["d"])] = sizes.get((v["h"], v["d"]), 0) + 1
out.append("via (hole,dia) mil -> count: " + ", ".join("%s:%d" % (k, v) for k, v in sizes.items()))
out.append("  = %s" % ", ".join("%.3f/%.3f mm x%d" % (k[0] / MM, k[1] / MM, v) for k, v in sizes.items()))

# ---- return-path stitching next to every non-ground signal via ------------
gnd = [v for v in vias if v["net"] in ("VSS", "PACK_N", "SW_N")]
sig = [v for v in vias if v not in gnd]
out.append("")
out.append("== 换层信号过孔旁是否有就近地过孔（回流路径） ==")
SENSITIVE = ("AFE_SDA", "AFE_SCL", "AFE_ALERT", "SRP_F", "SRN_F", "TS1", "VC0", "VC1",
             "VC2", "VC3", "VC4", "VC5", "REGOUT", "CAP1", "AFE_VBAT", "NET_1", "NET_2")
worst = []
for v in sig:
    near = sorted((math.hypot(g["x"] - v["x"], g["y"] - v["y"]), g) for g in gnd)[0]
    if v["net"] in SENSITIVE:
        worst.append((near[0], v, near[1]))
worst.sort()
for d, v, g in worst:
    out.append("   %-10s via (%.0f,%.0f)  最近地孔 %.0f mil = %.3f mm  (%s)"
               % (v["net"], v["x"], v["y"], d, d / MM, g["net"]))
out.append("   sensitive layer-changing vias = %d" % len(worst))

# ---- track widths ----------------------------------------------------------
wn = {}
for l in lines:
    wn.setdefault(l["net"], []).append(l["w"])
out.append("")
out.append("== 每个网络用到的线宽（mil / mm） ==")
for n in sorted(wn, key=lambda k: -max(wn[k])):
    ws = sorted(set(wn[n]))
    out.append("   %-10s n=%3d  %s" % (n, len(wn[n]),
               ", ".join("%dmil=%.3fmm" % (w, w / MM) for w in ws)))
allw = sorted(set(l["w"] for l in lines))
out.append("min track width = %d mil = %.3f mm ; widths used = %s"
           % (allw[0], allw[0] / MM, allw))
out.append("")
out.append("== 功率网络（PACK_N / SW_N / RSNS_*）走线明细 ==")
for n in ("PACK_N", "SW_N", "RSNS_SP", "RSNS_SN"):
    ls = [l for l in lines if l["net"] == n]
    out.append("   %s : %d 段, 最宽 %s mil, 总长 %.1f mm"
               % (n, len(ls), max([l["w"] for l in ls]) if ls else "-",
                  sum(math.hypot(l["x2"] - l["x1"], l["y2"] - l["y1"]) for l in ls) / MM))

# ---- mounting-hole copper in GTL ------------------------------------------
ZIPFILE = os.path.normpath(os.path.join(HERE, "..", "..", "..", "out", "pcb", "bms_A_order_v1.zip"))
C.ZIP = ZIPFILE
for entry in ("Gerber_TopLayer.GTL", "Gerber_BottomLayer.GBL", "Gerber_TopSolderMaskLayer.GTS"):
    ap, reg, st, fla = A.read_layer(entry, "A")
    for (a, x, y) in fla:
        for hx, hy in ((2555, 200), (2555, 1770)):
            if math.hypot(x - hx, y - hy) < 3:
                _, w, h, _ = C.ap_params(a)
                out.append("")
                out.append("!! %s has a flash at the M3 hole (%d,%d): %s %.2f x %.2f mil (%.3f x %.3f mm), hole = 126.0 mil"
                           % (entry, hx, hy, a[0], w, h, w / MM, h / MM))
print("\n".join(out))
open(os.path.join(HERE, "_i41.txt"), "w", encoding="utf-8").write("\n".join(out) + "\n")
