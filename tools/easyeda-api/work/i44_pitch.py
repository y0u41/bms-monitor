"""从下单包钻孔文件直接验证每个接插件的孔距（2.50 vs 2.54）。"""
import re, zipfile, os, collections

M2MIL = 39.3700787401575
z = zipfile.ZipFile(os.path.join("..", "..", "..", "out", "pcb", "bms_A_order_v1.zip"))
for entry in ("Drill_PTH_Through.DRL", "Drill_NPTH_Through.DRL", "Drill_PTH_Through_Via.DRL"):
    txt = z.read(entry).decode("ascii", "replace")
    tools = {}
    for m in re.finditer(r"T(\d+)C([\d.]+)", txt):
        tools["T%02d" % int(m.group(1))] = float(m.group(2)) * M2MIL
    cur = None
    holes = []
    for line in txt.splitlines():
        mt = re.match(r"T\d+", line.strip())
        if mt:
            cur = mt.group(0)
            continue
        m = re.search(r"X(-?[\d.]+)Y(-?[\d.]+)", line)
        if m and cur:
            holes.append((round(float(m.group(1)) * M2MIL, 1),
                          round(float(m.group(2)) * M2MIL, 1), cur))
    print("\n===== %s  holes=%d  tools(mil dia)=%s" % (entry, len(holes),
          {k: round(v, 2) for k, v in tools.items()}))
    if entry.endswith("Via.DRL"):
        continue
    rows = collections.defaultdict(list)
    for x, y, t in holes:
        rows[int(round(y / 5) * 5)].append((x, y, t))
    for y in sorted(rows):
        g = sorted(rows[y])
        if len(g) < 2:
            continue
        dx = [round(g[i + 1][0] - g[i][0], 2) for i in range(len(g) - 1)]
        dxd = sorted(set(d for d in dx if d < 300))
        print("  y=%-6d n=%-3d tools=%-12s  相邻孔距(mil): %s  => mm: %s"
              % (y, len(g), ",".join(sorted(set(t for _, _, t in g))),
                 " ".join("%.0f" % d for d in dx),
                 " ".join("%.3f" % (d / M2MIL) for d in dxd)))
