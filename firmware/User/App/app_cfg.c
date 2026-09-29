/**
 ****************************************************************************************************
 * @file        app_cfg.c
 * @brief       门限参数 Flash 持久化的实现（页布局 / 上电三态 / A-B 交替写 / 运行期层次校验）
 * @note        规格 = `Tech_Design.md` §8「门限参数区协议」（`附录C` C-121）；契约 = `app_cfg.h`。
 *              本文件里 ⛔ 不得出现：通信报文解析（§4.2 的"禁止"列）、执行器 GPIO、AFE 寄存器名。
 ****************************************************************************************************
 */

#include "./App/app_cfg.h"
#include "./App/app_log.h"              /* ★ 只为一个用途：复用 CCITT-FALSE 的唯一实现 */
#include "./BSP/STMFLASH/stmflash.h"
#include <string.h>

/* ===================== 编译期：表规模与页容量必须自洽 ===================== */
/* 载荷 + 页头必须装得下一页；80 格（=65 参数 + 3 状态 + 12 预留）不得超过协议块 */
APP_STATIC_ASSERT(APP_CFG_PAYLOAD_HW + APP_CFG_HDR_HW <= (APP_FLASH_PAGE_SIZE / 2u), cfg_payload_over_page);
APP_STATIC_ASSERT(APP_CFG_COUNT <= 80u, cfg_table_over_modbus_block);
APP_STATIC_ASSERT(APP_CFG_MODBUS_RESERVED == APP_CFG_MODBUS_FIRST + 0x44u, cfg_reserved_addr_wrong);

/* ===================== 由 X-macro 生成三张只读表 ===================== */
/* ⛔ 默认值一律来自 `app_config.h` 的宏名（`app_cfg.h` 的 X 表），⛔ 本文件不抄任何数值 */
#define APP_CFG_X_DFLT(n, d, mn, mx, s)     (int32_t)(d),
static const int32_t s_dflt[APP_CFG_COUNT] = { APP_CFG_ITEMS(APP_CFG_X_DFLT) };
#undef APP_CFG_X_DFLT

#define APP_CFG_X_MIN(n, d, mn, mx, s)      (int32_t)(mn),
static const int32_t s_min[APP_CFG_COUNT] = { APP_CFG_ITEMS(APP_CFG_X_MIN) };
#undef APP_CFG_X_MIN

#define APP_CFG_X_MAX(n, d, mn, mx, s)      (int32_t)(mx),
static const int32_t s_max[APP_CFG_COUNT] = { APP_CFG_ITEMS(APP_CFG_X_MAX) };
#undef APP_CFG_X_MAX

/* ===================== 模块状态 ===================== */
static uint16_t s_val[APP_CFG_COUNT];       /* 当前生效值（RAM 副本） */
static uint16_t s_page[APP_FLASH_PAGE_SIZE / 2u];   /* ★ 整页镜像（2KB）：⛔ 不用"部分写"避开 stmflash 的整页 RMW */

static uint8_t  s_boot_state = APP_CFG_BOOT_FRESH;
static uint8_t  s_active     = 0u;          /* 0 = A 区, 1 = B 区 */
static uint8_t  s_bad_mask;                 /* bit0 = A 坏, bit1 = B 坏（"魔数对但内容不可信"） */
static uint8_t  s_have_valid;               /* 1 = 至少有一区通过校验 */
static uint32_t s_seq;                      /* 当前活动区的提交序号 */
static uint8_t  s_pending;                  /* 待提交 */
static uint8_t  s_selftest_rc = 0xFFu;      /* 取证出口：⛔ 不只喂 printf（本机没有 COM 通路） */
static uint8_t  s_dbg[5];                   /* ★ 上电装载取证：A 区类 / B 区类 / 分支 / 跨项 / 返回态 */
static uint16_t s_dbg_poll_calls;           /* ★ 提交路径取证：`app_cfg_poll()` 被调了几次 */
static uint8_t  s_dbg_poll_rc;              /* ★ 最近一次 poll 的返回码 */

/* ★★ 2026-09-29：**每次提交的取证环**（4 条 × 4 字节）—— 用来钉死"提交写到哪一区、用的是哪个序号"。
 *   为什么必须有：一次真机观察出现了"B 区 ver=2/seq=2"而按推演应为 ver=1 的组合 ⇒
 *   ⛔ 不允许靠推理收场（`AGENTS.md` §3.1 ② 存在性探针的口径：先看见事实）。 */
static uint8_t  s_clog[4][4];               /* [i] = {目标区, 提交前的 s_active, 新序号低 8 位, 结构版本} */
static uint8_t  s_clog_n;                   /* 累计提交次数（低 2 位 = 环下标） */

/* ===================== 页内布局的小工具 ===================== */
#define CFG_HDR_MAGIC   0u
#define CFG_HDR_VER     1u
#define CFG_HDR_SEQ_LO  2u
#define CFG_HDR_SEQ_HI  3u
#define CFG_HDR_CRC     4u
#define CFG_HDR_LEN     5u

static uint32_t cfg_page_addr(uint8_t which)
{
    return (which == 0u) ? APP_PARAM_A_ADDR : APP_PARAM_B_ADDR;
}

/**
 * @brief       ★ CRC 覆盖"除 CRC 半字本身之外的全部页头 + 载荷"
 * @note        ⚠️ **这一条就地修正了 §8 协议 (1) 的原措辞**（原写"覆盖 `+0x00C ..`"= 只盖载荷）。
 *              实现时发现：`魔数` / `结构版本` / `提交序号` / `载荷长度` **四个都是"判据字段"** ——
 *              序号决定"哪一区是新的"、版本触发迁移。只盖载荷的话，**序号里翻一个比特不会被发现**，
 *              后果是**静默选了旧的那套门限**（而且两区 CRC 都"通过"）。⇒ 改成覆盖全部判据字段。
 *              做法 = 先把 `s_page[CFG_HDR_CRC]` 置 0 再算（接收侧同样先置 0 再算），
 *              ⛔ 不把 CRC 字段本身卷进去（那是不可能的自指）。
 */
static uint16_t cfg_page_crc(uint16_t n_hdr_plus_payload)
{
    uint16_t saved = s_page[CFG_HDR_CRC];

    s_page[CFG_HDR_CRC] = 0x0000u;

    {
        /* 头 6 个半字 + 载荷 = 6 + APP_CFG_PAYLOAD_HW 个半字（连续的） */
        uint16_t crc = app_log_crc16_hw(s_page, n_hdr_plus_payload);
        s_page[CFG_HDR_CRC] = saved;
        return crc;
    }
}

/**
 * @brief       读一区的页头并分类
 * @retval      `APP_CFG_SLOT_*`（见下四个常量）
 * @note        ★ **四类必须分开**，⛔ 不能把"版本旧"和"CRC 坏"混成一类：
 *                空白（没写过）/ 可用 / **版本是别的**（⇒ 迁移，§8 协议 (6)）/ **真的坏了**（⇒ 记坏 + 修复）
 *              混起来的后果很具体：**固件升级后必然出现"版本不等"**，若把它当"坏"，
 *              两区都旧 ⇒ 走"拒绝启动"，**一次正常的升级会把设备锁死**。
 */
#define APP_CFG_SLOT_BLANK      0u
#define APP_CFG_SLOT_OK         1u
#define APP_CFG_SLOT_OLD_VER    2u
#define APP_CFG_SLOT_CORRUPT    3u

static uint8_t cfg_read_slot(uint8_t which, uint32_t *seq)
{
    uint16_t hdr[APP_CFG_HDR_HW];
    uint16_t len;
    uint16_t crc;
    uint32_t s;

    stmflash_read(cfg_page_addr(which), hdr, (uint16_t)APP_CFG_HDR_HW);

    if (hdr[CFG_HDR_MAGIC] != APP_CFG_MAGIC)
    {
        return APP_CFG_SLOT_BLANK;          /* 从未写过（空白页 0xFFFF）⇒ **不是"坏"** */
    }

    /* 魔数对 ⇒ 这一区被写过。下面任何一条不过都是**真的损坏** */
    len = hdr[CFG_HDR_LEN];

    if (len != APP_CFG_PAYLOAD_HW)
    {
        return APP_CFG_SLOT_CORRUPT;
    }

    stmflash_read(cfg_page_addr(which), s_page, (uint16_t)(APP_CFG_HDR_HW + len));
    crc = cfg_page_crc((uint16_t)(APP_CFG_HDR_HW + len));

    if (crc != hdr[CFG_HDR_CRC])
    {
        return APP_CFG_SLOT_CORRUPT;        /* ★ 判据字段受 CRC 保护 ⇒ 这一条才配叫"内容可信" */
    }

    s = (uint32_t)hdr[CFG_HDR_SEQ_LO] | ((uint32_t)hdr[CFG_HDR_SEQ_HI] << 16);

    if (seq != NULL)
    {
        *seq = s;
    }

    return (hdr[CFG_HDR_VER] == APP_CFG_VER) ? APP_CFG_SLOT_OK : APP_CFG_SLOT_OLD_VER;
}

/* ===================== 运行期层次校验（★ 逐条镜像 §12.7.6 的编译期断言） ===================== */
/* ★ 口径：**两套规则必须逐条对应**（§8 协议 (4)）。多写一条 = 拒绝一个编译期接受的配置；
 *   少写一条 = S7 在运行期漏一格。下表每一行的注释都写着它镜像的那个断言 name。 */
static uint8_t cfg_cross_ok(const uint16_t *v)
{
/* ★★ 2026-09-29 修：**必须做符号扩展** `(int16_t)` —— 温度门限是负数，
 *  `v[]` 是 uint16_t（Flash 线格式）⇒ 少了 `(int16_t)` 这一步，`-50` 会变成 `65486`，
 *  于是 `T_CHG_UT_L1 > L2 > L3` 即 `0 > 65486` 判假 ⇒ **全新设备被误判"参数区不可信"而锁定**。
 *  ⚠️ 现场证据：`s_active=1`（走 FRESH 分支）却 `s_boot_state=2`（LATCHED），
 *     且"首次提交"因早返回从未入队 ⇒ 两页空白 —— **一个根因解释两个症状**。
 *  ⛔ 教训：拿"有符号宏值"做的离线镜像**不能**当作本函数一致性的证据（量纲/类型不同）。 */
#define V(i)  ((int32_t)(int16_t)v[APP_CFG_IDX_##i])

    /* `APP_BMS_ASSERT_ASC/DESC` 一族：级联的单调性 */
    if (!(V(CELL_OV_L1_MV)   < V(CELL_OV_L2_MV)   && V(CELL_OV_L2_MV)   < V(CELL_OV_L3_MV)))     { return 0u; }   /* ord_ov12/23 */
    if (!(V(CELL_UV_L1_MV)   > V(CELL_UV_L2_MV)   && V(CELL_UV_L2_MV)   > V(CELL_UV_L3_MV)))     { return 0u; }   /* ord_uv12/23 */
    if (!(V(T_CHG_OT_L1_X10) < V(T_CHG_OT_L2_X10) && V(T_CHG_OT_L2_X10) < V(T_CHG_OT_L3_X10)))   { return 0u; }   /* ord_cot12/23 */
    if (!(V(T_CHG_UT_L1_X10) > V(T_CHG_UT_L2_X10) && V(T_CHG_UT_L2_X10) > V(T_CHG_UT_L3_X10)))   { return 0u; }   /* ord_cut12/23 */
    if (!(V(T_DIS_OT_L1_X10) < V(T_DIS_OT_L2_X10) && V(T_DIS_OT_L2_X10) < V(T_DIS_OT_L3_X10)))   { return 0u; }   /* ord_dot12/23 */
    if (!(V(I_CHG_L1_10MA)   < V(I_CHG_L2_10MA)   && V(I_CHG_L2_10MA)   < V(I_CHG_L3_10MA)))     { return 0u; }   /* ord_ic12/23 */
    if (!(V(I_DIS_L1_10MA)   < V(I_DIS_L2_10MA)   && V(I_DIS_L2_10MA)   < V(I_DIS_L3_10MA)))     { return 0u; }   /* ord_id12/23 */

    /* ★★ S7 的层次关系：**软件三级必须比硬件层更保守**（这四条是本表的灵魂） */
    if (!(V(CELL_OV_L3_MV)   < V(CELL_OV_HW_MV)))     { return 0u; }   /* s7_ov  */
    if (!(V(CELL_UV_L3_MV)   > V(CELL_UV_HW_MV)))     { return 0u; }   /* s7_uv  */
    if (!(V(I_CHG_L3_10MA)   < V(I_OCC_HW_10MA)))     { return 0u; }   /* s7_occ */
    if (!(V(I_DIS_L3_10MA)   < V(I_OCD_HW_10MA)))     { return 0u; }   /* s7_ocd */

    /* `APP_BMS_ASSERT_HYST(hy, gap)`：2×迟滞 < 相邻级差（迟滞不得吃掉整个级差） */
    if (!(V(CELL_HYST_MV)     * 2 < (V(CELL_OV_L2_MV) - V(CELL_OV_L1_MV))))                        { return 0u; }   /* hyst_cell_ov */
    if (!(V(CELL_HYST_MV)     * 2 < (V(CELL_UV_L1_MV) - V(CELL_UV_L2_MV))))                        { return 0u; }   /* hyst_cell_uv */
    if (!(V(T_HYST_X10)       * 2 < (V(T_CHG_OT_L2_X10) - V(T_CHG_OT_L1_X10))))                    { return 0u; }   /* hyst_t_chg  */
    if (!(V(T_HYST_X10)       * 2 < (V(T_DIS_UT_L1_X10) - V(T_DIS_UT_L2_X10))))                    { return 0u; }   /* hyst_t_dis  */
    if (!(V(T_DIF_HYST_X10)   * 2 < (V(T_DIF_L2_X10) - V(T_DIF_L1_X10))))                          { return 0u; }   /* hyst_t_dif  */
    if (!(V(I_HYST_CHG_10MA)  * 2 < (V(I_CHG_L2_10MA) - V(I_CHG_L1_10MA))))                        { return 0u; }   /* hyst_i_chg  */
    if (!(V(I_HYST_DIS_10MA)  * 2 < (V(I_DIS_L2_10MA) - V(I_DIS_L1_10MA))))                        { return 0u; }   /* hyst_i_dis  */

    /* 其余 §12.7.6 的跨项断言（逐条对应，名字写在右边） */
    if (!((int32_t)V(I_HOLD_L3_MS)  < (int32_t)V(HW_OCC_OCD_MS)))                    { return 0u; }   /* s7_time           */
    if (!((int32_t)V(I_DIS_L3_10MA) <= (int32_t)(APP_DESIGN_CURRENT_MA / 10)))       { return 0u; }   /* l3_over_design    */
    if (!((int32_t)V(I_SCD_HW_10MA) > (int32_t)V(I_OCD_HW_10MA)))                    { return 0u; }   /* scd_under_ocd     */
    if (!((int32_t)V(CELL_OV_HW_MV) < (int32_t)APP_BMS_CELL_MV_VALID_MAX))           { return 0u; }   /* valid_ov          */
    if (!((int32_t)V(CELL_UV_HW_MV) > (int32_t)APP_BMS_CELL_MV_VALID_MIN))           { return 0u; }   /* valid_uv          */
    if (!((int32_t)V(T_NOCHG_X10)   >= (int32_t)V(T_CHG_UT_L3_X10)))                 { return 0u; }   /* nochg_in_range    */
    if (!((int32_t)V(T_NOCHG_REC_X10) > (int32_t)V(T_NOCHG_X10)))                    { return 0u; }   /* nochg_recover_dir */
    if (!((uint32_t)V(DEBOUNCE_L3_MS) + APP_PERIOD_PROTECT_MS <= 1000u))             { return 0u; }   /* l3_budget         */
    if (!((int32_t)V(PRECHARGE_MS)  >= (int32_t)(10u * APP_PERIOD_PROTECT_MS)))      { return 0u; }   /* precharge_window  */
    if (!((int32_t)V(MIN_INTERVAL_MS) >= (int32_t)V(PRECHARGE_MS)))                  { return 0u; }   /* cooldown_short    */
    if (!((int32_t)V(MIN_INTERVAL_MS) >= 3000))                                      { return 0u; }   /* min_interval_s10  */
    if (!((uint32_t)V(PRECHARGE_DV_MV) % 10u == 0u))                                 { return 0u; }   /* dv_not_divisible  */
    if (!((uint32_t)V(PRECHARGE_DV_MV) / 10u < (uint32_t)APP_MV_INVALID))            { return 0u; }   /* dv_collides_inv   */
    if (!((int32_t)V(DATA_STALE_MS) > 0))                                            { return 0u; }   /* 新鲜度必须为正     */
    if (!((int32_t)V(TR_HOLD_L3_N)  >= 1))                                           { return 0u; }   /* 持续周期数 ≥1      */

#undef V
    return 1u;
}

/** 单项范围检查（用 int32 比较：温度那几项是**负数**，⛔ 不能用 u16 比） */
static uint8_t cfg_range_ok(uint16_t idx, uint16_t val)
{
    int32_t x = (int32_t)(int16_t)val;      /* ★ 门限按 **int16** 语义解读（温度可为负） */

    if (idx >= (uint16_t)APP_CFG_COUNT)
    {
        return 0u;
    }

    return ((x >= s_min[idx]) && (x <= s_max[idx])) ? 1u : 0u;
}

/* ===================== 对外的六个动作 ===================== */

uint8_t app_cfg_load(void)
{
    uint32_t seq_a = 0u, seq_b = 0u;
    uint8_t  st_a, st_b;
    uint16_t i;

    s_bad_mask    = 0u;
    s_pending     = 0u;
    s_have_valid  = 0u;
    s_active      = 0u;
    s_seq         = 0u;

    st_a = cfg_read_slot(0u, &seq_a);
    st_b = cfg_read_slot(1u, &seq_b);
    s_dbg[0] = st_a;
    s_dbg[1] = st_b;

    /* 坏 = "魔数对但读不出可信内容"（blank 不算坏）*/
    if (st_a == APP_CFG_SLOT_CORRUPT) { s_bad_mask |= 0x01u; }
    if (st_b == APP_CFG_SLOT_CORRUPT) { s_bad_mask |= 0x02u; }

    {
        uint8_t ok_a = (uint8_t)(st_a == APP_CFG_SLOT_OK);
        uint8_t ok_b = (uint8_t)(st_b == APP_CFG_SLOT_OK);
        uint8_t old_any = (uint8_t)((st_a == APP_CFG_SLOT_OLD_VER) || (st_b == APP_CFG_SLOT_OLD_VER));
        uint8_t blank_all = (uint8_t)((st_a == APP_CFG_SLOT_BLANK) && (st_b == APP_CFG_SLOT_BLANK));

        if (blank_all != 0u)
        {
            /* ---- 态①：两区都空白 = 首次上电 ⇒ 用编译期默认值，并立刻固化进 A 区 ---- */
            s_boot_state = APP_CFG_BOOT_FRESH;
            s_active     = 1u;      /* ★ 修正：让**首次提交写 A 区**（target = s_active^1）。
                                     *   原写 0 会让首份参数落在 B 区，与 §8 协议 (2)①"写进 A 区"矛盾 —— */
            s_seq        = 0u;
            s_dbg[2]     = 1u;      /* 分支 1 = FRESH */
        }
        else if ((ok_a != 0u) || (ok_b != 0u))
        {
            /* ---- 态②：至少一区好 ⇒ 取**提交序号更大**的那区 ---- */
            uint8_t  use     = 0u;
            uint32_t use_seq = seq_a;

            if ((ok_a != 0u) && (ok_b != 0u))
            {
                use     = (seq_b > seq_a) ? 1u : 0u;    /* ⚠️ 相等时取 A：序号相等只可能是"从没提交过" */
                use_seq = (use == 0u) ? seq_a : seq_b;
            }
            else
            {
                use     = (ok_a != 0u) ? 0u : 1u;
                use_seq = (use == 0u) ? seq_a : seq_b;
            }

            stmflash_read(cfg_page_addr(use) + (APP_CFG_HDR_HW * 2u), s_val,
                          (uint16_t)APP_CFG_PAYLOAD_HW);

            s_active     = use;
            s_seq        = use_seq;
            s_have_valid = 1u;
            s_boot_state = APP_CFG_BOOT_LOADED;
            s_dbg[2]     = 2u;      /* 分支 2 = LOADED */

            /* ★ 迁移（§8 协议 (6)）：有旧版本的区、或另一区坏了 ⇒ 置一次待提交把它治好。
             * ⚠️ 本版只有结构版本 1 ⇒ **没有更旧的布局可解析**，迁移的退化形态 = 用当前默认值重写。
             *    下一版必须在这里补真正的"旧布局 → 新布局"，⛔ 不是把这段留着当占位。 */
            if ((s_bad_mask != 0u) || (old_any != 0u))
            {
                s_pending = 1u;
            }
        }
        else if (old_any != 0u)
        {
            /* ---- ★★ 迁移分支（§8 协议 (6)）：**没有"当前版本"可用的区，但有"旧版本"可读** ⇒ 迁移，⛔ 不锁定。
             * ⚠️ 2026-09-29 补：原实现漏了这一支 ⇒ 固件升级后（两区都成了旧版本）会掉进态③**把设备锁死**。
             *    这正是 `OLD_VER` 与 `CORRUPT` 必须分开的全部意义：**"旧"是可读的，"坏"才是不可信的**。
             * ⚠️ 本版只有结构版本 1 ⇒ **不存在更旧的布局可解析** ⇒ 迁移的退化形态 = **取当前默认值**并重写。
             *    ⛔ 故意**不**做"按下标原样搬运"：那样一旦将来版本改了项序，会把 A 参数的值静默装进 B 参数
             *    （比"退回默认值"危险得多）。真要做迁移必须**写一张显式映射表**（下一版的题目）。
             * ⇒ 外部表现：`s_boot_state = FRESH`（= 用默认值跑 + 排一次提交），但 `s_dbg[2] = 5` 记录"这是迁移不是首装"。 */
            s_boot_state = APP_CFG_BOOT_FRESH;
            s_dbg[2]     = 5u;      /* 分支 5 = 迁移（区别于 1 = 真空板首装） */
        }
        else
        {
            /* ---- 态③：**被写过**但两区都不可信 ⇒ 拒绝启动（§5.2 / §8 协议 (2)）---- */
            s_boot_state = APP_CFG_BOOT_LATCHED;
            s_dbg[2]     = 3u;      /* 分支 3 = 三态③（被写过但两区都不可信） */
        }
    }

    /* 非 LOADED 的两支（FRESH / LATCHED）都要把 RAM 副本填成默认值：
     * ⛔ "读得到"不代表"可以继续跑" —— 是否继续由调用方按返回的态决定 */
    if (s_boot_state != APP_CFG_BOOT_LOADED)
    {
        for (i = 0u; i < (uint16_t)APP_CFG_COUNT; i++)
        {
            s_val[i] = (uint16_t)s_dflt[i];
        }
    }

    if (s_boot_state == APP_CFG_BOOT_LATCHED)
    {
        return s_boot_state;
    }

    /* ★ 装载后必须过一次运行期校验：参数区的值也必须是自洽的
     *   （S7 不因为"这个值来自 Flash"而豁免；与 §12.7.6 的编译期断言是同一条口径） */
    s_dbg[3] = (uint8_t)((cfg_cross_ok(s_val) != 0u) ? 1u : 2u);    /* 1 = 过, 2 = 不过 */

    if (s_dbg[3] != 1u)
    {
        s_boot_state = APP_CFG_BOOT_LATCHED;    /* 与"两区都坏"同等级：拿一套不自洽的门限跑 = 不可知即不安全 */
        s_dbg[2]     = 4u;                      /* 分支 4 = 跨项校验不过 */
        s_dbg[4]     = s_boot_state;
        return s_boot_state;
    }

    if (s_boot_state == APP_CFG_BOOT_FRESH)
    {
        s_pending = 1u;                         /* 首次上电：把默认值固化（序号 1） */
    }

    s_dbg[4] = s_boot_state;
    return s_boot_state;
}

uint16_t app_cfg_get(uint16_t idx)
{
    if (idx >= (uint16_t)APP_CFG_COUNT)
    {
        return 0u;
    }

    return s_val[idx];
}

uint8_t app_cfg_set(uint16_t idx, uint16_t val)
{
    uint16_t tmp[APP_CFG_COUNT];

    if (idx >= (uint16_t)APP_CFG_COUNT)
    {
        return APP_CFG_ERR_PARAM;
    }

    if (cfg_range_ok(idx, val) == 0u)
    {
        return APP_CFG_ERR_RANGE;
    }

    /* ★ 跨项校验必须对**候选值**做（不是对旧值）：先把候选值放进一份临时副本再跑全部不等式 */
    memcpy(tmp, s_val, sizeof(tmp));
    tmp[idx] = val;

    if (cfg_cross_ok(tmp) == 0u)
    {
        return APP_CFG_ERR_CROSS;
    }

    s_val[idx] = val;

    return APP_CFG_OK;
}

uint8_t app_cfg_commit_request(void)
{
    if (s_pending != 0u)
    {
        return APP_CFG_ERR_BUSY;            /* ⛔ 任何时刻只允许一次提交在队里（§8 协议 (3) 第 6 条） */
    }

    s_pending = 1u;

    return APP_CFG_OK;
}

uint8_t app_cfg_restore_defaults(void)
{
    uint16_t i;

    for (i = 0u; i < (uint16_t)APP_CFG_COUNT; i++)
    {
        s_val[i] = (uint16_t)s_dflt[i];
    }

    if (s_pending != 0u)
    {
        return APP_CFG_ERR_BUSY;
    }

    s_pending = 1u;

    return APP_CFG_OK;
}

uint8_t app_cfg_poll(uint8_t in_safe_off)
{
    uint8_t  target;
    uint32_t new_seq;
    uint16_t hdr[APP_CFG_HDR_HW];
    uint16_t rd[APP_CFG_HDR_HW];

    s_dbg_poll_calls++;

    if (s_pending == 0u)
    {
        s_dbg_poll_rc = APP_CFG_OK;
        return APP_CFG_OK;                  /* 无事可做 */
    }

    if (in_safe_off == 0u)
    {
        s_dbg_poll_rc = APP_CFG_ERR_NOT_SAFE;
        return APP_CFG_ERR_NOT_SAFE;        /* ★ 排队等着，⛔ 不是失败、⛔ 不改任何状态 */
    }

    target  = (uint8_t)(s_active ^ 1u);
    new_seq = s_seq + 1u;

    /* ---- 1) 组装整页镜像：除载荷外全 0xFF（"干净的页"——⛔ 不沿用旧区残留） ---- */
    memset(s_page, 0xFF, sizeof(s_page));

    hdr[CFG_HDR_MAGIC]  = APP_CFG_MAGIC;
    hdr[CFG_HDR_VER]    = APP_CFG_VER;
    hdr[CFG_HDR_SEQ_LO] = (uint16_t)(new_seq & 0xFFFFu);
    hdr[CFG_HDR_SEQ_HI] = (uint16_t)(new_seq >> 16);
    hdr[CFG_HDR_CRC]    = 0x0000u;          /* 先占位，最后回填 */
    hdr[CFG_HDR_LEN]    = APP_CFG_PAYLOAD_HW;
    memcpy(s_page, hdr, sizeof(hdr));
    memcpy(&s_page[APP_CFG_HDR_HW], s_val, (size_t)(APP_CFG_PAYLOAD_HW * 2u));

    s_page[CFG_HDR_CRC] = cfg_page_crc((uint16_t)(APP_CFG_HDR_HW + APP_CFG_PAYLOAD_HW));

    /* ---- 2) 整页写（`stmflash_write()` = 整页擦 + 整页写回）---- */
    stmflash_write(cfg_page_addr(target), s_page, (uint16_t)(APP_FLASH_PAGE_SIZE / 2u));

    /* ---- 3) ★ 回读校验：⛔ **不信任"写成功"**，只信任"读回来是对的" ---- */
    stmflash_read(cfg_page_addr(target), rd, (uint16_t)APP_CFG_HDR_HW);

    if ((rd[CFG_HDR_MAGIC] != APP_CFG_MAGIC) || (rd[CFG_HDR_LEN] != APP_CFG_PAYLOAD_HW) ||
        (rd[CFG_HDR_VER]   != APP_CFG_VER)   ||
        (rd[CFG_HDR_SEQ_LO] != (uint16_t)(new_seq & 0xFFFFu)) ||
        (rd[CFG_HDR_SEQ_HI] != (uint16_t)(new_seq >> 16)))
    {
        s_pending = 0u;                     /* 目标区没写成功 ⇒ 原活动区**原封不动**（现场不变） */
        return APP_CFG_ERR_FLASH;
    }

    {
        uint16_t crc;

        stmflash_read(cfg_page_addr(target), s_page, (uint16_t)(APP_CFG_HDR_HW + APP_CFG_PAYLOAD_HW));
        crc = cfg_page_crc((uint16_t)(APP_CFG_HDR_HW + APP_CFG_PAYLOAD_HW));

        if (crc != rd[CFG_HDR_CRC])
        {
            s_pending = 0u;
            return APP_CFG_ERR_FLASH;
        }
    }

    /* ---- 4) 通过 ⇒ 切换活动区（★ 只改元数据；载荷本来就是 `s_val`，⛔ 不需要再"写回" ---- */
    {
        uint8_t i = (uint8_t)(s_clog_n & 0x03u);

        s_clog[i][0] = target;              /* 写到哪一区 */
        s_clog[i][1] = s_active;            /* 提交前谁是活动区 */
        s_clog[i][2] = (uint8_t)(new_seq & 0xFFu);
        s_clog[i][3] = (uint8_t)APP_CFG_VER;
        s_clog_n++;
    }

    s_active     = target;
    s_seq        = new_seq;
    s_have_valid = 1u;
    s_bad_mask   = (uint8_t)(s_bad_mask & (uint8_t)~(1u << target));
    s_pending    = 0u;

    return APP_CFG_OK;
}

void app_cfg_status(uint8_t *boot_state, uint32_t *commit_seq, uint8_t *bad_mask)
{
    if (boot_state != NULL) { *boot_state = s_boot_state; }
    if (commit_seq != NULL) { *commit_seq = s_seq; }
    if (bad_mask   != NULL) { *bad_mask   = s_bad_mask; }
}

uint8_t app_cfg_selftest(void)
{
    /* ① CRC 已知答案（两条形态）：标准检查值 + **半字口径**的检查值
     *    ★ 后者才是本工程的直接形态（`app_log_crc16_hw()` 吃半字），前者是跨实现的对照。 */
    static const uint8_t kat[9] = { '1', '2', '3', '4', '5', '6', '7', '8', '9' };
    uint16_t hw[6];
    uint16_t i;
    uint16_t c;

    for (i = 0u; i < 6u; i++) { hw[i] = 0u; }

    /* 逐字节算一遍 CCITT-FALSE（**离线对照**用；⛔ 不是第二份实现：下面那份走的是半字口径接口） */
    c = 0xFFFFu;

    for (i = 0u; i < 9u; i++)
    {
        uint8_t b = kat[i];
        uint8_t k;

        c ^= (uint16_t)((uint16_t)b << 8);

        for (k = 0u; k < 8u; k++)
        {
            c = (uint16_t)(((c & 0x8000u) != 0u) ? (uint16_t)((uint16_t)(c << 1) ^ 0x1021u)
                                                 : (uint16_t)(c << 1));
        }
    }

    if (c != 0x29B1u) { s_selftest_rc = 1u; return 1u; }

    if (app_log_crc16_hw(hw, 6u) != 0x84F9u) { s_selftest_rc = 1u; return 1u; }

    /* ② ★ 默认值必须**自己**过得去全部跨项不等式（= 与 §12.7.6 的编译期断言互为对照） */
    {
        uint16_t d[APP_CFG_COUNT];

        for (i = 0u; i < (uint16_t)APP_CFG_COUNT; i++) { d[i] = (uint16_t)s_dflt[i]; }

        if (cfg_cross_ok(d) == 0u) { s_selftest_rc = 2u; return 2u; }
    }

    s_selftest_rc = 0u;
    return 0u;
}

uint8_t app_cfg_commit_pending(void)
{
    return (s_pending != 0u) ? 1u : 0u;
}

uint8_t app_cfg_selftest_result(void)
{
    return s_selftest_rc;
}

void app_cfg_commit_trace(uint8_t *out16, uint8_t *n)
{
    uint8_t i;

    if (n != NULL) { *n = s_clog_n; }

    if (out16 != NULL)
    {
        for (i = 0u; i < 16u; i++) { out16[i] = s_clog[i >> 2][i & 3u]; }
    }
}

void app_cfg_diag(uint8_t *d)
{
    if (d != NULL)
    {
        d[0] = s_dbg[0];
        d[1] = s_dbg[1];
        d[2] = s_dbg[2];
        d[3] = s_dbg[3];
        d[4] = s_dbg[4];
    }
}
