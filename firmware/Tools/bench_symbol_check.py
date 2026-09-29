#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
bench_symbol_check.py —— 「到货当天照做的那几页」里点名的符号，必须在**现役**工程里真的存在。

为什么要有它（本项目的第 N 次同族命中）：
  · 文档里写「读 `xxx()`」不等于镜像里真有 `xxx()` —— 链接器会把**没人调用的**符号整段删掉
    （`app_log_dump()` 的教训 / `附录C` **C-140**），编译器还会把**被内联**的小函数删掉。
  · 到货当天只有一次机会：照着一页纸走下去，卡在一个不存在的符号上 = 白等一天。
  · 这个检查**只查"照着做"的那几节**（`开工执行清单.md` §3.6 / §5.1~§5.5 ＋ `Build.md` §2），
    ⛔ 不查历史账本（`Build.md` §8.x / `附录C` 里点名的旧符号是**当时的留痕**，本来就不该动）。

两条判据（分级）：
  L1（★ 会判红）= 文档点名的标识符必须在**现役固件源码**里出现过（函数 / 宏 / 枚举 / 字段都算）。
                    ── 命中不了 ⇒ 要么写错了名、要么那个东西已经被删/改名 ⇒ 现场必然扑空。
  L2（★ 也会判红）= 名字后面带 `()`（= 文档把它当**函数**读）的，必须在**现役 `bms.map`** 里有地址。
                    ── 没地址的两种成因: 被内联 / 被裁剪 / 只在注释里 ⇒ 现场按这个名字读不到。
                    例外（★ 必须带**到期条件**，写在下方的 `L2_EXCUSE` 里，且到期要删）：
                    宏（源码里有 `#define`，读法本来就是读源码）与 `L2_EXCUSE` 里登记过的"还没接线"函数。
                    ★ 反向也查：豁免还在、而符号**已经**进镜像 ⇒ 判"豁免过期"，同样判红。

用法：
    python3 Tools/bench_symbol_check.py                    # 用 Output/bms.map
    python3 Tools/bench_symbol_check.py --map <path>       # 指定 map（例如归档件）
退出码：L1 命中缺失 ⇒ 1；否则 0（L2 永远不判红，只列出来给下一轮收）。
"""
import argparse
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)                     # firmware/
PROJ = os.path.dirname(FW)                     # 项目根
DOCS = {
    "开工执行清单.md": ["### 3.6", "### 5.1", "### 5.2", "### 5.3", "### 5.4", "### 5.5"],
    "Build.md": ["## 2."],
}

# 源码语料（现役固件）。⛔ 不含 Output/ 里的构建产物；含 Middlewares/ 以便 FreeRTOS API 名也能解析。
SRC_DIRS = ["User", "Drivers", "Middlewares"]

# ── 非符号白名单（每条都要有理由；⛔ 不许往这里塞"其实已经不存在但我不想改文档"的名字）──
ALLOW = {
    # 调试器 / 工具命令与文件
    "mdw", "mdh", "mdb", "reg", "pc", "halt", "reset", "init", "verify_image", "Verified",
    "openocd", "sysfs", "hex", "map", "log", "dump",
    # 器件与型号
    "bq7692003PWR", "bq76920", "INA226", "SHT30", "AT24C02", "STM32F103ZET6", "CH340",
    # 引脚 / 网络名（硬件对象，不是固件符号）
    "PD11", "PD12", "PD13", "PB6", "PB7", "PA6", "PA9", "PA10", "PC0", "PE0", "PE1", "V_SW",
    "SRN", "SRP", "P5", "P3", "P6", "P7", "B0", "B1", "Rsns", "DIFF", "ALERT", "GND",
    # 协议 / 框架术语
    "Modbus", "RTU", "OneNET", "WiFi", "CRC16", "ACK", "NACK", "SOC", "OCV", "SOH", "LFP",
    "SAFE_OFF", "OK", "TODO", "IDLE", "DMA", "EXTI", "IWDG", "LSI", "LSE", "BKP", "RTC",
    # ★ 引脚功能名（《精英V2 IO引脚分配表》的口径，⛔ 不是固件符号 —— 固件里叫 ADC_CHANNEL_5 /
    #   DAC_CHANNEL_2）。写在这里是为了**允许文档用板级口径描述引脚**，而不是放行写错的名字。
    "DAC_OUT2", "ADC1_IN5", "ADC12_IN5",
    # 纯占位/格式
    "n", "m", "x", "y", "a", "b", "i", "k", "s", "t", "v", "w", "d", "e", "f", "g",
}

# ── L2 豁免（★ 每条必须带**到期条件**；到期不还 ⇒ 这一条就该删掉或把它接上线）──
#   判据：带 `()` 却是"还没接线"的功能函数 —— 现役无人调用 ⇒ 镜像里没有地址，这是**预期**。
L2_EXCUSE = {
    "afe_hw_fault": "D8 才接线（`afe.h` 注释：由 TaskProtect 在 ALERT 上升沿后调用；今天无调用者）⇒ 到期 D8",
}

# ── 反例名（★ 文档**故意**写出来的错误名字，用来说明"本板没有这个东西"）──
#   与 ALLOW 的区别：ALLOW = 不是固件符号；这里是**读者可能真去试的名字**，必须写出来警告。
#   ★ 反向自检：万一哪天这个名字真的进了源码/镜像，说明本条已过期 ⇒ 判红要求删掉。
KNOWN_WRONG = {
    "BKP0R": "F4/L4 的备份域命名；本板（F1）叫 `BKP->DRn` —— §5.4 明确警告⛔别用",
    "BKP3R": "同上（`BKP0R`~`BKP3R` 是 F4/L4 叫法）",
    "ADC_IN5": "漏了系列号的名字（官方口径 = `ADC12_IN5`，本工程用 `ADC1_IN5`）",
}


def read_sections(path, prefixes):
    """按标题前缀切出目标节；返回 [(节标题, 正文)]。⛔ 前缀一个都没命中 ⇒ 抛异常（判据不成立）。"""
    with open(path, encoding="utf-8") as fh:
        lines = fh.read().split("\n")
    heads = [i for i, ln in enumerate(lines) if re.match(r"^#{1,4} ", ln)]
    out, hit = [], set()
    for pref in prefixes:
        found = False
        for i, ln in enumerate(lines):
            if ln.startswith(pref):
                found = True
                hit.add(pref)
                end = next((j for j in heads if j > i), len(lines))
                out.append((ln.strip(), "\n".join(lines[i:end])))
        if not found:
            raise SystemExit("✗ 判据不成立：在 %s 里找不到目标节 %r" % (os.path.basename(path), pref))
    return out


def load_sources():
    blob = []
    for d in SRC_DIRS:
        root = os.path.join(FW, d)
        for dirpath, _dirnames, filenames in os.walk(root):
            for fn in filenames:
                if os.path.splitext(fn)[1] in (".c", ".h", ".s", ".S"):
                    p = os.path.join(dirpath, fn)
                    try:
                        blob.append(open(p, encoding="utf-8", errors="replace").read())
                    except OSError:
                        pass
    return "\n".join(blob)


def load_map_names(map_path):
    """从 map 里取『有地址的符号』= 真的进了镜像（函数 / 变量 / 段）。"""
    names = set()
    with open(map_path, encoding="utf-8", errors="replace") as fh:
        for ln in fh:
            m = re.match(r"^    ([A-Za-z_][A-Za-z0-9_]*)\s+0x[0-9a-fA-F]+\s+", ln)
            if m:
                names.add(m.group(1))
    return names


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--map", default=os.path.join(FW, "Output", "bms.map"))
    args = ap.parse_args()

    if not os.path.isfile(args.map):
        raise SystemExit("✗ 找不到 map：%s（先编译一次）" % args.map)

    src = load_sources()
    map_names = load_map_names(args.map)
    if len(map_names) < 500:
        raise SystemExit("✗ 判据不成立：map 里只解析出 %d 个符号（正常应 >500）" % len(map_names))

    l1_fail, l2_fail, l2_ok_excuse, ok_cnt = [], [], [], 0
    wrong_seen, wrong_stale = set(), []
    for doc, prefixes in DOCS.items():
        path = os.path.join(PROJ, doc)
        for title, body in read_sections(path, prefixes):
            for ln in body.split("\n"):
                for span in re.findall(r"`([^`\n]+)`", ln):
                    called = span.strip().endswith("()")
                    name = span.strip().rstrip("()").strip()
                    if not re.match(r"^[A-Za-z_][A-Za-z0-9_]*$", name):
                        continue
                    if re.match(r"^[0-9a-fA-F]{8,}$", name):     # md5/sha 片段、hex 常量：不是符号
                        continue
                    if name in ALLOW or len(name) < 3:
                        continue
                    if name in KNOWN_WRONG:                      # 反例名：故意写出来的错误名字
                        wrong_seen.add(name)
                        if re.search(r"\b" + re.escape(name) + r"\b", src):
                            wrong_stale.append(name)             # 它居然进了源码 ⇒ 反例已过期
                        continue
                    if not re.search(r"\b" + re.escape(name) + r"\b", src):
                        l1_fail.append((doc, title, name, ln.strip()[:100]))
                        continue
                    ok_cnt += 1
                    if not called:
                        continue
                    if name in map_names:
                        continue                                   # 有地址 = 现场读得到
                    if ("#define " + name) in src:                 # 宏：编译期常量，读法就是读源码
                        continue
                    if name in L2_EXCUSE:
                        l2_ok_excuse.append((name, L2_EXCUSE[name]))
                        continue
                    l2_fail.append((doc, title, name, ln.strip()[:100]))

    # ★ 豁免到期检查：豁免还在，但符号**已经**进了镜像 ⇒ 那条豁免过期了，必须删
    stale = [n for n in L2_EXCUSE if n in map_names]

    print("== 到货当天脚本 · 符号体检 ==")
    print("查的是：%s" % " ／ ".join("%s %s" % (d, ",".join(p)) for d, p in DOCS.items()))
    print("map = %s（有地址符号 %d 个）" % (os.path.relpath(args.map, PROJ), len(map_names)))
    print()
    print("L1 缺失（★ 判红）= %d 个" % len(l1_fail))
    for doc, title, name, ctx in l1_fail:
        print("   ✗ %-28s [%s / %s]  %s" % (name, doc, title[:24], ctx))
    print()
    print("L2 违规（★ 判红：文档按 `name()` 读，而镜像里没有这个地址）= %d 个" % len(l2_fail))
    for doc, title, name, ctx in l2_fail:
        print("   ✗ %-28s [%s / %s]  %s" % (name + "()", doc, title[:24], ctx))
    print("L2 豁免（带到期条件，%d 条）：" % len(l2_ok_excuse))
    for name, why in sorted(l2_ok_excuse):
        print("   ⚑ %-28s %s" % (name + "()", why))
    if stale:
        print("   ✗ 豁免过期（符号已进镜像 ⇒ 删掉豁免）：%s" % ", ".join(sorted(stale)))
    print("反例名（★ 文档故意写出来的错误名字，%d 条）：" % len(wrong_seen))
    for name in sorted(wrong_seen):
        print("   ⚑ %-28s %s" % (name, KNOWN_WRONG[name]))
    if wrong_stale:
        print("   ✗ 反例名过期（它居然进了源码 ⇒ 本表要删）：%s" % ", ".join(sorted(wrong_stale)))
    print()
    print("在源码里解析成功 = %d 个符号" % ok_cnt)
    if l1_fail or l2_fail or stale or wrong_stale:
        print("---- 有判红项：现场会扑空 / 豁免过期，先改文档或改代码 ----")
        return 1
    print("---- 全绿（L1 无缺失、L2 无未豁免违规）----")
    return 0


if __name__ == "__main__":
    sys.exit(main())
