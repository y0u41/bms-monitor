#!/bin/bash
# ============================================================================
#  V3 携带通路探针（★ C-110 的真机验证；配套构建 = v0.16 及以后）
#
#  它验的是三件事，一件比一件难：
#    ① 判活检出了 prio≥2 的停滞（TaskProtect 把 位图/年龄/窗口 写进携带槽）
#    ② 携带真的进了**备份域**（不是 RAM）⇒ 狗复位之后它还在
#    ③ 复位后那次开机把它读回来、补投成 Flash 里的一条 `EV_TASK_STALLED`
#  ⇒ ①② 由本脚本现场直读；③ 由 `sh Tools/log_decode.sh` 事后判读（两条形态互相独立，
#     缺一条时另一条仍成立 —— §2.3「每条真机判据要有第二形态」那条债的正解形态）。
#
# ★ 为什么全程 ⛔ 不 halt：`DBGMCU->CR` 的 `DBG_IWDG_STOP`（bit8）在本机实测 = 1
#   （0x00000307，调试会话残留）⇒ **halt 期间狗被冻住**，注入之后停住就永远不会复位，
#   整个测试会退化成"什么都没发生"。读 SRAM / APB 寄存器本来就不需要 halt（存储器映射）。
# ★ 为什么地址全部现取（AGENTS §3.1 ①⑤）：每次重烧 .bss/.rodata 整体挪位（本项目已实测
#   同一批符号按构建 +0x14 平移过三次）⇒ 一律按符号名从 `Output/bms.map` 查，查不到就退出。
# ★ 为什么槽位/魔数从 `app_config.h` 现算：写死的期望会跟着构建过期（本目录
#   `forensic_readout.sh` 原来那句"RLR 期望 1250"就是活例子）。
#
#  用法: sh Tools/v3_carry_probe.sh [最大采样数，默认 40]     （每个样本 ≈100ms）
#  ⚠️ 本脚本会**故意让板子被狗咬一次**（G3 要求的"人为构造故障"验证记录 = 这一条）。
#  ⚠️ 跑之前确认在板固件就是 `Output/bms.hex` 那一版（脚本会打印 md5 做对账）。
# ============================================================================
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
cd "$HERE" || exit 2

# ★ 2026-09-28 就地修正：原写死的 `/e/DevTools/...` 已不存在（xpack 现装于 `/e/xpack-openocd-0.12.0-7/`）
#   ⇒ 改为「`OPENOCD_ROOT` 环境变量优先 + 候选路径自动探测」，同时兼容 Git Bash 与 WSL 两种写法。
OO_ROOT="${OPENOCD_ROOT:-}"
if [ -z "$OO_ROOT" ]; then
  for c in /e/xpack-openocd-0.12.0-7/xpack-openocd-0.12.0-7 \
           /mnt/e/xpack-openocd-0.12.0-7/xpack-openocd-0.12.0-7 \
           /e/DevTools/xpack-openocd-0.12.0-7/xpack-openocd-0.12.0-7; do
    [ -x "$c/bin/openocd.exe" ] && { OO_ROOT="$c"; break; }
  done
fi
[ -n "$OO_ROOT" ] || { echo "FATAL: 找不到 openocd.exe（用 OPENOCD_ROOT=<xpack 根目录> 指定）"; exit 1; }
OO="$OO_ROOT/bin/openocd.exe"
SCRIPTS="$OO_ROOT/openocd/scripts"
MAP="$HERE/Output/bms.map"
CFG="$HERE/User/App/app_config.h"
ROUNDS="${1:-40}"

[ -x "$OO" ] || { echo "FATAL: 找不到 openocd.exe"; exit 1; }
[ -f "$MAP" ] || { echo "FATAL: 找不到 $MAP"; exit 1; }
[ -f "$CFG" ] || { echo "FATAL: 找不到 $CFG"; exit 1; }

# tcl 丢在 ASCII 路径里：OpenOCD 的 -f/dump 参数走 Windows 路径，中文目录曾出现过编码歧义
TCL_POSIX=$(mktemp /tmp/v3probe_XXXXXX.tcl) || { echo "FATAL: mktemp 失败"; exit 1; }
TCL=$(cygpath -w "$TCL_POSIX" 2>/dev/null || echo "$TCL_POSIX")
trap 'rm -f "$TCL_POSIX"' EXIT

addr() { awk -v n="$1" '$1==n{print $2; exit}' "$MAP"; }
need() { [ -n "$(addr "$1")" ] || { echo "FATAL: 符号 $1 不在 $MAP ⇒ 这张 map 不是当前固件的，或该符号已删/改名。⛔ 不许沿用旧地址"; exit 1; }; }
# ★ 2026-09-28（v0.20）：`s_v3_stall_acq`（V3 注入位）已按计划拆除 ⇒ 不再列入 `need`
#   ⇒ 本脚本降级为**只读巡检**（心跳/年龄/携带槽照读），⛔ 无法再制造卡死（见下方 SKIP-ARM）
for s in s_beat_tick s_stalled_last s_over_age_ms s_over_dl_ms uwTick; do need "$s"; done

# 携带魔数/槽位：从 app_config.h 现取（hex 与十进制两种写法都要能吃 —— 本目录踩过一次
# `gsub(/[^0-9]/,"")` 把 0x5354 抠成 5354 = 0x14EA，于是"魔数不匹配"变成一条永远绿的判据）
def_int() { awk -v n="$1" '$1=="#define" && $2==n {v=$3; gsub(/[uUlL]/,"",v); if (v ~ /^0[xX]/) print strtonum(v); else { gsub(/[^0-9]/,"",v); print v } exit}' "$CFG"; }
MA=$(def_int APP_STALL_CARRY_MAGIC_A); MB=$(def_int APP_STALL_CARRY_MAGIC_B)
SA=$(def_int APP_STALL_CARRY_SLOT_A);  SB=$(def_int APP_STALL_CARRY_SLOT_B)
SM=$(def_int APP_STALL_CARRY_SLOT_MASK); SG=$(def_int APP_STALL_CARRY_SLOT_AGE); SD=$(def_int APP_STALL_CARRY_SLOT_DL)
for v in MA MB SA SB SM SG SD; do
    [ -n "${!v}" ] || { echo "FATAL: $CFG 解析不到 $v ⇒ 宏被改名/删除，⛔ 不许凭记忆补一个数"; exit 1; }
done
# BKP DR1 @ +0x04（stm32f103xe.h:199~200：RESERVED0@0x00 ⇒ DR1@0x04），每格 +4
bkp() { printf '0x%08X' $(( 0x40006C00 + 0x04 + 4 * ($1 - 1) )); }

A_ARM=$(addr s_v3_stall_acq); [ -n "$A_ARM" ] || A_ARM=0    # ★ 0 = 注入位已拆 ⇒ 跳过武装
A_BEAT=$(addr s_beat_tick); A_STALL=$(addr s_stalled_last)
A_OAGE=$(addr s_over_age_ms); A_ODL=$(addr s_over_dl_ms); A_TICK=$(addr uwTick)
A_MA=$(bkp "$SA"); A_MB=$(bkp "$SB"); A_MM=$(bkp "$SM"); A_MG=$(bkp "$SG"); A_MD=$(bkp "$SD")

echo "== 探针地址（现取自 Output/bms.map，构建时间 $(date -r "$MAP" '+%m-%d %H:%M')）=="
echo "   产物对账   Output/bms.hex md5 = $(md5sum Output/bms.hex | cut -d' ' -f1)"
if [ "$A_ARM" = 0 ]; then
    echo "   注入位     已拆除（2026-09-28 / v0.20）⇒ 本次只读巡检，⛔ 不会制造卡死"
else
    echo "   注入位     v3_arm 槽 $A_ARM"
fi
echo "   运行计数   uwTick $A_TICK ｜ s_beat_tick $A_BEAT ｜ s_stalled_last $A_STALL"
echo "   判活现场   s_over_age_ms $A_OAGE ｜ s_over_dl_ms $A_ODL"
echo "   携带槽位   DR$SA=$A_MA(魔数A 0x$(printf %04X "$MA")) DR$SB=$A_MB(魔数B 0x$(printf %04X "$MB"))"
echo "              DR$SM=$A_MM(位图) DR$SG=$A_MG(年龄ms) DR$SD=$A_MD(窗口ms)"
echo ""
if [ "$A_ARM" = 0 ]; then
    echo "!! 注入位已拆 ⇒ 本次**不武装**（只读巡检）。⚠️ 要复验携带/咬狗，须用一次临时注入构建。"
else
    echo "!! 接下来会武装 TaskAcq 的死循环 ⇒ 板子会在 ≤2s 内被狗咬一次。"
fi

cat > "$TCL_POSIX" <<'TCL'
# 地址与魔数由 bash 侧从 map / app_config.h 现取后填进这几行（⛔ 不在这里写死）
SET_ARM
SET_TICK
SET_BEAT
SET_STALL
SET_OAGE
SET_ODL
SET_RMA
SET_RMB
SET_RMM
SET_RMGS
SET_RMD
SET_MA
SET_MB
ROUNDS_

proc r8  {a} { return [lindex [read_memory $a 8 1] 0] }
proc r16 {a} { return [lindex [read_memory $a 16 1] 0] }
proc r32 {a} { return [lindex [read_memory $a 32 1] 0] }

proc sample {i armed} {
    set t   [r32 $::G_TICK]
    set b1  [lindex [read_memory $::G_BEAT 32 5] 1]
    set sl  [r8 $::G_STALL]
    set oa  [r32 $::G_OAGE]
    set od  [r32 $::G_ODL]
    set va  [r16 $::G_RMA]
    set vb  [r16 $::G_RMB]
    set mk  [r16 $::G_RMM]
    set ag  [r16 $::G_RMGS]
    set dl  [r16 $::G_RMD]
    set iw  [read_memory 0x40003000 32 4]
    set live [expr {($va == $::G_MA) && ($vb == $::G_MB)}]
    puts [format "S %02d armed=%d tick=%-9d beatAcq=%-9d stalled=0x%02X over=%d/%d | carry A=0x%04X B=0x%04X mask=0x%02X age=%-5d dl=%-5d LIVE=%d | IWDG PR=%d RLR=%d SR=0x%X" \
        $i $armed $t $b1 $sl $oa $od $va $vb $mk $ag $dl $live \
        [lindex $iw 1] [lindex $iw 2] [lindex $iw 3]]
    return [list $live $t]
}

init
sample 0 0
if {$::G_ARM == 0} {
    puts "SKIP-ARM 注入位已按计划拆除（2026-09-28 / v0.20）⇒ 本次不武装，只读巡检"
} else {
    write_memory $::G_ARM 8 1
    puts [format "ARMED arm=0x%02X" [r8 $::G_ARM]]
}

set prev [r32 $::G_TICK]
set hit  0
set res  0
set n 0
while {$n < $::G_ROUNDS} {
    incr n
    after 100
    set s [sample $n 1]
    set live [lindex $s 0]
    set t    [lindex $s 1]
    if {$live == 1 && $hit == 0} { set hit $n; puts "HIT sample=$n" }
    if {$t < $prev} {
        set res $n
        puts "RESET sample=$n prev_tick=$prev now_tick=$t"
    }
    set prev $t
    if {$res != 0 && $n >= $res + 4} { break }
}
puts [format "VERDICT carry_sample=%d reset_sample=%d last_sample=%d" $hit $res $n]
shutdown
TCL

# 把现取的地址/魔数注入 tcl（用 sed 逐行替换，⛔ 不在 tcl 里写死任何一个数）
sed -i \
  -e "s|^SET_ARM\$|set G_ARM $A_ARM|" \
  -e "s|^SET_TICK\$|set G_TICK $A_TICK|" \
  -e "s|^SET_BEAT\$|set G_BEAT $A_BEAT|" \
  -e "s|^SET_STALL\$|set G_STALL $A_STALL|" \
  -e "s|^SET_OAGE\$|set G_OAGE $A_OAGE|" \
  -e "s|^SET_ODL\$|set G_ODL $A_ODL|" \
  -e "s|^SET_RMA\$|set G_RMA $A_MA|" \
  -e "s|^SET_RMB\$|set G_RMB $A_MB|" \
  -e "s|^SET_RMM\$|set G_RMM $A_MM|" \
  -e "s|^SET_RMGS\$|set G_RMGS $A_MG|" \
  -e "s|^SET_RMD\$|set G_RMD $A_MD|" \
  -e "s|^SET_MA\$|set G_MA $MA|" \
  -e "s|^SET_MB\$|set G_MB $MB|" \
  -e "s|^ROUNDS_\$|set G_ROUNDS $ROUNDS|" \
  "$TCL_POSIX"

RAW=$(mktemp /tmp/v3probe_raw_XXXXXX.txt)
"$OO" -s "$SCRIPTS" -f interface/stlink.cfg -f target/stm32f1x.cfg \
      -c "transport select swd" -c "adapter speed 4000" -f "$TCL" > "$RAW" 2>&1
RC=$?
grep -E "^(S |ARMED|SKIP-ARM|HIT|RESET|VERDICT)" "$RAW" || {
    echo "!! 没有任何样本行 ⇒ 会话没跑通（OpenOCD 退出码 $RC）。原始输出尾部："
    tail -12 "$RAW"; exit 2; }

V=$(grep -E "^VERDICT" "$RAW" | tail -1)
HITN=$(echo "$V" | sed -n 's/.*carry_sample=\([0-9]*\).*/\1/p')
RESN=$(echo "$V" | sed -n 's/.*reset_sample=\([0-9]*\).*/\1/p')
echo ""
echo "== 判定 =="
if [ "${HITN:-0}" != "0" ]; then
    echo "   ① ② ✅ 携带在第 ${HITN} 个 100ms 样本落进备份域（DR3/DR4 两个魔数同时成立）"
else
    echo "   ① ② ⛔ 全程没看到携带 ⇒ 去看 stalled/over 两列：位图有了而魔数没有 = 写路径没跑（DBP/BKP 时钟）"
fi
if [ "${RESN:-0}" != "0" ]; then
    echo "   ✅ 狗确实咬了（第 ${RESN} 个样本 uwTick 回绕）⇒ 携带值必须在这次复位之后仍在（它现在已被开机读取归位）"
else
    echo "   ⛔ 没复位 ⇒ 判活没检出来，或狗没在计数（⚠️ halt 过就会这样：DBG_IWDG_STOP=1）"
fi
echo "   ③ 事后判读（复位后那次开机有没有把这条补进 Flash）："
echo "        sh Tools/log_decode.sh        # 找最新一对 EV_WDG_RESET/EV_BOOT 之后的 EV_TASK_STALLED"
echo "      ★ 期望 code ≠ 0x10（0x10=只有容器）；v0.16 之前**从未出现过带 bit1 的记录**，那正是 C-110。"
rm -f "$RAW"
