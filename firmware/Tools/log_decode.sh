#!/bin/bash
# V7 第二形态：不开串口、不拍照，直接从内部 Flash 日志区把 EV_BOOT 记录解出来
# 记录格式（`User/App/app_log.c:47~56` + `:156~206`，一槽 = 8 个半字 = 16B）：
#   hw[0]=ts_lo hw[1]=ts_hi hw[2]=event hw[3]=code hw[4]=aux_a hw[5]=aux_b
#   hw[6]=CRC-16(0x1021, init 0xFFFF，覆盖 hw[0..5]，每半字先低字节后高字节)
#   hw[7]=提交位（0x0000 = 已提交；0xFFFF = 槽空；其它 = 损坏）
# 区域：APP_LOG_ADDR 0x08072000，28 页 × 2KB（app_config.h §13）
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
LOG_ADDR=0x08072000
LOG_BYTES=57344
DIR="${TMPDIR:-.}"
BIN="$DIR/bms_log_flash.bin"
# ★ 增量基线（V7 的正确判据）：日志区**不因重烧而清**，全区 EV_BOOT 是跨多版固件的历史累计
#   ⇒ 判"这一版上连续 ≥10 次断电重上电"必须按**基线之后**数。基线 = 上次跑完时的"页:槽"写指针。
#   用法: BASE_MARK=6:86 sh Tools/log_decode.sh      （不给 ⇒ 只打全区histogram，⛔ 不得当 V7 判据）
BASE_MARK="${BASE_MARK:-}"

# ⚠️ 参数序 = `dump_image <file> <addr> <size>`（本机 0.12 help 实测）。
#    ★ 2026-09-28 就地更正（AGENTS §3.1 ⑤ 的又一次命中，出处 `附录C` C-110）：本行原来写着
#      "且**必须先 halt**"，理由是"没 halt 时报的错文字指向参数序"。那是把**两个独立事实**缝成了
#      一条 —— 报错确实只说参数序，而"不 halt 能不能 dump"我当时根本没测。本轮实测：
#      **不 halt 也能 dumped 57344 bytes**（Flash 是存储器映射，APB/AHB 读不要求停核）。
#      ⇒ 保留下面的 `-c halt`，但理由换成真的那条：halt 让镜像**静止**（写指针不会在 dump 中途
#        前进），判"某一条记录的邻接关系"时才有意义；只数条数（V7）时 halt 不是必需。
#      ⚠️ 顺带一条：halt 期间狗被 `DBG_IWDG_STOP` 冻住 ⇒ ⛔ 不得用"halt 后不复位"当"狗不咬"的证据。
"$OO" -s "$SCRIPTS" -f interface/stlink.cfg -f target/stm32f1x.cfg -c "transport select swd" \
  -c init -c halt -c "echo {DUMP_START}" -c "dump_image $BIN $LOG_ADDR $LOG_BYTES" -c resume -c shutdown \
  > "$DIR/log_dump.txt" 2>&1
RC=$?
grep -E "DUMP_START|dumped|Error|Warn" "$DIR/log_dump.txt" | tail -4
grep -q "dumped $LOG_BYTES bytes" "$DIR/log_dump.txt" || { echo "FATAL: 没有 'dumped $LOG_BYTES bytes' 这一行 ⇒ 通路未成立"; exit 4; }
[ -f "$BIN" ] || { echo "FATAL: 没拿到镜像（$BIN）"; exit 2; }
SZ=$(stat -c %s "$BIN")
echo "镜像: $BIN  $SZ B（期望 $LOG_BYTES）"
[ "$SZ" = "$LOG_BYTES" ] || { echo "FATAL: 尺寸不符 ⇒ 读数作废，先查通路"; exit 3; }

python - "$BIN" "${BASE_MARK:-}" <<'PY'
import struct, sys
try:                                    # ★ Git Bash 里 python 的 stdout 默认跟随 GBK 代码页
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')
except AttributeError:                  # 而本脚本的输出里有 ⇒ ✅ ⛔ 这类 GBK 没有的字符，
    pass                                # 不重设编码会 UnicodeEncodeError 崩在中途（本轮实测）
data = open(sys.argv[1], 'rb').read()
BASE = None
if len(sys.argv) > 1 and sys.argv[2]:
    bp, bs = sys.argv[2].split(':')
    BASE = (int(bp), int(bs))
SLOT, PG, PAGES = 16, 2048, 28
HDR = 8                       # APP_LOG_HDR_SIZE：每页前 8 字节是页头 ⇒ 槽从 +8 起
SLOTS_PER_PG = (PG - HDR) // SLOT   # 127（⛔ 不是 128）
NAME = {0x0001: 'EV_BOOT', 0x0002: 'EV_WDG_RESET', 0x0010: 'EV_ENV_ALARM',
        0x0011: 'EV_ENV_CLEAR', 0x0050: 'EV_LINK_STALE', 0x0051: 'EV_LINK_RECOVER',
        0x0052: 'EV_TASK_STALLED', 0x00F0: 'EV_LOG_CORRUPT'}
def crc16(hw):
    c = 0xFFFF
    for h in hw:
        for b in (h & 0xFF, h >> 8):
            c ^= b << 8
            for _ in range(8):
                c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c
n_valid = n_blank = n_bad = 0
recs = []
for pg in range(PAGES):
    for slot in range(SLOTS_PER_PG):
        off = pg * PG + HDR + slot * SLOT          # ★ 槽从"页首 + 8"起，⛔ 不是页首
        hw = struct.unpack_from('<8H', data, off)
        if all(h == 0xFFFF for h in hw):
            n_blank += 1
            continue
        if hw[7] != 0x0000:
            n_bad += 1
            continue
        if crc16(hw[:6]) != hw[6]:
            n_bad += 1
            continue
        n_valid += 1
        recs.append((pg, slot, hw))
print(f'槽统计: 有效 {n_valid} / 空 {n_blank} / 损坏或未提交 {n_bad}（总 {PAGES*SLOTS_PER_PG}）')
print('\n== 事件直方图（有效条数）==')
hist = {}
for pg, slot, hw in recs:
    hist[hw[2]] = hist.get(hw[2], 0) + 1
for ev in sorted(hist):
    print(f'  {NAME.get(ev, "EV_%04X" % ev):<16} {hist[ev]:>4}')

# ★ C-110 的观测通路自证：`EV_TASK_STALLED` 的 code 字段就是**任务位图**（`app_tasks.c:131`
#   `mask |= 1u << i`，i = `app_tasks.h:23~27` 的下标）。⛔ 只看总条数不够 —— 原缺陷的形态正是
#   "1226 条全带 bit4、带 bit0~bit3 = 0 条"，而"一条都没有"与"从没卡过"**同形**。
TBL = {0: 'Protect(4)', 1: 'Acq(3)', 2: 'Comm(2)', 3: 'Log(1)', 4: 'Ui/容器(1)'}
st_codes = {}
for pg, slot, hw in recs:
    if hw[2] == 0x0052:
        st_codes[hw[3]] = st_codes.get(hw[3], 0) + 1
print('\n== ★ C-110：EV_TASK_STALLED 按任务位图拆开数（⛔ 总条数会把"通路错"读成"从没卡过"）==')
if not st_codes:
    print('  全区 0 条')
per_bit = [0] * 5
for c in sorted(st_codes):
    names = '+'.join(TBL[b] for b in range(5) if c & (1 << b)) or '(无低 5 位)'
    if c >> 5:
        names += ' + ⚠️ 高位未知(0x%X)' % (c >> 5)
    print(f'  code=0x{c:04X} × {st_codes[c]:>4}  = {names}')
    for b in range(5):
        if c & (1 << b):
            per_bit[b] += st_codes[c]
for b in range(5):
    print(f'  bit{b} {TBL[b]:<12} 出现在 {per_bit[b]:>4} 条')
prio_sum = sum(per_bit[:4])
# ⚠️ 下面两行是**两个不同的自变量**，⛔ 不得相互推算：per_bit 数"位出现了多少次"
#    （一条记录带三位就算三次），n_hi 数"有几条记录带了这些位"。C-110 的判据要的是后者。
n_hi = sum(v for c, v in st_codes.items() if c & 0x0F)
print(f'  -> 带 bit0~bit3（Protect/Acq/Comm/Log —— 这四路任一卡死都会把落盘者一起饿死）'
      f'**的记录** = {n_hi} 条' + (f'（位出现次数之和 = {prio_sum}，⛔ 不是条数）' if prio_sum != n_hi else ''))
if n_hi == 0:
    print('  ⚠️ **两态未分**：要判"从没卡过"还是"卡了、但记录它的日志和它一起被饿死"，'
          '必须看**备份域携带**那条通路（`app_protect.c` 的 `app_protect_stall_carry_report()` 补投，'
          '取证跑 `Tools/v3_carry_probe.sh` 第三段）⇒ ⛔ 不得由本行得出"卡死检测没用武之地"')
else:
    print('  ✅ 带 bit0~bit3 的记录存在 ⇒ 上面 code 含这些位的那些行就是证据'
          '（⚠️ 其中由携带通路补投的那条 `ts` 落在 RTC 同步之前，事件与载荷可信、'
          '`ts` 只当"那次开机的最早时刻"用）')

boot = [r for r in recs if r[2][2] == 0x0001]   # r = (页, 槽, hw) ⇒ hw 在 r[2]
print('\n== 最近 12 条（按 ts_unix 排序；同 ts 按页/槽）==')
def ts_of(hw):
    return hw[0] | (hw[1] << 16)
for pg, slot, hw in sorted(recs, key=lambda r: (ts_of(r[2]), r[0], r[1]))[-12:]:
    print(f'  页{pg:>2} 槽{slot:>3}  ts={ts_of(hw):>9}  {NAME.get(hw[2], "EV_%04X" % hw[2]):<16}'
          f' code=0x{hw[3]:04X} aux=({hw[4]},{hw[5]})')
if n_valid == 0:
    # 通路自检：一条都不认 ⇒ 先怀疑我的布局假设，把前 6 个非空槽的原始半字打出来
    print('\n⚠️ 零条有效：把前 6 个非空槽按原样打出（判"是不是我对页头/槽序的理解错了"）')
    shown = 0
    for pg in range(PAGES):
        for slot in range(SLOTS_PER_PG):
            off = pg * PG + HDR + slot * SLOT
            hw = struct.unpack_from('<8H', data, off)
            if all(h == 0xFFFF for h in hw):
                continue
            print(f'   页{pg:>2} 槽{slot:>3} @0x{0x08072000 + off:08X}: ' +
                  ' '.join(f'{h:04X}' for h in hw))
            shown += 1
            if shown >= 6:
                break
        if shown >= 6:
            break
    print('   页头 8 字节（页0/1/2）: ' + ' | '.join(
        ' '.join(f'{h:04X}' for h in struct.unpack_from('<4H', data, p * PG)) for p in range(3)))
# ⚠️ code 不是 RCC->CSR 原值，而是 `app_main.c:135~141` 压扁后的 6 位紧凑码
#    （1=PINRST，2=POR/PDR，4=SFTRST，8=IWDGRST，16=WWDGRST，32=LPWRRST）
#    F1 的这些标志在 RCC_CSR 的 bit26~31（stm32f103xe.h:1853~1869），固件只取其存在性。
#    ⚠️ 冷上电的本机形态 = **0x0003**（POR 拉低 NRST 的 RC 让 PINRSTF 同时置位），
#    所以 V7 的判据应是"每条都含 bit1(PORRSTF)"，⛔ 不是"每条恰好等于 0x0002"。
RS = {1: 'PIN', 2: 'POR', 4: 'SFT', 8: 'IWDG', 16: 'WWDG', 32: 'LPWR'}
def why(c):
    return '+'.join(v for k, v in RS.items() if c & k) or '(全 0)'
print(f'\n== V7：EV_BOOT 共 {len(boot)} 条 ⇒ 判据"≥10 次冷上电且每次含 POR"看这张直方图 ==')
ch = {}
for pg, slot, hw in boot:
    ch[hw[3]] = ch.get(hw[3], 0) + 1
por_ok = 0
for c in sorted(ch):
    has_por = bool(c & 2)
    por_ok += ch[c] if has_por else 0
    print(f'  code=0x{c:04X} ({c:>2}) × {ch[c]:>3}  = {why(c):<22}'
          + ('  ✅ 含 PORRSTF' if has_por else '  ⛔ 无 PORRSTF ⇒ 这次不是断电重上电（调试器/软件复位）'))
print(f'  -> 含 PORRSTF 的冷上电条数 = {por_ok} / {len(boot)}')
print(f'  ⛔ **这一格不等于 V7 通过**：全区 {len(boot)} 条 EV_BOOT 是**跨多版固件的历史累计**'
      '（D1~D5 每次上电都记在这里，日志区从不因重烧而清）。'
      '⇒ V7 的判据必须是"**当前在板这一版**烧好之后，连续 ≥10 次断电重上电，每次 code 含 bit1(POR)"。'
      '⇒ 跑法 = 先记基线写指针"页:槽"，拔插 ≥10 次后用 `BASE_MARK=页:槽` 重跑本脚本看**增量**。')
if BASE is None:
    print('\n== 未给 BASE_MARK ⇒ 上面全是历史累计，V7 判定不了（判据要按基线之后数）==')
else:
    # ========== 增量：只数基线"页:槽"之后的记录（追加式 = 物理序即写入序）==========
    def after_base(r):
        return (r[0], r[1]) > BASE
    inc = [r for r in recs if after_base(r)]
    inc_boot = [r for r in inc if r[2][2] == 0x0001]
    print(f'\n== ★ V7 增量判定（基线 = 页{BASE[0]} 槽{BASE[1]} 之后）==')
    print(f'  基线后新写入 {len(inc)} 条，其中 EV_BOOT {len(inc_boot)} 条')
    n_por = 0
    for pg, slot, hw in inc_boot:
        has = bool(hw[3] & 2)
        n_por += has
        print(f'  页{pg:>2} 槽{slot:>3}  ts={ts_of(hw):>10}  code=0x{hw[3]:04X} ({why(hw[3]):<12})'
              + ('  ✅' if has else '  ⛔ 不含 PORRSTF ⇒ 这一笔不是断电重上电'))
    print(f'  -> 含 PORRSTF 的冷上电 = {n_por} / {len(inc_boot)}')
    print('  判据 = "≥10 次 且 每一次都含 bit1(PORRSTF)" ⇒ 只有分子算数，⛔ 分母里那几笔不算通过')
    if n_por >= 10:
        print('  ✅ V7 通过（断电重上电 ≥10 次，逐条含 PORRSTF）')
    else:
        print(f'  ⏳ V7 未通过：只有 {n_por} 次冷上电（<10）'
              + (f'，另有 {len(inc_boot) - n_por} 笔非断电复位混在里面' if len(inc_boot) > n_por else ''))
        print('  ⚠️ 调试器 `reset run` / `F5` 也会各写一条 EV_BOOT（code 含 SFT 或 PIN，不含 POR）'
              ' ⇒ 基线之后**只要碰过调试器**就要把它和断电次数分开数，⛔ 不得凑数')
    print(f'  基线后 EV_TASK_STALLED 新增 = {len([r for r in inc if r[2][2] == 0x0052])} 条'
          f' / EV_WDG_RESET 新增 = {len([r for r in inc if r[2][2] == 0x0002])} 条')
    print('  基线后最后 8 条（物理序）：')
    for pg, slot, hw in inc[-8:]:
        print(f'    页{pg:>2} 槽{slot:>3}  ts={ts_of(hw):>10}  {NAME.get(hw[2], "EV_%04X" % hw[2]):<16}'
              f' code=0x{hw[3]:04X} aux=({hw[4]},{hw[5]})')
    print(f'  ⇒ 下一次基线 = 页{recs[-1][0]} 槽{recs[-1][1]}（最后一条有效记录的位置）')
# ⚠️ 上面这个"通过"有边界：232 条 EV_BOOT 是**跨多版固件的历史累计**（今天 D1~D5 全在里面）。
#    V7 要的是"当前在板这一版上连续 10 次冷上电"，所以要另看下面这一行。
stall = sorted([ts_of(r[2]) for r in recs if r[2][2] == 0x0052])
wdg = [r for r in recs if r[2][2] == 0x0002]
last_stall = stall[-1] if stall else None
if last_stall is None:
    print('\n== 判活事件的"干净尾巴" ==  全区从未记过 EV_TASK_STALLED')
else:
    after = [b for b in boot if ts_of(b[2]) > last_stall]
    wafter = [w for w in wdg if ts_of(w[2]) > last_stall]
    print(f'\n== 判活事件的"干净尾巴"（V1/V7 的回顾性证据）==')
    print(f'  EV_TASK_STALLED 总条数 = {len(stall)}，最后一条 ts = {last_stall}')
    print(f'  其后仍有 EV_BOOT {len(after)} 次（code: ' +
          ', '.join('0x%04X' % b[2][3] for b in after[-10:]) + '）')
    print(f'  其后 EV_WDG_RESET = {len(wafter)} 条'
          + ('   ⇒ 最后这几尾是"记了 stall 但没有狗咬"还是"根本没再 stall"，看这一格' if wafter else ''))
    print('  ⚠️ ts 的可比范围 = **同一个供电周期内**：F1 的 RTC 备份域只被掉电(POR)清，'
          'nRST/软件/狗咬三种复位都不清 ⇒ 暖复位之间 ts 连续可比；'
          '⛔ 断过电之后 ts 回到预设值，跨断电比 ts 大小无意义。')
    print('  ⇒ 因此本行只说明"最后一次 stall 的 ts 之后又写了 N 条 BOOT"，'
          '⛔ 不构成"连续 N 次干净启动"，也不得记成 V1/V7 通过。')
    # 真正的时序 = Flash 里的物理顺序（追加式；页回收会绕回，故用页/槽序近似写序）
    print('  按**物理写入顺序**的最后 6 条：')
    for pg, slot, hw in recs[-6:]:
        print(f'    页{pg:>2} 槽{slot:>3}  ts={ts_of(hw):>10}  {NAME.get(hw[2], "EV_%04X" % hw[2]):<16}'
              f' code=0x{hw[3]:04X}')
PY
