"""Minimal xlsx reader (no openpyxl) + offline layout/TP/BOM checks off the ORDER files."""
import json
import math
import os
import re
import sys
import zipfile
from xml.etree import ElementTree as ET

OUT_DIR = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                        "..", "..", "..", "out", "pcb"))
NS = "{http://schemas.openxmlformats.org/spreadsheetml/2006/main}"


def sheets(path):
    z = zipfile.ZipFile(path)
    shared = []
    if "xl/sharedStrings.xml" in z.namelist():
        root = ET.fromstring(z.read("xl/sharedStrings.xml"))
        for si in root:
            shared.append("".join(t.text or "" for t in si.iter(NS + "t")))
    wb = ET.fromstring(z.read("xl/workbook.xml"))
    names = [s.get("name") for s in wb.iter(NS + "sheet")]
    res = {}
    for i, nm in enumerate(names, start=1):
        f = "xl/worksheets/sheet%d.xml" % i
        if f not in z.namelist():
            continue
        rows = []
        root = ET.fromstring(z.read(f))
        for row in root.iter(NS + "row"):
            vals = []
            for c in row.iter(NS + "c"):
                v = c.find(NS + "v")
                txt = ""
                if v is not None:
                    txt = v.text or ""
                    if c.get("t") == "s":
                        txt = shared[int(txt)]
                else:
                    isn = c.find(NS + "is")
                    if isn is not None:
                        txt = "".join(t.text or "" for t in isn.iter(NS + "t"))
                vals.append(txt)
            rows.append(vals)
        res[nm] = rows
    return res


MM = 39.3700787401575
Z = zipfile.ZipFile(os.path.join(OUT_DIR, "bms_A_order_v1.zip"))
d = json.loads(Z.read("FlyingProbeTesting.json").decode("utf-8"))
F = d["pins"]["fields"]
pins = [dict(zip(F, r)) for r in d["pins"]["rows"]]

lines = []

# ---------------------------------------------------------------- BOM
for fn in ("bms_bom.xlsx",):
    p = os.path.join(OUT_DIR, fn)
    if not os.path.exists(p):
        lines.append("MISSING %s" % fn)
        continue
    for nm, rows in sheets(p).items():
        lines.append("== %s [%s] rows=%d ==" % (fn, nm, len(rows)))
        for r in rows:
            lines.append("   " + " | ".join(x for x in r if x != ""))

# ---------------------------------------------------------------- test points
tp = [p for p in pins if re.search(r"_TP$", p["PIN_NAME"])]
nets = {}
for p in pins:
    nets.setdefault(p["NET_NAME"], []).append(p)
covered = set(p["NET_NAME"] for p in tp)
lines.append("")
lines.append("== test points ==")
lines.append("TP pads = %d   nets with a TP = %d / %d" % (len(tp), len(covered), len(nets)))
for p in sorted(tp, key=lambda q: q["PIN_NAME"]):
    reach = max(p["PAD_SIZEX"], p["PAD_SIZEY"]) / MM
    lines.append("   %-10s net=%-10s %s %.2fmm x %.2fmm (%.3f in) @ (%.1f,%.1f)"
                 % (p["PIN_NAME"], p["NET_NAME"], p["PAD_SHAPE"], p["PAD_SIZEX"] / MM,
                    p["PAD_SIZEY"] / MM, reach / MM, p["PIN_X"], p["PIN_Y"]))
lines.append("nets WITHOUT any test point: " + ", ".join(sorted(set(nets) - covered)))

# ---------------------------------------------------------------- decoupling distance
def comp_of(name):
    return re.sub(r"_\d+$", "", name)


groups = {}
for p in pins:
    groups.setdefault(comp_of(p["PIN_NAME"]), []).append(p)


def nearest_cap(ref_pins, cap_prefix="C", max_mm=25.0):
    out = []
    for rp in ref_pins:
        best = None
        for nm, pl in groups.items():
            if not nm.startswith(cap_prefix) or nm == "":
                continue
            for cp in pl:
                dd = math.hypot(cp["PIN_X"] - rp["PIN_X"], cp["PIN_Y"] - rp["PIN_Y"])
                if best is None or dd < best[0]:
                    best = (dd, nm, cp)
        if best:
            out.append((rp["PIN_NAME"], rp["NET_NAME"], best[1], best[0] / MM))
    return out


u1 = [p for p in groups.get("U1", [])]
u1.sort(key=lambda q: int(re.sub(r"\D", "", q["PIN_NAME"]) or 0))
lines.append("")
lines.append("== 去耦/滤波电容到 U1 引脚的距离（mm，边缘间隙按中心距-盘半径近似） ==")
for pn, net, cap, mm in nearest_cap(u1):
    cp = groups[cap]
    half = max(q["PAD_SIZEX"] for q in cp) / 2.0
    half_u = max(q["PAD_SIZEX"] for q in u1 if q["PIN_NAME"] == pn)[0] if False else 0
    lines.append("   U1 %-6s %-10s  最近电容 %-5s 中心距 %.3f mm" % (pn, net, cap, mm))

lines.append("")
lines.append("== 关键网络走线密度（引脚数） ==")
for n in sorted(nets, key=lambda k: -len(nets[k])):
    lines.append("   %-10s %d pads" % (n, len(nets[n])))

txt = "\n".join(lines)
print(txt[:9000])
open("_i38.txt", "w", encoding="utf-8").write(txt + "\n")
