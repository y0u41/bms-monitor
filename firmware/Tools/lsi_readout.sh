#!/bin/bash
# ============================================================================
# C 组取证读取（V4 / F3b 实测 LSI ＋ 狗活过 BDRST 的证据回读）
#   配套构建 = v0.15-d5-forensic-lsi-v3（`app_main.c` 的 `⏳ FORENSIC-V4V3` 块）
#   判据本体查 `Build.md` §8.24 与 `开工执行清单.md` §3.5 C 组；本脚本只管"读回来 + 算 + 拒收"。
#
# ★ 为什么所有绝对地址是**运行时从设备头算出来的**（AGENTS §3.1 ①）：
#   本项目已经三次栽在"手抄地址"上（F0/F4 的 TPR/WINR 偏移、BKP_DR1 写成 +0x0C、
#   DBGMCU->CR 写成 BASE）。地址也是一种数字 ⇒ 凭记忆写就等着被证伪。
#   ⇒ 本脚本从 `stm32f103xe.h` 现算 PERIPH/APB1/AHB 三级基址，再**数结构体成员**得到
#     BDCR/CSR/KR/PR/RLR/SR/DRn 的偏移；解析失败就退出，⛔ 不回落到"我记得的值"。
#
# ★ 用法：
#     cd firmware && sh Tools/lsi_readout.sh            # 连板子读一次并出报告
#     sh Tools/lsi_readout.sh --parse-only              # 不连板子，只验"地址推导"这一层
#     sh Tools/lsi_readout.sh --hex path/to/bms.hex     # 换产物做 md5 对账
#
# ⚠️ 三条已知代价/边界（都写进报告里，不靠人记）：
#   1) 本构建每次开机都 `BDRST` ⇒ 备份域**全部**被清（含 BKP DR3..DR8 与 `rtc.c` 的标志），
#      所以 `DR6='CV'` 只在"进死循环之后、狗咬之前"那 ≤2s 窗口内可读；⛔ 复位后再读必为 0。
#      ⇒ V3 的正证挂 Flash 日志（`sh Tools/log_decode.sh`），不挂 DR6。
#   2) `app_log_boot_event()` 开机就 `__HAL_RCC_CLEAR_RESET_FLAGS()`（RMVF）⇒ 事后直读
#      `RCC->CSR` 的 `IWDGRSTF` **必然读到 0**，⛔ 不得当作"没发生过狗复位"的证据。
#   3) halt 会不会冻住狗取决于 `DBGMCU->CR` 的 `DBG_IWDG_STOP`（bit8）—— 本脚本把它的**实测值**
#      打进报告，⛔ 不假设。读完一律 `resume` 再 `shutdown`（否则板子停在 halt 态）。
#
# ⏳ **本脚本随 v0.15 一起转为留档**（2026-09-28，`附录C` C-110）：v0.16 起取证块已从固件拆除
#   ⇒ RTC 不再挂 LSI、开机不再 `BDRST`、DR3..DR8 也不再写 counts/'CV'。跑它只对 md5 对得上
#   （`EXPECT_MD5`，v0.15）的产物有意义。★ 两处别读错：
#     a) 下面第 3 节"期望 RLR=1250"是**v0.15 那一次构建**的期望（当时 `APP_LSI_HZ` 还是标称
#        40000）⇒ ⛔ 不要拿它去比 v0.16 的板子（v0.16 起 `RLR=1206`，期望值改由
#        `Tools/forensic_readout.sh` 从 `app_config.h` **现算**，不再写死）。
#     b) 本构建的 `BDRST` 会把 C-110 的**卡死携带槽位一起抹掉** ⇒ 携带通路只能在 v0.16 及以后验。
# ============================================================================
set -u

HERE=$(cd "$(dirname "$0")/.." && pwd)                 # = firmware/
HDR="$HERE/Drivers/CMSIS/Device/ST/STM32F1xx/Include/stm32f103xe.h"
HEX="$HERE/Output/bms.hex"
EXPECT_MD5="0ed6eeec4e1bd30be7ea0a5ed0bd39c3"          # v0.15-d5-forensic-lsi-v3 的对账值
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
DIR="${TMPDIR:-.}"
ENVF="$DIR/lsi_addr.env"
RAW="$DIR/lsi_raw.txt"

PARSE_ONLY=0
while [ $# -gt 0 ]; do
  case "$1" in
    --parse-only)  PARSE_ONLY=1 ;;
    --hex)         HEX="$2"; shift ;;
    --expect-md5)  EXPECT_MD5="$2"; shift ;;
    *) echo "未知参数：$1（可用 --parse-only / --hex <文件> / --expect-md5 <值>）"; exit 1 ;;
  esac
  shift
done

[ -f "$HDR" ] || { echo "FATAL: 找不到设备头 $HDR（要从 firmware/ 下跑，或在别处时检查路径）"; exit 1; }

# ── ① 地址推导：从设备头算，⛔ 不写死 ────────────────────────────────────
python - "$HDR" "$ENVF" <<'PY'
import re, sys
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    sys.stderr.reconfigure(encoding='utf-8', errors='replace')
except AttributeError:
    pass

hdr = open(sys.argv[1], encoding='utf-8', errors='replace').read()
fails = []

def raw_define(name):
    m = re.search(r'^\s*#define\s+' + name + r'\s+(.*)$', hdr, re.M)
    if not m:
        fails.append('#define %s 不在设备头里' % name)
        return None
    return m.group(1).split('/*')[0].strip()

def strip_u(num):
    return re.sub(r'[UL]+$', '', num.strip())

def resolve(name, seen=()):
    if name in seen:
        fails.append('宏循环展开：%s' % name)
        return 0
    e = raw_define(name)
    if e is None:
        return 0
    return eval_expr(e, name, seen)

def eval_expr(e, who, seen=()):
    """只认三种写法：纯数字 / 单个宏名 / (A + B)、(A << B)。看不懂就报，⛔ 不猜。"""
    e = e.replace('<<', ' << ').replace('(', ' ( ').replace(')', ' ) ').replace('+', ' + ')
    toks = [t for t in e.split() if t not in ('(', ')', '+')]
    if '<<' in toks:
        i = toks.index('<<')
        left = eval_expr(' '.join(toks[:i]), who, seen)
        right = eval_expr(' '.join(toks[i + 1:]), who, seen)
        return left << right
    total = 0
    for t in toks:
        s = strip_u(t)
        if s.startswith('0x'):
            total += int(s, 16)
        elif s.isdigit():
            total += int(s)
        elif re.fullmatch(r'\w+', t):
            total += resolve(t, seen + (who,))
        else:
            fails.append('%s 的写法没覆盖：%r' % (who, t))
    return total

def struct_offsets(typename):
    # 体内不含 '}' ⇒ 用 [^}]* 卡住边界；⛔ 用 .*? 会把前面所有结构体一起吞进来（本机实测过）
    m = re.search(r'typedef struct\s*\{([^}]*)\}\s*' + typename + r'\s*;', hdr, re.S)
    if not m:
        fails.append('结构体 %s 解析失败' % typename)
        return {}
    body = re.sub(r'/\*.*?\*/', '', m.group(1), flags=re.S)      # 整块注释先剥掉，⛔ 别按行数
    out, off = {}, 0
    for line in body.splitlines():
        line = line.strip()
        mm = re.match(r'(?:__IO|__I|__IU|volatile)?\s*(?:const\s+)?uint32_t\s+([A-Za-z_]\w*)\s*(?:\[(\d+)\])?\s*;', line)
        if not mm:
            if line:
                fails.append('%s 里有一行没被数进去：%r' % (typename, line[:60]))
            continue
        nm, cnt = mm.group(1), int(mm.group(2) or 1)
        if not nm.startswith('RESERVED'):
            out[nm] = off
        off += 4 * cnt
    return out

peri = resolve('PERIPH_BASE')
apb1 = resolve('APB1PERIPH_BASE')
ahb = resolve('AHBPERIPH_BASE')
rcc = resolve('RCC_BASE')
bkp = resolve('BKP_BASE')
iwdg = resolve('IWDG_BASE')

iwdg_of = struct_offsets('IWDG_TypeDef')
bKP_of = struct_offsets('BKP_TypeDef')
rcc_of = struct_offsets('RCC_TypeDef')

def macro_vals():
    out = {}
    for k in ('RCC_BDCR_RTCSEL_LSI', 'RCC_BDCR_RTCSEL_LSE', 'RCC_BDCR_RTCEN',
              'RCC_BDCR_RTCSEL_Msk', 'RCC_CSR_LSION', 'RCC_CSR_LSIRDY',
              'RCC_CSR_IWDGRSTF', 'RCC_BDCR_BDRST'):
        e = raw_define(k)
        if e is None:
            continue
        out[k] = eval_expr(e, k)
    return out

sel = macro_vals()
lines = []
w = lines.append
w('# ── 地址推导证据（本机设备头实测，⛔ 不是记忆）──')
w('  PERIPH_BASE   = 0x%08X   (宏直接给)' % peri)
w('  APB1PERIPH    = 0x%08X   AHBPERIPH = 0x%08X' % (apb1, ahb))
w('  IWDG          = 0x%08X   (宏 %s)' % (iwdg, raw_define('IWDG_BASE')))
w('  BKP           = 0x%08X   (宏 %s)' % (bkp, raw_define('BKP_BASE')))
w('  RCC           = 0x%08X   (宏 %s)' % (rcc, raw_define('RCC_BASE')))
if iwdg_of:
    w('  IWDG_TypeDef 逐成员偏移：' + ' / '.join('%s=+0x%02X' % (k, v) for k, v in sorted(iwdg_of.items())))
if bKP_of:
    w('  BKP_TypeDef  DR1..DR10 偏移：' + ' '.join('DR%d=+0x%02X' % (n, bKP_of['DR%d' % n]) for n in range(1, 11)))
if rcc_of:
    w('  RCC_TypeDef  BDCR=+0x%02X  CSR=+0x%02X' % (rcc_of.get('BDCR', -1), rcc_of.get('CSR', -1)))
if sel:
    w('  ★ RTC 源选择位（这一条决定"算出来的是 LSI 还是一个看着正常的假数"）：')
    for k in ('RCC_BDCR_RTCSEL_LSE', 'RCC_BDCR_RTCSEL_LSI'):
        if k in sel:
            w('      %-26s = 0x%04X' % (k, sel[k]))

if fails:
    sys.stderr.write('\n'.join('FATAL(parse): ' + f for f in fails) + '\n')
    sys.exit(1)

need = all(x in iwdg_of for x in ('KR', 'PR', 'RLR', 'SR')) and \
       all(('DR%d' % n) in bKP_of for n in (3, 4, 5, 6, 7, 8)) and \
       'BDCR' in rcc_of and 'CSR' in rcc_of and \
       'RCC_BDCR_RTCSEL_LSI' in sel and 'RCC_BDCR_RTCSEL_LSE' in sel
if not need:
    sys.stderr.write('FATAL(parse): 关键成员/宏没拿到 ⇒ ⛔ 不许回落到记忆值，先修解析\n')
    sys.exit(1)

print('\n'.join(lines))

with open(sys.argv[2], 'w') as f:
    f.write('IWDG_KR=0x%08X\n' % (iwdg + iwdg_of['KR']))
    f.write('IWDG_SR=0x%08X\n' % (iwdg + iwdg_of['SR']))
    f.write('BKP_DR3=0x%08X\n' % (bkp + bKP_of['DR3']))
    f.write('BKP_COUNT=%d\n' % ((bKP_of['DR8'] - bKP_of['DR3']) // 4 + 1))
    f.write('RCC_BDCR=0x%08X\n' % (rcc + rcc_of['BDCR']))
    f.write('RCC_CSR=0x%08X\n' % (rcc + rcc_of['CSR']))
    f.write('RTCSEL_LSI=%d\n' % sel['RCC_BDCR_RTCSEL_LSI'])
    f.write('RTCSEL_LSE=%d\n' % sel['RCC_BDCR_RTCSEL_LSE'])
    f.write('RTCSEL_MASK=%d\n' % sel.get('RCC_BDCR_RTCSEL_Msk', 0x300))
    f.write('RTCEN=%d\n' % sel.get('RCC_BDCR_RTCEN', 0x8000))
    f.write('LSION=%d\n' % sel.get('RCC_CSR_LSION', 1))
    f.write('LSIRDY=%d\n' % sel.get('RCC_CSR_LSIRDY', 2))
    f.write('IWDGRSTF=%d\n' % sel.get('RCC_CSR_IWDGRSTF', 0x20000000))
PY
[ -f "$ENVF" ] || { echo "FATAL: 地址推导没落盘（$ENVF），上面应有 FATAL(parse) 行"; exit 1; }
. "$ENVF"
echo ""

if [ "$PARSE_ONLY" = "1" ]; then
  echo "（--parse-only）地址层已验：DR3=$BKP_DR3 ×6 个 / IWDG KR=$IWDG_KR..SR=$IWDG_SR / BDCR=$RCC_BDCR CSR=$RCC_CSR"
  echo "RTCSEL: LSI=0x$(printf %X $RTCSEL_LSI) / LSE=0x$(printf %X $RTCSEL_LSE) ⇒ v0.15 里 \`RCC_BDCR_RTCSEL_1\` 应落在 **LSI** 那一支"
  exit 0
fi

# ── ② 产物对账（PC 侧，⛔ 不是板上证据）────────────────────────────────
[ -f "$HEX" ] || { echo "FATAL: 找不到 $HEX"; exit 1; }
M=$(md5sum "$HEX" | cut -d' ' -f1)
echo "== 0. 产物对账 =="
echo "  文件      $HEX"
echo "  md5       $M"
echo "  期望 md5  $EXPECT_MD5   (v0.15-d5-forensic-lsi-v3)"
[ -n "$EXPECT_MD5" ] && [ "$M" != "$EXPECT_MD5" ] && echo "  ⛔ md5 不符 ⇒ 手上这个 hex 不是那次归档的取证构建；⚠️ 若你刚重编译过，这是预期的，改 --expect-md5 并**记住读数只属于现在这一版**"
echo "  尺寸      $(stat -c %s "$HEX") B   修改时间 $(date -r "$HEX" '+%m-%d %H:%M')"
echo ""

# ── ③ 一次会话读全部（每条命令独立 -c；⛔ 不要包 catch，见 forensic_readout.sh 的坑 2）──
[ -x "$OO" ] || { echo "FATAL: 找不到 openocd.exe（$OO）"; exit 1; }
BASE=( -s "$SCRIPTS" -f interface/stlink.cfg -f target/stm32f1x.cfg -c "transport select swd" )
R=( "${BASE[@]}" -c init -c halt
  -c "echo {### CPUID}"     -c "mdw 0xE000ED00 1"
  -c "echo {### IDCODE}"    -c "mdw 0xE0042000 1"
  -c "echo {### DBGMCU_CR}" -c "mdw 0xE0042004 1"
  -c "echo {### BKP}"       -c "mdw $BKP_DR3 $BKP_COUNT"
  -c "echo {### IWDG}"      -c "mdw $IWDG_KR 4"
  -c "echo {### BDCR}"      -c "mdw $RCC_BDCR 1"
  -c "echo {### CSR}"       -c "mdw $RCC_CSR 1"
  -c resume -c shutdown )
"$OO" "${R[@]}" > "$RAW" 2>&1
if grep -qE "unable to connect|init mode failed|Error: unable" "$RAW"; then
  echo "CONNECT FAILED："; grep -E "Error|Target voltage" "$RAW" | tail -6
  echo "⇒ 先查接线/供电/ST-Link 驱动，⛔ 不要怀疑固件"
  exit 2
fi

# ── ④ 判定 ──
python - "$RAW" "$ENVF" <<'PY'
import re, sys
try:
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except AttributeError:
    pass

raw = open(sys.argv[1], encoding='utf-8', errors='replace').read()
env = {}
for line in open(sys.argv[2]):
    if '=' in line:
        k, v = line.strip().split('=', 1)
        env[k] = int(v, 0)

def vals(tag):
    m = re.search(r'### ' + tag + r'\n+(.*?)(?=\n### |\Z)', raw, re.S)
    if not m:
        return []
    out = []
    for line in m.group(1).splitlines():
        p = line.split()
        if p and p[0].startswith('0x') and p[0].endswith(':'):
            out += [int(x, 16) for x in p[1:]]
    return out

print('== 1. 通路自检（非对称常量：读到它 = 会话成立、字节序成立）==')
for tag, want, note in [('CPUID', 0x411FC231, 'Cortex-M3 r1p1'),
                        ('IDCODE', 0x10036414, 'F103 互联型')]:
    v = vals(tag)
    ok = '✅' if v and v[0] == want else '⛔ 通路存疑 ⇒ 下面所有读数作废'
    print(f'  {tag:<10} {"0x%08X" % v[0] if v else "(读不到)"}   期望 0x{want:08X}  {ok}  # {note}')

cr = vals('DBGMCU_CR')
frozen = cr and (cr[0] & 0x100)
print(f'\n  DBGMCU_CR = {"0x%08X" % cr[0] if cr else "?"}  ⇒ DBG_IWDG_STOP(bit8) = {1 if frozen else 0}'
      f'   {"⇒ halt 会冻住狗（停住≠狗不咬）" if frozen else "⚠️ halt **不**冻狗 ⇒ 停在halt里也会被咬，读窗口要短"}')

d = vals('BKP')
dr3, dr4, dr5, dr6, dr7, dr8 = (d + [0] * 6)[:6]
print('\n== 2. BKP DR3..DR8 原始读数（★ 只有低 16 位有效）==')
print(f'  DR3 Δcounts = {dr3:<6} (0x{dr3:04X})')
print(f'  DR4 Δms     = {dr4:<6} (0x{dr4:04X})   ← 窗口长度由 HSE 派生的 uwTick 给出（与被测钟无关）')
print(f'  DR5 magic   = 0x{dr5:04X}   期望 0x4C53(\'LS\') 测完 / 0x4641(\'AF\') 通路失败')
print(f'  DR6 \'CV\'    = 0x{dr6:04X}   ⚠️ 本构建每次开机 BDRST ⇒ 只有"咬狗前那 ≤2s"内可读，事后必为 0')
print(f'  DR7 BDCR@测 = 0x{dr7:08X}')
print(f'  DR8 LSI 位  = 0x{dr8:04X}   bit0 LSION={dr8 & 1}  bit1 LSIRDY={(dr8 >> 1) & 1}')

iw = vals('IWDG')
kr, pr, rlr, sr = (iw + [0] * 4)[:4]
bdcr_v = (vals('BDCR') + [0])[0]
csr_v = (vals('CSR') + [0])[0]
print('\n== 3. 狗活过 BDRST 了吗（★ AGENTS §2.3 i) 的"绝不得用屏还亮推断狗活着"）==')
print(f'  IWDG: KR=0x{kr:X} PR={pr} RLR={rlr}(0x{rlr:04X}) SR=0x{sr:X}')
print(f'  -> 期望 PR=4 / RLR=1250=0x{1250:04X}（换算式 2.000s × 40000 ÷ 64）/ SR=0；'
      f'{"✅ 一致" if (pr, rlr, sr) == (4, 1250, 0) else "⛔ 不一致 ⇒ BDRST 把配置带走了而自愈没成立，去查 app_protect_wdg_start() 的第二次调用"}')

print('\n== 4. V4 / F3b 判定 ==')
MASK = env.get('RTCSEL_MASK', 0x300)
sel_live = bdcr_v & MASK
sel_meas = dr7 & MASK
lse, lsi = env['RTCSEL_LSE'], env['RTCSEL_LSI']
print(f'  RTCSEL@测量时刻 = 0x{sel_meas:03X}   {"LSI ✅" if sel_meas == lsi else ("LSE ⇒ **读数全是 LSE 算出来的**" if sel_meas == lse else "没选源/异常")}')
print(f'  RTCSEL@现在     = 0x{sel_live:03X}   {"✅ 与测量时刻一致" if sel_live == sel_meas else "⚠️ 不一致 ⇒ 中间有别的代码改过备份域（或这次读的是重启后的开机态）"}')
print(f'  LSION/LSIRDY 现在 = {(csr_v & env["LSION"]) != 0}/{(csr_v & env["LSIRDY"]) != 0}')

refuse = None
if dr5 == 0x4641:
    refuse = "'AF' = 固件自己宣布通路失败（RTCSEL 没锁存 / 进不了初始化模式 / 计数器越界）⇒ **我不知道**，⛔ 不得读成\"LSI 很慢\""
elif dr5 == 0:
    refuse = 'DR5 = 0 ⇒ 测量从没跑完（板子没起来 / 断电清了备份域 / 烧的根本不是 v0.15）'
elif dr5 != 0x4C53:
    refuse = f'DR5 = 0x{dr5:04X} 不在两个已知态里 ⇒ ⛔ 先怀疑测量路径，不要挑一个假设去套'
elif sel_meas != lsi:
    refuse = 'RTCSEL 不是 LSI ⇒ 这条通路从头就是错的（LSE 32768÷40 = 819.2Hz 会算出一个"落在 30~60kHz 合法区间"的假数）'
elif dr4 == 0:
    refuse = 'Δms = 0 ⇒ 除零，读数作废'

if refuse:
    print(f'\n  ⛔ 拒绝出数：{refuse}')
    print('  ⇒ 按 AGENTS §3.1：读数与所有候选都不符时先查通路；这里固件已给出"通路失败"态，直接按上面那行修。')
else:
    lsi_hz = dr3 * 40 * 1000.0 / dr4
    dev = (lsi_hz - 40000.0) / 40000.0 * 100.0
    win_ms = rlr * (4 << pr) * 1000.0 / lsi_hz if lsi_hz else 0.0
    band = 30000.0 <= lsi_hz <= 60000.0
    print(f'\n  LSI_HZ = Δcounts × 40 × 1000 ÷ Δms = {dr3} × 40 × 1000 ÷ {dr4} = **{lsi_hz:.0f} Hz**')
    print(f'    偏差 vs 标称 40000 = {dev:+.2f}%     计数分辨率 ±1 计数 = {40 * 1000.0 / dr4:.2f} Hz（窗口 {dr4}ms）')
    print(f'    合法域 30~60kHz ⇒ {"✅ 落在域内" if band else "⛔ 不在域内：与所有候选都不符，先怀疑测量路径（预分频没装上？RTC 没在跑？）"}')
    print(f'  ★ 狗的真实窗口（用**实测** LSI，不用标称）= RLR × (4<<PR) / LSI = {rlr} × {4 << pr} / {lsi_hz:.0f} = **{win_ms:.0f} ms**')
    print(f'    vs 设计 2000 ms ⇒ 余量 {2000 - win_ms:+.0f} ms；判活窗（4×周期 + 500ms 检查）必须小于这个数')
    print(f'\n  ⇒ 出数之后**只改一个宏**：`app_config.h` 的 `APP_LSI_HZ` 改成 {round(lsi_hz)}（⛔ 不许顺手挑一个看着更准的数）')
    print('    改完必须重跑 §12.6 的 10 条断言 + 删掉 `⏳ FORENSIC-V4V3` 整块（`grep -rc FORENSIC-V4V3` = 0）+ 重编译重烧复跑 V1/V2')

print('\n== 5. V3 的证据不在这里 ==')
print(f'  RCC_CSR 的 IWDGRSTF(bit29) 现在 = {(csr_v & env["IWDGRSTF"]) != 0}'
      '   ⛔ 这条**不能**当"没发生狗复位"的证据：`app_log_boot_event()` 开机即 RMVF 清标志，'
      '而本构建还额外做了 BDRST')
print('  ⇒ V3 的正证 = Flash 日志：`sh Tools/log_decode.sh` 里要找成对的两条')
print('      EV_TASK_STALLED  code=0x0002(只有 bit1) 且 **aux_a(年龄) > aux_b(窗口=400ms)**')
print('      EV_WDG_RESET     reason bit3(0x0008) = IWDG 咬过')
print('    ⚠️ 只有 EV_WDG_RESET 而没抓到 EV_TASK_STALLED ⇒ 那条记录可能被复位切在写半程（CRC 认得出来）'
      '⇒ 重跑一次，⛔ 不要判成"判活没报"')
print('\n== 6. 上机顺序（一次上机拿 V4/F3b ＋ V3 ＋ 补 V7 观测）==')
print('  ① 正常开机（⛔ 不按任何键）等 ≥8s ⇒ `sh Tools/lsi_readout.sh` = 本报告 ⇒ 出 LSI_HZ')
print('  ② 断电、**按住 KEY0(PE4) 不放**再上电，按住约 6s（3s 稳定期 + 2s 窗口之后才进死循环）')
print('     ⇒ 之后 ≤2s 内应当复位（不按 = 完全正常，这是 V3 的对照组）')
print('  ③ 松开键、让它正常跑起来 ⇒ `sh Tools/log_decode.sh` 解第 ⑤ 节那两条 + 顺便把 V7 的 10 次上电基线读掉')
PY
