"""PACK_N / SW_N 主电流通路的铜箔实测（i42 第二版，口径修正）。

栅格 1 px = 1 mil = 0.0254 mm。
载流截面 = 对每个 x 列，把该 net 铜岛落在这一列上的**所有**连续段长度求和
（= 垂直于电流方向 x 的真实铜截面宽度），取路径区间内的最小值；
再 × 铜厚(1oz = 0.035 mm)，顶/底两层并联。

用法: python i42_pour.py
"""
import json, os, sys
from collections import defaultdict
import i28_cc as C
import i28_arcs as A

MIL2MM = 0.0254
ENTRY = {"GTL": ("Gerber_TopLayer.GTL", 1), "GBL": ("Gerber_BottomLayer.GBL", 2)}
SEEDS = {
    "PACK_N": [(400, 180), (600, 180), (799.6, 180)],
    "SW_N": [(1700, 180), (1900, 180), (1500.4, 180)],
}
# 电流真正流经的 x 区间（端子外缘 -> 分流器焊盘内缘），避开铺铜边界的自然收口
PATH = {"PACK_N": (353, 800), "SW_N": (1500, 1947)}


def full_image(lyr):
    ap, regions, strokes, flashes = A.read_layer(ENTRY[lyr][0], "A")
    from PIL import Image, ImageDraw
    img = Image.new("L", (C.W, C.H), 0)
    d = ImageDraw.Draw(img)
    for a, x1, y1, x2, y2 in strokes:
        w = C.ap_params(a)[1]
        d.line([(x1, y1), (x2, y2)], fill=255, width=max(1, int(round(w))))
    for r in regions:
        d.polygon(r, fill=255)
    for a, x, y in flashes:
        C.draw_aperture(d, a, x, y, 255)
    return img


def ipc_dT(I, a_mil2):
    """IPC-2152 外层: I = 0.048 * dT^0.44 * A^0.725"""
    try:
        d = (I / (0.048 * a_mil2 ** 0.725)) ** (1 / 0.44)
    except (OverflowError, ValueError):
        return float("inf")
    return min(d, 9999.0)


def main():
    lab = {k: C.label(full_image(k)) for k in ENTRY}

    res = {}
    for net, seeds in SEEDS.items():
        col = defaultdict(int)
        islands = {}
        for k in ENTRY:
            rows, parent, starts = lab[k]
            found = set()
            for x, y in seeds:
                r = C.label_at(rows, parent, x, y, starts)
                if r is not None:
                    found.add(r)
            islands[k] = found
            for y, runs in enumerate(rows):
                for s, e, l in runs:
                    if C.find(parent, l) in found:
                        a = max(s, 0)
                        for x in range(a, e + 1):
                            col[x] += 1
        prof = sorted((col[x], x) for x in range(*PATH[net]))
        res[net] = (islands, prof)
        print("%s islands=%s %s  |  路径区间 x=%s..%s mil"
              % (net, {k: sorted(v) for k, v in islands.items()}, "", *PATH[net]))
        print("  最窄 6 列(双层合计铜宽): "
              + "  ".join("x=%d %.0fmil=%.2fmm" % (x, w, w * MIL2MM) for w, x in prof[:6]))
        print("  最宽列: %.0f mil = %.2f mm" % (prof[-1][0], prof[-1][0] * MIL2MM))
        w_mil = prof[0][0]
        w_mm = w_mil * MIL2MM
        cs_mm2 = w_mm * 0.035           # 单层 1oz
        cs_tot = cs_mm2                  # 顶层
        # 顶/底并联：分别取各自最窄列
        each = {}
        for k in ENTRY:
            rows, parent, starts = lab[k]
            c2 = defaultdict(int)
            for y, runs in enumerate(rows):
                for s, e, l in runs:
                    if C.find(parent, l) in islands[k]:
                        for x in range(s, e + 1):
                            c2[x] += 1
            each[k] = min(c2[x] for x in range(*PATH[net]))
        print("  分层最窄列: GTL=%.0f mil (%.2f mm)  GBL=%.0f mil (%.2f mm)"
              % (each["GTL"], each["GTL"] * MIL2MM, each["GBL"], each["GBL"] * MIL2MM))
        a_mil2 = (each["GTL"] + each["GBL"]) * 0.035 / MIL2MM  # 截面 mm2 -> mil2 (长单位 mil)
        cs = (each["GTL"] + each["GBL"]) * MIL2MM * 0.035
        print("  => 有效截面(双层,1oz) = %.4f mm2 = %.0f mil2" % (cs, a_mil2 / 1))
        for I in (5, 10, 15):
            print("     I=%2dA  IPC-2152 外层 ΔT≈%.0f ℃   10mm 路径压降≈%.2f mV"
                  % (I, ipc_dT(I, a_mil2), I * 0.175 / cs))
        print("  反查该截面能承受 40℃ 温升的电流: %.1f A"
              % (0.048 * 40 ** 0.44 * a_mil2 ** 0.725))
        res[net] = (each, cs, a_mil2)

    print("\n--- 需要多少铜（IPC-2152 外层, 1oz 铜厚 = 1.378 mil）---")
    t = 0.035 / MIL2MM
    for I in (5, 10, 15):
        for dT in (10, 20, 40):
            a = (I / (0.048 * dT ** 0.44)) ** (1 / 0.725)
            print("   I=%2dA @ΔT=%2d℃  需截面 %.0f mil2 = %.4f mm2  => 1oz 下需铜宽 %.2f mm(双层合计)/%.2f mm(单层)"
                  % (I, dT, a, a * MIL2MM ** 2, a / t * MIL2MM, a / t * MIL2MM / 2))


if __name__ == "__main__":
    main()
