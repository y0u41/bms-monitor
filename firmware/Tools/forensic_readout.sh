#!/bin/bash
# D5 真机判据 A 组取证（OpenOCD + ST-Link 直读，不经过任何 GUI）
# 覆盖：V1 推进性 / V1 年龄余量 / V2 栈高水位 / V5 NVIC / IWDG 现状 / 栈溢出钩子
# ⚠️ 地址只对"当前在板上的那一版"有效：重烧后必须从最新 firmware/Output/bms.map 按符号名重 grep。
# 用法: sh Tools/forensic_readout.sh [两次心跳采样之间的运行ms，默认1500]
#
# 实测踩过的坑（都写在 -c 分离与 catch 的用法里）：
#   1) 一条 -c 里用分号串多个命令 ⇒ 任一条报错会中断整串（后续 -c 也不执行）。故每条命令独立 -c。
#   2) catch {mdw ...} 会把输出吞掉（mdw 的打印是它的返回值）⇒ 需要看输出的命令 ⛔ 不要包 catch。
#   3) 读 0xE000xxxx（内核私有总线 PPB）必须先 halt；读 SRAM/APB 不用。
#   4) 0xE0042000 = DBGMCU_IDCODE，CR 在 +0x04；DBG_IWDG_STOP = bit8（stm32f103xe.h:11050）。
#   5) "resume → after N {halt} → wait_halt" 在本机超时（timed out while waiting for target halted）
#      ⇒ 推进性改由**两次独立会话**取得：会话一读完就 resume+shutdown 让板子自己跑，
#        bash 侧 sleep，会话二再连一次读。⛔ 不在一次会话里做时间推进。
#   6) ⚠️ 本脚本自己的两条判据曾被实测抓到写错（都属"我写的判据也是待核证据"）：
#      a) ST 的 HAL 把分组**常量本身**编码成了寄存器字段值：
#         stm32f1xx_hal_cortex.h:89~97 ⇒ NVIC_PRIORITYGROUP_4 = 0x00000003U（⛔ 不是 4），
#         core_cm3.h:1489 又是 `PriorityGroupTmp << PRIGROUP_Pos` ⇒ GROUP_4 的正确 AIRCR 读回
#         是 **0xFA050300（PRIGROUP=3）**。按"期望 4"比对会把健康的机器判成违规。
#         ⚠️ 且 PRIGROUP=3 与 =4 在 4 位优先级内核上**效果相同**（分组边界落在未实现位之下 ⇒
#         全部 4 位都归抢占），所以这个 3 不是"被截断"，而是 ST 的正规写法。
#      b) 0xE000ED08 在 Cortex-M3 上是 **VTOR**，不是"优先级位数"寄存器 ⇒ 读 0 属正常
#         （F1 向量表在 0x00000000 别名处）。通路自检改用**非对称常量** CPUID / DBGMCU_IDCODE。
#
set -u

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
GAP_MS="${1:-1500}"
DIR="${TMPDIR:-.}"
OUT_A="$DIR/forensic_A.txt"
OUT_B="$DIR/forensic_B.txt"
[ -x "$OO" ] || { echo "FATAL: openocd.exe not found"; exit 1; }

# ── 地址：★ 一律**现取**，⛔ 不写死在脚本里 ─────────────────────────────
# 为什么改：地址是"某一次构建"的快照，而每次重烧 .bss/.rodata 都会整体挪位。
# 本轮就实测踩到一次 —— 用上一版构建的 `s_worst_age` 地址去读新固件，读到的
# 是 `app_ui.o` 的 .bss，得到一个"UI 年龄 100ms > 窗口 80ms 却没有任何超窗记录"
# 的**假矛盾**（真值 43ms）。这类错的形态是"数看起来很合理、且不报错"。
# ⇒ 判据由脚本自己按符号名从 map 查，长度按 map 的字节数算（数组以后加长也不会少读）。
MAP="${2:-Output/bms.map}"
[ -f "$MAP" ] || { echo "FATAL: 找不到符号表 $MAP（要在 firmware/ 下跑，或把 map 路径作为第 2 参数传进来）"; exit 1; }

addr()  { awk -v n="$1" '$1==n{print $2; exit}' "$MAP"; }
bytes() { awk -v n="$1" '$1==n{print $4; exit}' "$MAP"; }

# ★ 2026-09-28（v0.20）：`s_v3_stall_acq`（V3 注入位）已按计划拆除 ⇒ 从本表**移除**。
#   本脚本其余符号全是永久件，照常可读；⛔ 不要为了这一格去重加注入位。
SYM_ALL="s_beat_tick s_worst_age s_stalled_last s_overflow_count s_overflow_name s_period_ms \
s_over_age_ms s_over_dl_ms \
s_stk_protect s_stk_acq s_stk_comm s_stk_log s_stk_ui"
for n in $SYM_ALL; do
  [ -n "$(addr "$n")" ] || { echo "FATAL: 符号 $n 不在 $MAP ⇒ 要么已删/改名，要么这张 map 不是当前固件的。⛔ 不许沿用旧地址"; exit 1; }
done
echo "  (地址取自 $MAP，构建时间 $(date -r "$MAP" '+%m-%d %H:%M'))"

# ── ★ 期望值也一律**现算**，⛔ 不写死（AGENTS §3.1 ④/⑤）─────────────────
# 为什么改：本脚本原来是硬编码 `40000` 和"RLR 期望 1250"。2026-09-28 实测回填 LSI 之后
# `APP_IWDG_RLR` 变成 1206 ⇒ 照旧判据精确比对会把**健康**的固件判成 MISMATCH（与 C-108
# 那轮"把 1250 写成 0x04D2"是同一类错，只是这次错在"文档里的期望没跟着构建走"）。
# ⇒ 期望从 `app_config.h` 现算；解析不出来就退出，⛔ 不回落到"我记得的数"。
CFG="User/App/app_config.h"
[ -f "$CFG" ] || { echo "FATAL: 找不到 $CFG（要在 firmware/ 下跑）"; exit 1; }
# ★ 取值要分十进制与 0x 两种写法：原来这一行用 `gsub(/[^0-9]/,"")` 把 `0x5354` 抠成 `5354`
#   = 0x14EA ⇒ 魔数期望被算成一个谁都不会出现的数，而"魔数不匹配"恰好是**健康**机器的表现
#   ⇒ 一条永远绿的判据（本轮实测抓到，输出里那对 0x14EA/0x01AF 就是它留下的现场）。
def_int() { awk -v n="$1" '$1=="#define" && $2==n {v=$3; gsub(/[uUlL]/,"",v); if (v ~ /^0[xX]/) { print strtonum(v) } else { gsub(/[^0-9]/,"",v); print v } exit}' "$CFG"; }
LSI=$(def_int APP_LSI_HZ); PR=$(def_int APP_IWDG_PR); TMO=$(def_int APP_IWDG_TIMEOUT_MS)
MAGIC_A=$(def_int APP_STALL_CARRY_MAGIC_A); MAGIC_B=$(def_int APP_STALL_CARRY_MAGIC_B)
SLOT_A=$(def_int APP_STALL_CARRY_SLOT_A); SLOT_B=$(def_int APP_STALL_CARRY_SLOT_B)
SLOT_MASK=$(def_int APP_STALL_CARRY_SLOT_MASK); SLOT_AGE=$(def_int APP_STALL_CARRY_SLOT_AGE)
SLOT_DL=$(def_int APP_STALL_CARRY_SLOT_DL)
for v in LSI PR TMO MAGIC_A MAGIC_B SLOT_A SLOT_B SLOT_MASK SLOT_AGE SLOT_DL; do
  [ -n "${!v}" ] || { echo "FATAL: $CFG 里解析不到 $v ⇒ 宏被改名/删除，⛔ 不许凭记忆补一个数"; exit 1; }
done
PRE=$((4 << PR))                                        # 分频数 = 4·2^编码（RM0008 IWDG_PR）
EXP_RLR=$(( (LSI / PRE) * TMO / 1000 ))                 # 与 APP_IWDG_RLR 同一个算式
EXP_MS=$(( EXP_RLR * PRE * 1000 / LSI ))                # 与 APP_IWDG_REAL_MS 同一个算式
# BKP DR1 在 0x04（stm32f103xe.h:199~200：RESERVED0@0x00 ⇒ DR1@0x04），每格 +4 ⇒ 一次读 10 格
BKP_BASE_ADDR=0x40006C04
echo "  (期望值现算自 $CFG：LSI=$LSI PR=$PR(÷$PRE) RLR=$EXP_RLR 窗口=${EXP_MS}ms | 携带槽位 DR$SLOT_A/DR$SLOT_B=魔数, DR$SLOT_MASK/DR$SLOT_AGE/DR$SLOT_DL=载荷)"

A_BEAT=$(addr s_beat_tick);     N_BEAT=$(( $(bytes s_beat_tick)  / 4 ))
A_AGE=$(addr s_worst_age);      N_AGE=$(( $(bytes s_worst_age)   / 4 ))
A_STALL=$(addr s_stalled_last)
A_OVF_CNT=$(addr s_overflow_count)
A_OVF_NAME=$(addr s_overflow_name); N_OVF_NAME=$(bytes s_overflow_name)
A_PERIOD=$(addr s_period_ms);   N_PERIOD=$(( $(bytes s_period_ms) / 4 ))
# 判活现场（永久量：与 EV_TASK_STALLED 的 aux 两列同源，⛔ 不是临时取证）
A_OVERAGE=$(addr s_over_age_ms)
A_OVERDL=$(addr s_over_dl_ms)
# 五个任务栈：栈名 / 地址 / 字节数全部现取
STACKS=""
for t in protect acq comm log ui; do
  sa=$(addr "s_stk_$t"); sb=$(bytes "s_stk_$t")
  STACKS="$STACKS $t:$sa:$sb"
done

BASE=( -s "$SCRIPTS" -f interface/stlink.cfg -f target/stm32f1x.cfg -c "transport select swd" )

# ── 会话 A：NVIC / 调试控制 / IWDG / 第一次心跳 / 年龄 / 溢出钩子 / 五个栈 ──
A=( "${BASE[@]}" -c init -c halt
  -c "echo {### CPUID}"         -c "mdw 0xE000ED00 1"
  -c "echo {### TYPEPR}"        -c "mdw 0xE000ED08 1"
  -c "echo {### AIRCR}"         -c "mdw 0xE000ED0C 1"
  -c "echo {### IP_36_39}"      -c "mdw 0xE000E424 1"
  -c "echo {### IP_40_43}"      -c "mdw 0xE000E428 1"
  -c "echo {### ISER1}"         -c "mdw 0xE000E104 1"
  -c "echo {### DBGMCU_IDCODE}" -c "mdw 0xE0042000 1"
  -c "echo {### DBGMCU_CR}"     -c "mdw 0xE0042004 1"
  -c "echo {### IWDG}"          -c "mdw 0x40003000 4"
  -c "echo {### BKP}"           -c "mdw $BKP_BASE_ADDR 10"
  -c "echo {### S1_beat}"       -c "mdw $A_BEAT $N_BEAT"
  -c "echo {### S1_age}"        -c "mdw $A_AGE $N_AGE"
  -c "echo {### over_age}"      -c "mdw $A_OVERAGE 1"
  -c "echo {### over_dl}"       -c "mdw $A_OVERDL 1"
  -c "echo {### stalled}"       -c "mdb $A_STALL 1"
  -c "echo {### ovf_cnt}"       -c "mdw $A_OVF_CNT 1"
  -c "echo {### ovf_name}"      -c "mdb $A_OVF_NAME $N_OVF_NAME"
  -c "echo {### period}"        -c "mdw $A_PERIOD $N_PERIOD" )
for e in $STACKS; do
  name=${e%%:*}; rest=${e#*:}; addr=${rest%%:*}; words=$(( ${rest##*:} / 4 ))
  A+=( -c "echo {### STACK_$name}" -c "mdw $addr $words" )
done
A+=( -c resume -c shutdown )

"$OO" "${A[@]}" > "$OUT_A" 2>&1

if grep -qE "unable to connect|init mode failed|Error: unable" "$OUT_A"; then
  echo "CONNECT FAILED（会话 A）："; grep -E "Error|Info : Target voltage" "$OUT_A" | tail -6; exit 2
fi

# ── 板子自己跑 GAP_MS ──
python -c "import time;time.sleep($GAP_MS/1000.0)"

# ── 会话 B：第二次心跳 + 年龄 + 溢出钩子；读完放行 ──
B=( "${BASE[@]}" -c init -c halt
  -c "echo {### S2_beat}"  -c "mdw $A_BEAT $N_BEAT"
  -c "echo {### S2_age}"   -c "mdw $A_AGE $N_AGE"
  -c "echo {### stalled2}" -c "mdb $A_STALL 1"
  -c "echo {### ovf_cnt2}" -c "mdw $A_OVF_CNT 1"
  -c "echo {### IWDG2}"    -c "mdw 0x40003000 4" )

# 会话 B 读完**照样 resume 后 shutdown**：本会话期间 halt 把狗冻住了（`DBG_IWDG_STOP`=1），
# 但调试器一断狗就恢复计数 ⇒ ⛔ 不得"读完就走"，那会让板子停在 halt 态、下次连上来
# 读到的是一个停了几分钟的现场。放行之后它自己继续跑。
B+=( -c resume -c shutdown )
"$OO" "${B[@]}" > "$OUT_B" 2>&1

if grep -qE "unable to connect|init mode failed|Error: unable" "$OUT_B"; then
  echo "CONNECT FAILED（会话 B）："; grep -E "Error|Info : Target voltage" "$OUT_B" | tail -6; exit 2
fi

cat "$OUT_A" "$OUT_B" > "$DIR/forensic_raw.txt"

python - "$DIR/forensic_raw.txt" "$GAP_MS" "$STACKS" \
       "$LSI:$PRE:$EXP_RLR:$EXP_MS:$MAGIC_A:$MAGIC_B:$SLOT_A:$SLOT_B:$SLOT_MASK:$SLOT_AGE:$SLOT_DL" <<'PY'
import re, sys
try:                                    # ★ Git Bash 里 python 的 stdout 默认跟随 GBK 代码页，
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except AttributeError:                  # 而输出里有 ⇒ ✅ ⛔ 这类 GBK 没有的字符
    pass                                # ⇒ 不重设编码会 UnicodeEncodeError 崩在中途（实测两次）
raw = open(sys.argv[1], encoding='utf-8', errors='replace').read()
gap = int(sys.argv[2])
LSI, PRE, EXP_RLR, EXP_MS, MAGIC_A, MAGIC_B, SLOT_A, SLOT_B, SLOT_MASK, SLOT_AGE, SLOT_DL = \
    [int(x) for x in sys.argv[4].split(':')]
def blk(tag):
    m = re.search(r'### ' + re.escape(tag) + r'\n+(.*?)(?=\n### |\Z)', raw, re.S)
    return m.group(1).strip() if m else ''
def vals(tag):
    """去掉每行开头的地址列"""
    out = []
    for line in blk(tag).splitlines():
        parts = line.split()
        if parts and parts[0].startswith('0x') and parts[0].endswith(':'):
            out += [int(p, 16) for p in parts[1:]]
    return out
N = ['Protect', 'Acq', 'Comm', 'Log', 'UI']
print('== 通路自检（非对称常量：读到它 = 字节序与会话都成立）==')
for t, want, note in [('CPUID', 0x411FC231, 'Cortex-M3 r1p1'),
                      ('DBGMCU_IDCODE', 0x10036414, 'F103 互联型 rev 0x1003')]:
    v = vals(t)
    ok = 'OK' if v and v[0] == want else '<== 通路存疑'
    print(f'  {t:<16} 0x{v[0]:08X}  期望 0x{want:08X}  {ok}  # {note}')
print('\n== V5 / 内核与调试控制 ==')
for t, note in [('TYPEPR', '0xED08 = Cortex-M3 VTOR（F1 向量表在 0 别名 ⇒ 读 0 正常），⛔ 不是判据'),
                ('AIRCR', 'GROUP_4 ⇒ 期望 0xFA050300（ST 编码：常量 0x03 直接进 PRIGROUP[10:8]）'),
                ('IP_36_39', 'IP[37]/IP[39]=USART1/USART3 ⇒ 期望字节 1,3 = 0x60'),
                ('IP_40_43', 'IP[40]=EXTI15_10 ⇒ 期望字节 0 = 0x50'),
                ('ISER1', 'USART1=IRQ37→ISER1 bit5, USART3=IRQ39→bit7 ⇒ 0xA0'),
                ('DBGMCU_CR', 'DBG_IWDG_STOP = bit8(0x100)')]:
    b = blk(t).splitlines()
    print(f'  {t:<16} {b[0] if b else "(missing)":<52} # {note}')
cr = vals('DBGMCU_CR')
if cr:
    print(f'  -> DBG_IWDG_STOP(bit8) = {1 if cr[0] & 0x100 else 0}  值=0x{cr[0]:08X}'
          '   ⚠️ 这是调试器会话残留（Keil/ST 插件风格配置），不是固件写的')
w = vals('AIRCR')
if w:
    pg = (w[0] >> 8) & 7
    key = w[0] >> 16
    ok = (w[0] == 0xFA050300)
    print(f'  -> PRIGROUP[10:8] = {pg}   VECTKEY[31:16] = 0x{key:04X}'
          + ('   ✅ 与 GROUP_4 的期望值 0xFA050300 逐位相同'
             if ok else '   ⛔ 不等于 0xFA050300 ⇒ app_nvic_init() 的那笔写没生效，去查调用序'))
    print(f'  -> 生效的抢占位数 = min(4, 8-{pg}) = {min(4, 8 - pg)} 位（GROUP_4 要求 4 位）'
          + ('  ✅' if min(4, 8 - pg) == 4 else '  ⛔'))
iw, iw2 = vals('IWDG'), vals('IWDG2')
for tag, v in (('会话A', iw), ('会话B', iw2)):
    if len(v) >= 4:
        kr, pr, rlr, sr = v[0], v[1], v[2], v[3]
        real = rlr * (4 << pr) / LSI * 1000.0
        print(f'\n== IWDG 现状（{tag}）==  KR=0x{kr:X} PR={pr} RLR={rlr}(0x{rlr:04X}) SR=0x{sr:X}')
        print(f'  -> 按**读回来的这两个数**算的真实窗口 = {rlr} × {4 << pr} / {LSI} = {real:.0f} ms'
              f'   （LSI={LSI} 取自 app_config.h，★ 已实测回填、⛔ 不是标称 40000）')
        print(f'  -> 期望（现算自 app_config.h，⛔ 不是写死的数）：'
              f'RLR={EXP_RLR}=0x{EXP_RLR:04X} 窗口={EXP_MS}ms'
              f'   |   换算式 {EXP_RLR} × {PRE} / {LSI} × 1000 = {EXP_MS}ms')
        print('  -> ' + ('✅ 寄存器现状与设计值一致' if (rlr == EXP_RLR and sr == 0)
                         else '⛔ 不一致 ⇒ 先按 §3.1 ②判断"测早了/没跑到"，再谈缺陷'
                              '（SR 非 0 = PVU/RVU 还没清；RLR=0x0FFF = 那次写被静默丢弃 = C-106 形态）'))
bkb = vals('BKP')
if len(bkb) >= 10:
    print('\n== 备份域携带（★ C-110；DR1..DR10 @ 0x40006C04 起，偏移出处 stm32f103xe.h:199~200）==')
    print('  ' + '  '.join(f'DR{i + 1}=0x{bkb[i]:04X}' for i in range(10)))
    print(f'  （DR1/DR2 = `rtc.c` 在用（时钟源记录 + 已设置标志），⛔ 携带不得动它们）')
    va, vb = bkb[SLOT_A - 1], bkb[SLOT_B - 1]
    live = (va == MAGIC_A and vb == MAGIC_B)
    print(f'  -> 魔数 DR{SLOT_A}/DR{SLOT_B} = 0x{va:04X}/0x{vb:04X}（期望 0x{MAGIC_A:04X}/0x{MAGIC_B:04X}）'
          + ('  ✅ 有携带 ⇒ 此刻正有一次卡死没被复位前的开机读回（正常机器不该看到这一行）'
             if live else '  ✅ 无携带（正常）'))
    if live:
        print(f'     载荷 DR{SLOT_MASK}=0x{bkb[SLOT_MASK - 1]:02X} '
              f'DR{SLOT_AGE}={bkb[SLOT_AGE - 1]}ms DR{SLOT_DL}={bkb[SLOT_DL - 1]}ms')
print('\n== V3 注入位：已于 2026-09-28（v0.20）按计划拆除 ⇒ 本脚本不再读它')
print('   ⛔ 不得改回"按住某个键上电就武装"的形态（那是现场可误触发的人为失效向量）')
b1, b2 = vals('S1_beat'), vals('S2_beat')
a1, a2 = vals('S1_age'), vals('S2_age')
per = vals('period')
print('\n== V1 推进性（tick=1ms；两次独立会话之间板子自由跑了 %d ms）==' % gap)
if b1 and b2:
    print('  idx task     S1        S2        delta')
    starve = []
    for i, n in enumerate(N):
        d = b2[i] - b1[i]
        if d == 0:
            starve.append(n)
        print(f'  {i}   {n:<8} {b1[i]:<9} {b2[i]:<9} {d:>6}' + ('   <== delta=0 ⇒ 该路饿死' if d == 0 else ''))
    print('  -> ' + ('全部 5 路都在推进 ✅' if not starve else '饿死路 = ' + ', '.join(starve)))
print('\n== V1 年龄余量：deadline = 4 × 周期（周期读自镜像 rodata）==')
if per:
    print('  周期(读自镜像) =', ' / '.join(f'{p}ms' for p in per))
    for i, n in enumerate(N):
        dl = 4 * per[i]
        v = max([x for x in [(a1[i] if a1 else 0), (a2[i] if a2 else 0)]])
        print(f'  {n:<8} peak_age={v:>5}ms  deadline={dl:>4}ms  {"OK" if v < dl else "OVER"}  (hex 0x{v:X})')
print('  s_stalled_last  会话A=', blk('stalled').splitlines()[-1].strip() if blk('stalled') else '?')
print('                  会话B=', blk('stalled2').splitlines()[-1].strip() if blk('stalled2') else '?')
print('\n== 判活现场（与 EV_TASK_STALLED 的 aux 两列同源）==')
_oa = vals('over_age'); _od = vals('over_dl')
print(f'  最近一次判活的最坏那一路：age={_oa[0] if _oa else 0}ms  '
      f'deadline={_od[0] if _od else 0}ms   (0/0 = 本次上电还没判活过)')
print('  s_overflow_count=', blk('ovf_cnt').strip(), '/', blk('ovf_cnt2').strip())
print('  s_overflow_name =', blk('ovf_name').strip())
print('\n== V2 栈高水位：从栈低地址数连续 0xA5A5A5A5（= 内核 uxTaskGetStackHighWaterMark 同口径）==')
FLOOR = {'protect': 461, 'acq': 768, 'comm': 1076, 'log': 615, 'ui': 922}
TOTAL = {'protect': 1536, 'acq': 2560, 'comm': 3584, 'log': 2048, 'ui': 3072}
# ⚠️ 上面两张表是**手抄的死值**：栈一旦调大，死底线会把"已经够用"判成"不够用"。
#    改由 shell 侧从 map 现取（argv[3] = "名字:地址:字节数" 列表），30% 底线按总量算。
import math
for e in sys.argv[3].split():
    p = e.split(':')
    if len(p) == 3:
        TOTAL[p[0]] = int(p[2])
        FLOOR[p[0]] = int(math.ceil(int(p[2]) * 0.3))
for n in FLOOR:
    v = vals('STACK_' + n)
    free = 0
    for x in v:
        if x == 0xA5A5A5A5:
            free += 4
        else:
            break
    if not v:
        print(f'  {n:<8} (missing)'); continue
    used = TOTAL[n] - free
    verdict = 'PASS' if free >= FLOOR[n] else ('读到整窗皆填充 ⇒ 扩窗' if free >= len(v) * 4 else 'FAIL')
    print(f'  {n:<8} 总{TOTAL[n]:>5}B  30%底线{FLOOR[n]:>5}B  余量>={free:>5}B  已用<={used:>4}B  {verdict}'
          f'  (窗口 {len(v)} 字)')
PY
