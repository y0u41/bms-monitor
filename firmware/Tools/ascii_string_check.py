#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
ascii_string_check.py —— 固件里**字符串字面量**必须全是 ASCII（门禁 G7-⑨）。

为什么（这条纪律已经**两次**没守住）：
  · 「原因串必 ASCII」是本项目写成纪律的一条（`AGENTS.md`）：现场终端的中文编码不定，
    非 ASCII 打出来就是 `??????`，而**开机诊断行正是到货当天最先要读的东西**。
  · ★ 第一次（v0.61）只改了 `app_main.c` 的 10 处，**漏掉了 `User/main.c` 的 `[AFE] init=` 那行**
    （AFE 诊断的打印点在 `main.c`，不在 `app_main.c`）⇒ 2026-09-30 的真机串口里
    仍能看到 `[AFE] init=3 (1=NACK:?????? PE6 ...)`。
  · ★ 而且当时的"线上验证通过"是**覆盖面不足**的：抓回来的日志是**被截断的日志导出**，
    开机那几行根本没进日志 ⇒ 「没看到 `??????`」被写成了「全 ASCII」。
    ⇒ 本脚本按"**遍历全部字面量并报出总数**"的方式给出覆盖面证明。

判据：本工程自己写的三层（`User/` ＋ `Drivers/BSP/` ＋ `Drivers/SYSTEM/`）里，
      **所有字符串字面量**（注释不算、字符常量不算）不得含 >0x7F 的字节。

用法：  python3 Tools/ascii_string_check.py
退出码：0 = 全 ASCII；1 = 有非 ASCII 字面量（打印 文件:行 + 字节）。
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
FW = os.path.dirname(HERE)
SEARCH = ["User", "Drivers/BSP", "Drivers/SYSTEM"]
EXT = (".c", ".h")

# ── 白名单（★ 每条必须写理由；本项目 UI/日志一律 ASCII，所以现在是空的）──
ALLOW = {}      # "文件:行": "理由"


def strip_comments(text):
    """去掉 /* */ 与 // 注释，保留字符串字面量与字符常量（注释里的中文不算违规）。"""
    out = []
    i, n, state = 0, len(text), None
    while i < n:
        if state is None:
            if text.startswith("/*", i):
                state, i = "block", i + 2
                continue
            if text.startswith("//", i):
                state, i = "line", i + 2
                continue
            c = text[i]
            if c in ('"', "'"):                       # 字面量：整段原样保留（含转义）
                quote, j, buf = c, i + 1, [c]
                while j < n:
                    if text[j] == "\\":
                        buf.append(text[j:j + 2])
                        j += 2
                        continue
                    buf.append(text[j])
                    if text[j] == quote:
                        j += 1
                        break
                    if text[j] == "\n":               # 未闭合（不该发生）：到此为止
                        break
                    j += 1
                out.append("".join(buf))
                i = j
                continue
            out.append(c)
            i += 1
        elif state == "block":
            if text.startswith("*/", i):
                state, i = None, i + 2
            else:
                i += 1
        else:                                          # line
            if text[i] == "\n":
                state = None
                out.append("\n")
            i += 1
    return "".join(out)


def main():
    hits, literals = [], 0
    for root in SEARCH:
        for dirpath, _dirs, files in os.walk(os.path.join(FW, root)):
            for fn in sorted(files):
                if not fn.endswith(EXT):
                    continue
                path = os.path.join(dirpath, fn)
                raw = open(path, "rb").read()
                txt = raw.decode("utf-8", errors="replace")
                cleaned = strip_comments(txt)
                for m in re.finditer(r'"((?:[^"\\\n]|\\.)*)"', cleaned):
                    literals += 1
                    s = m.group(1)
                    bad = [(k, ch) for k, ch in enumerate(s) if ord(ch) > 127]
                    if not bad:
                        continue
                    # 行号：用字面量文本在原文里找（去注释后可能偏移，这里在原文里重找一次同样的串）
                    idx = txt.find('"' + s + '"')
                    line = txt[:idx].count("\n") + 1 if idx >= 0 else 0
                    key = "%s:%d" % (os.path.relpath(path, FW), line)
                    if key in ALLOW:
                        continue
                    hits.append((os.path.relpath(path, FW), line, s, [hex(ord(c)) for _, c in bad[:6]]))

    print("== 固件字符串字面量 ASCII 体检（G7-⑨）==")
    print("搜索根 = %s（排除 Middlewares / HAL / CMSIS）" % ", ".join(SEARCH))
    print("扫过字符串字面量 = %d 个（★ 覆盖面证明：2026-09-30 基线 = **699**；下限取 500 —— 掉到 500 以下说明搜索根或去注释器坏了）" % literals)
    if literals < 500:
        print("✗ 判据不成立：字面量计数异常 ⇒ 判红（⛔ 不许把探针坏了读成全对）")
        return 1
    print("非 ASCII 字面量 = %d 处" % len(hits))
    for f, line, s, codes in hits:
        print("   ✗ %s:%d  字节=%s  内容前 40 字符=%r" % (f, line, ",".join(codes), s[:40]))
    if hits:
        print("---- 有非 ASCII：现场终端会显示成 ?????? ⇒ 改成 ASCII（⛔ 不改判据、不改逻辑）----")
        return 1
    print("---- 全 ASCII ----")
    return 0


if __name__ == "__main__":
    sys.exit(main())
