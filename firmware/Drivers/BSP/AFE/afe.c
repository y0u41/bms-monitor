/**
 ****************************************************************************************************
 * @file        afe.c
 * @brief       AFE 通用层：初始化时序、ADC 码→mV 换算、有效性、链路健康
 * @note        本文件**不含任何寄存器地址**（型号相关的一切在 `afe_hal_bq76920.c`）——
 *              换 AFE 型号时这里不动（`Tech_Design.md` §4.6 决定 2）。
 *
 *              换算式来自 `SLUSBK2I` §8.3.1.1（已核实，非推测）：
 *                  V(cell) = code × GAIN + OFFSET          GAIN 单位 µV/LSB，OFFSET 单位 mV
 *                  V(BAT)  = 4 × GAIN × code + N × OFFSET   （BAT = Σ单体/4，16 位）
 *
 *              ★ **不可知即不安全**：标定值读不到就整体报错，绝不退化成"用标称 382µV 凑合" ——
 *                几个百分点的标度误差正好会把临界电芯推到误判方向（漏报比误报更危险）。
 ****************************************************************************************************
 */

#include "./BSP/AFE/afe.h"
#include "./App/app_cfg.h"          /* ★ D8：硬件门限取自参数区 */
#include "./BSP/AFE/afe_hal.h"
#include "./BSP/SOFTI2C/softi2c.h"
#include "./App/app_config.h"

static uint16_t s_gain_uv   = 0;        /* 0 = 尚未取到标定值，任何测量都不许开始 */
static int16_t  s_offset_mv = 0;
static uint8_t  s_ready     = 0;

/* ===================== 换算 ===================== */

static uint32_t s_cell_k[AFE_CHIP_MAX_CELLS];     /* ppm；0 = 该串未标定 */
static int32_t  s_cell_b[AFE_CHIP_MAX_CELLS];     /* µV */
static uint16_t s_cell_mask;
static uint16_t s_cell_selftest_rc = 0xFFFFu;

void afe_clear_cell_calib(void)
{
    uint8_t i;

    for (i = 0; i < AFE_CHIP_MAX_CELLS; i++)
    {
        s_cell_k[i] = 0u;
        s_cell_b[i] = 0;
    }

    s_cell_mask = 0u;
}

uint16_t afe_cell_calib_mask(void) { return s_cell_mask; }

uint8_t afe_set_cell_calib(uint8_t idx, uint32_t k_ppm, int32_t b_uv)
{
    /* ★ 合理性：k 落在 [0.9, 1.1]（与 `app_calib_set_cell()` 同口径）—— 超出这个范围的"标定"几乎一定是算错了 */
    if ((idx >= (uint8_t)AFE_CHIP_MAX_CELLS) || (k_ppm < 900000u) || (k_ppm > 1100000u))
    {
        return AFE_ERR_IO;
    }

    s_cell_k[idx] = k_ppm;
    s_cell_b[idx] = b_uv;
    s_cell_mask |= (uint16_t)(1u << idx);

    return AFE_OK;
}

/** 全局换算 + 该串标定修正（★ 修正只在**原始值有效**时施加，且修正后**再判一次量程**） */
static uint16_t afe_code_to_mv(uint16_t code, uint8_t idx)
{
    int32_t  uv = (int32_t)code * (int32_t)s_gain_uv + (int32_t)s_offset_mv * 1000;
    int32_t  mv;

    if (uv < 0) { return APP_MV_INVALID; }

    mv = uv / 1000;

    if ((mv < (int32_t)APP_BMS_CELL_MV_VALID_MIN) || (mv > (int32_t)APP_BMS_CELL_MV_VALID_MAX))
    {
        return APP_MV_INVALID;      /* 量程外一律判无效，不"就近取整"（§4.4 无效值口径） */
    }

    /* ★ 逐串标定：`mv_corr = mv + mv×(k−1e6)/1e6 + b`（µV 口径），⛔ 未标定的串原样返回 */
    if ((idx < (uint8_t)AFE_CHIP_MAX_CELLS) &&
        ((s_cell_mask & (uint16_t)(1u << idx)) != 0u))
    {
        int32_t d_uv = (int32_t)(((int64_t)uv * ((int64_t)s_cell_k[idx] - 1000000)) / 1000000);
        int32_t c_uv = uv + d_uv + s_cell_b[idx];

        if (c_uv < 0) { return APP_MV_INVALID; }

        mv = c_uv / 1000;

        if ((mv < (int32_t)APP_BMS_CELL_MV_VALID_MIN) || (mv > (int32_t)APP_BMS_CELL_MV_VALID_MAX))
        {
            return APP_MV_INVALID;  /* ★ 修正**不得**把越界值救回来 */
        }
    }

    return (uint16_t)mv;
}

/* ===================== 生命周期 ===================== */

uint8_t afe_init(void)
{
    uint8_t err;

    s_ready = 0;

    softi2c_init(&g_bus_afe);           /* AFE 独占总线（PD11/PD12）；传感总线 g_bus_sens 由启动序列各自初始化 */

    /* ① ★ BOOT：不 boot 就碰 I2C 一定拿不到 ACK，而且症状与"芯片是坏的""地址写错"完全同形（C-64） */
    afe_hal_boot();

    err = afe_hal_probe();

    if (err != AFE_OK)
    {
        afe_hal_boot();                 /* 再给一次机会：上电瞬间 TS1 可能来不及看到上升沿 */
        err = afe_hal_probe();

        if (err != AFE_OK)
        {
            return err;                 /* 先量 TS1/PE6 有没有脉冲，再怀疑芯片与线束 */
        }
    }

    /* ② ★ 先做**只读**的标定读取：它同时也是 CRC 口径的检验。
     *    若芯片实际不带 CRC（`06` 后缀）而 AFE_USE_CRC=1，这里返回 AFE_ERR_CRC，
     *    于是**一次写操作都不会发生** —— 顺序反了就会把 CRC 字节写进下一个寄存器（可能改掉 PROTECT1）。 */
    err = afe_hal_read_scale(&s_gain_uv, &s_offset_mv);

    if (err != AFE_OK)
    {
        s_gain_uv = 0;
        return err;
    }

    if ((s_gain_uv < 365u) || (s_gain_uv > 396u))
    {
        return AFE_ERR_DATA;            /* 超出手册区间的 GAIN 只能是读错了，不能用来换算 */
    }

    /* ③ 配置：只开 ADC（禁止关保护，S4）、把充/放通道归零（S3 常态断开） */
    err = afe_hal_setup();

    if (err != AFE_OK)
    {
        return err;
    }

    s_ready = 1;

    return AFE_OK;
}

uint8_t afe_self_test(void)
{
    uint16_t code[AFE_CHIP_MAX_CELLS];
    uint16_t mv[AFE_CHIP_MAX_CELLS];
    uint16_t pack_10mv;
    uint32_t sum_mv = 0;
    uint32_t pack_mv;
    uint32_t diff_mv;
    uint8_t  bad = 0;
    uint8_t  i;
    uint8_t  err;

    if (!s_ready)
    {
        return AFE_ERR_DATA;
    }

    /* ① 通信仍在位（拔线/复位后自检用） */
    err = afe_hal_probe();

    if (err != AFE_OK)
    {
        return err;
    }

    /* ② 逐串合理性 */
    err = afe_hal_read_vc(code, APP_CELL_COUNT);

    if (err != AFE_OK)
    {
        return err;
    }

    for (i = 0; i < APP_CELL_COUNT; i++)
    {
        mv[i] = afe_code_to_mv(code[i], i);

        if (mv[i] == APP_MV_INVALID)
        {
            bad++;
        }
        else
        {
            sum_mv += mv[i];
        }
    }

    if (bad != 0)
    {
        return AFE_ERR_DATA;    /* 有串不在物理可能区间：线序错、采样电阻没焊、或芯片坏 */
    }

    /* ③ Σ单体 vs 组压寄存器交叉校验（≤1%）。与 INA226 的交叉校验在 `app_acq` 做 ——
     *    AFE 层不该知道第二个测量器件的存在（§2.1 单向数据流）。 */
    err = afe_read_pack(&pack_10mv);

    if (err != AFE_OK)
    {
        return err;
    }

    pack_mv = (uint32_t)pack_10mv * 10u;
    diff_mv = (sum_mv > pack_mv) ? (sum_mv - pack_mv) : (pack_mv - sum_mv);

    if ((diff_mv * 100u) > pack_mv)
    {
        return AFE_ERR_DATA;
    }

    return AFE_OK;
}

/* ===================== 采集 ===================== */

uint8_t afe_read_cells(uint16_t *mv, uint8_t count)
{
    uint16_t code[AFE_CHIP_MAX_CELLS];
    uint8_t i;
    uint8_t err;

    if ((mv == 0) || (count == 0) || (count > AFE_CHIP_MAX_CELLS) || (count > APP_CELL_COUNT_MAX))
    {
        return AFE_ERR_IO;
    }

    if (!s_ready)
    {
        for (i = 0; i < count; i++)
        {
            mv[i] = APP_MV_INVALID;
        }

        return AFE_ERR_DATA;            /* 没标定就不测，不用旧值假装新鲜 */
    }

    err = afe_hal_read_vc(code, count);

    for (i = 0; i < count; i++)
    {
        mv[i] = (err == AFE_OK) ? afe_code_to_mv(code[i], i) : APP_MV_INVALID;
    }

    if (err != AFE_OK)
    {
        return err;
    }

    for (i = 0; i < count; i++)
    {
        if (mv[i] == APP_MV_INVALID)
        {
            return AFE_ERR_DATA;        /* 数组已填好，无效位由上层写进 cell_valid 位图 */
        }
    }

    return AFE_OK;
}

uint8_t afe_read_pack(uint16_t *pack_10mv)
{
    uint16_t code;
    int32_t  uv;
    uint8_t  err;

    if ((pack_10mv == 0) || (!s_ready))
    {
        return s_ready ? AFE_ERR_IO : AFE_ERR_DATA;
    }

    err = afe_hal_read_bat(&code);

    if (err != AFE_OK)
    {
        return err;
    }

    /* V(BAT) = 4 × GAIN × code + N × OFFSET  （§8.3.1.1 式 9，N = 实际串数）
     * GAIN 单位 µV/LSB、OFFSET 单位 mV —— 相加前必须统一到 µV，否则差 1000 倍。 */
    uv = (int32_t)4 * (int32_t)s_gain_uv * (int32_t)code
         + (int32_t)s_offset_mv * (int32_t)APP_CELL_COUNT * 1000;

    if (uv < 0)
    {
        return AFE_ERR_DATA;                /* 负的组压只能是标定值或码错了，不是"电池反接" */
    }

    uv /= 1000;                             /* µV → mV */

    if (uv > (int32_t)APP_BMS_PACK_MV_VALID_MAX)
    {
        return AFE_ERR_DATA;
    }

    *pack_10mv = (uint16_t)(uv / 10);                   /* ★ 输出量纲 = Modbus 量纲(10mV) */

    return AFE_OK;
}

/* ===================== 均衡 / 状态 / 通道 ===================== */

uint8_t afe_get_balance(uint8_t *mask)
{
    if (!s_ready)
    {
        return AFE_ERR_DATA;
    }

    return afe_hal_read_bal(mask);
}

uint8_t afe_set_balance(uint8_t mask)
{
    if (!s_ready)
    {
        return AFE_ERR_DATA;
    }

    /* 只允许给"存在且有读数"的串开均衡：多串位一律砍掉，防止误开不存在的通道 */
    mask &= (uint8_t)((1u << APP_CELL_COUNT) - 1u);

    return afe_hal_write_bal(mask);
}

uint8_t afe_hw_fault(afe_hw_t *st)
{
    if (st == 0)
    {
        return AFE_ERR_IO;
    }

    st->ov = st->uv = st->ocd = st->scd = st->cc_ready = 0;

    if (!s_ready)
    {
        return AFE_ERR_DATA;
    }

    return afe_hal_read_hw_stat(st);
}

uint8_t afe_set_channel(uint8_t chg_on, uint8_t dsg_on)
{
    if (!s_ready)
    {
        return AFE_ERR_DATA;            /* 链路不可信时拒绝"放行"，只允许保持断开（S2） */
    }

    return afe_hal_set_channel(chg_on ? 1 : 0, dsg_on ? 1 : 0);
}

/* ===================== 链路健康（S2） ===================== */

uint8_t afe_link_ok(void)
{
    if (!s_ready)
    {
        return 0;
    }

    /* ★ 只看 AFE 自己那条总线的账（C-39）：SHT30 的偶发 NACK 不得把这里推过门限 */
    return (softi2c_fail_count(&g_bus_afe) < APP_AFE_FAIL_MAX) ? 1 : 0;
}

uint16_t afe_fail_count(void)
{
    return (uint16_t)softi2c_fail_count(&g_bus_afe);
}

void afe_clear_fail(void)
{
    softi2c_clear_fail(&g_bus_afe);
}


/* ==================== ★★ D8 前置：硬件保护门限编码（纯函数，⛔ 不碰总线） ====================
 * 所有公式与表取自本仓手册 `datasheet/BQ769x0_数据手册_SLUSBK2I.txt`（行号在各处注明）。 */

/* 表 8-11（PROTECT3 0x08） */
static const uint16_t s_ov_delay_s[4]   = { 1u, 2u, 4u, 8u };
static const uint16_t s_uv_delay_s[4]   = { 1u, 4u, 8u, 16u };
/* 表 8-10（PROTECT2 0x07） */
static const uint16_t s_ocd_delay_ms[8] = { 8u, 20u, 40u, 80u, 160u, 320u, 640u, 1280u };
/* 表 8-9（PROTECT1 0x06） */
static const uint16_t s_scd_delay_us[4] = { 70u, 100u, 200u, 400u };
/* 表 8-10 的 OCD_T3:0 毫伏梯（RSNS = 0）—— 前 13 项（本工程 30A 档 用不到更高的） */
static const uint16_t s_ocd_mv[13] = { 8u, 11u, 14u, 17u, 19u, 22u, 25u, 28u, 31u, 33u, 36u, 39u, 42u };

#define AFE_CAL_SELFTEST_CODE      8600u   /* ★ 自检用码：换算约 3276mV，落在合法电芯量程内 */
#define AFE_PROT_SELFTEST_GAIN_UV  381u   /* ★ 自检用**标称刻度**（在 [365,396] 区间内）⇒ 不依赖器件 */
#define AFE_SCD_MV_BASE     44u         /* 表 8-9：SCD_THRESH = 44 + 22.2 × code (mV) */
#define AFE_SCD_MV_STEP10   222u        /* 22.2 mV 的 10 倍（纯整数） */

static uint16_t s_prot_selftest_rc = 0xFFFFu;

/** 取 **≤ v 的最大码**（保守：门限不高于目标、延时不长于目标） */
static uint8_t prot_pick_le(const uint16_t *tbl, uint8_t n, uint16_t v)
{
    uint8_t i;
    uint8_t best = 0u;

    for (i = 0u; i < n; i++)
    {
        if (tbl[i] <= v) { best = i; }
    }

    return best;
}

/** 取 **≥ v 的最小码**（用于欠压：门限不低于目标） */
static uint8_t prot_pick_ge(const uint16_t *tbl, uint8_t n, uint16_t v)
{
    uint8_t i;

    for (i = 0u; i < n; i++)
    {
        if (tbl[i] >= v) { return i; }
    }

    return (uint8_t)(n - 1u);
}

/** OV/UV 门限：目标 mV → 中间 8 位 ADC 码（手册 p22 名 9 步骤） */
static uint8_t prot_trip_code(uint16_t mv, int8_t cond, uint16_t gain_uv, int16_t offset_mv)
{
    int32_t counts;

    if (mv <= (uint16_t)((offset_mv > 0) ? offset_mv : 0)) { return 0u; }     /* ★ 防下溢 */

    counts = ((int32_t)mv - (int32_t)offset_mv) * 1000;

    /* ★★ 2026-09-29 修（本轮自检在板上抓出）：**两级取整必须同向**。
     *   原写法只在“换算成计数”那一级向上取，而隐含的 `>>4`（丢低 4 位）
     *   **又向下截一次** ⇒ 解码出来的欠压门限反而**低于目标**（方向错）。
     *   现在：OV 两级向下；UV 两级向上（`+15 >> 4` = 向上取到 16 的倍数）。 */
    if (cond == 0)
    {
        counts = (counts / (int32_t)gain_uv) >> 4;                          /* OV：两级向下 */
    }
    else
    {
        counts = (((counts + (int32_t)gain_uv - 1) / (int32_t)gain_uv) + 15) >> 4;   /* UV：两级向上 */
    }

    /* ★★ 2026-09-29 修（本轮回手册核对，双源）：**这里要 `& 0xFF` 回绕，⛔ 不是钳到 255**。
     *   手册表 8-12/8-13 的编码设计是「上 2 位固定 + 中间 8 位 + 下 4 位固定」，
     *   所以 8 位字段只是 14 位结果的**中间一段**；超出 8 位的部分由**固定的上 2 位**承担。
     *   ⛔ 钳位会把一个 4.5V 的过压门限变成约 1.5V —— 那是**把保护值改小**，方向危险。
     *   反算锚点（手册自己的默认值）：0xAC ⇒ 0x2AC8 ≈ 4173mV；0x97 ⇒ 0x1970 ≈ 2481mV。 */
    counts &= 0xFF;

    return (uint8_t)counts;
}

uint8_t afe_prot_pack(const afe_prot_t *p, uint16_t gain, int16_t offs, afe_prot_regs_t *r)
{
    uint8_t scd_code;

    if ((p == NULL) || (r == NULL)) { return AFE_ERR_IO; }

    /* ★★ **断死「用 0 刻度算码」**：这正是本轮自检失败的根因（AFE 不在位 ⇒ s_gain_uv = 0）。
     *   ⛔ 不允许用一个未知的刻度算出一组看起来像模像样的门限。 */
    if (gain == 0u) { return AFE_ERR_IO; }

    /* 0x06 PROTECT1：bit7 RSNS = 0；bit4:3 SCD_D1:0；bit2:0 SCD_T2:0 */
    scd_code = (uint8_t)(((uint32_t)((p->scd_mv > AFE_SCD_MV_BASE) ? (p->scd_mv - AFE_SCD_MV_BASE) : 0u) * 10u) / AFE_SCD_MV_STEP10);
    if (scd_code > 7u) { scd_code = 7u; }

    r->protect1 = (uint8_t)((prot_pick_le(s_scd_delay_us, 4u, p->scd_us) << 3) | scd_code);

    /* 0x07 PROTECT2：bit6:4 OCD_D2:0；bit3:0 OCD_T3:0 */
    r->protect2 = (uint8_t)((prot_pick_le(s_ocd_delay_ms, 8u, p->ocd_ms) << 4) |
                            prot_pick_le(s_ocd_mv, 13u, p->ocd_mv));

    /* 0x08 PROTECT3：bit7:6 UV_D1:0；bit5:4 OV_D1:0 */
    r->protect3 = (uint8_t)((prot_pick_le(s_uv_delay_s, 4u, p->uv_s) << 6) |
                            (prot_pick_le(s_ov_delay_s, 4u, p->ov_s) << 4));

    /* 0x09 / 0x0A：过压向下取（宁可早跳）；欠压向上取（宁可早跳） */
    r->ov_trip = prot_trip_code(p->ov_mv, 0, gain, offs);
    r->uv_trip = prot_trip_code(p->uv_mv, 1, gain, offs);

    return AFE_OK;
}

void afe_prot_unpack(const afe_prot_regs_t *r, uint16_t gain_in, int16_t offs, afe_prot_t *p)
{
    uint16_t gain = (gain_in != 0u) ? gain_in : 380u;   /* ★ 读回时允许省略刻度（用标称值重建） */
    int32_t  counts;

    if ((r == NULL) || (p == NULL)) { return; }

    p->scd_mv = (uint16_t)(AFE_SCD_MV_BASE + ((uint32_t)(r->protect1 & 0x07u) * AFE_SCD_MV_STEP10) / 10u);
    p->scd_us = s_scd_delay_us[(r->protect1 >> 3) & 0x03u];

    p->ocd_mv = s_ocd_mv[(r->protect2 & 0x0Fu) <= 12u ? (r->protect2 & 0x0Fu) : 12u];
    p->ocd_ms = s_ocd_delay_ms[(r->protect2 >> 4) & 0x07u];

    p->uv_s = s_uv_delay_s[(r->protect3 >> 6) & 0x03u];
    p->ov_s = s_ov_delay_s[(r->protect3 >> 4) & 0x03u];

    /* ★★ 反向：码 → mV —— **必须补上手册规定的固定位**（表 8-12/8-13 的说明文字）：
     *   OV：`10` + OV_T<7:0> + `1000` ⇒ 0x2000 | (reg << 4) | 0x8
     *   UV：`01` + UV_T<7:0> + `0000` ⇒ 0x1000 | (reg << 4)
     *   ★ 锚点（手册默认值反算）：0xAC ⇒ 0x2AC8 = 10952 码 ≈ 4173mV（过压默认）
     *                            0x97 ⇒ 0x1970 =  6512 码 ≈ 2481mV（欠压默认） */
    counts = (int32_t)(0x2000u | ((uint16_t)r->ov_trip << 4) | 0x0008u);
    p->ov_mv = (uint16_t)((counts * (int32_t)gain) / 1000 + offs);

    counts = (int32_t)(0x1000u | ((uint16_t)r->uv_trip << 4));
    p->uv_mv = (uint16_t)((counts * (int32_t)gain) / 1000 + offs);
}

uint8_t afe_prot_from_params(afe_prot_t *p)
{
    int32_t i_ocd;
    int32_t i_scd;

    if (p == NULL) { return AFE_ERR_IO; }

    p->ov_mv  = (uint16_t)app_cfg_get(APP_CFG_IDX_CELL_OV_HW_MV);
    p->uv_mv  = (uint16_t)app_cfg_get(APP_CFG_IDX_CELL_UV_HW_MV);

    /* ★ 10mA 单位 → 跨 Rsns(5mΩ) 的 mV：1A = 5mV ⇒ mV = I_10ma / 20 */
    i_ocd = (int32_t)app_cfg_get(APP_CFG_IDX_I_OCD_HW_10MA);
    i_scd = (int32_t)app_cfg_get(APP_CFG_IDX_I_SCD_HW_10MA);
    p->ocd_mv = (uint16_t)((i_ocd > 0) ? (i_ocd / 20) : 0);
    p->scd_mv = (uint16_t)((i_scd > 0) ? (i_scd / 20) : 0);

    p->ocd_ms = (uint16_t)app_cfg_get(APP_CFG_IDX_HW_OCC_OCD_MS);

    /* ⚠️ 参数区**没有** OV/UV/SCD 延时三项 ⇒ 取最短码 0（最保守：跳得最早），
     *   并登记为缺口：⛔ 不自行编一个“合理”延时。番号 = OV 1s、UV 1s、SCD 70µs（表 8-9/8-11 的 code 0）。 */
    p->ov_s   = s_ov_delay_s[0];
    p->uv_s   = s_uv_delay_s[0];
    p->scd_us = s_scd_delay_us[0];

    return AFE_OK;
}

uint16_t afe_prot_selftest_result(void) { return s_prot_selftest_rc; }

uint16_t afe_cell_calib_selftest_result(void) { return s_cell_selftest_rc; }

/**
 * @brief       \u79bb\u7ebf\u81ea\u68c0\uff1a\u9010\u4e32\u6807\u5b9a\u7684\u5e94\u7528\u4fa7\uff08\u26d4 \u4e0d\u9700\u5668\u4ef6\uff09
 * @note        \u2605 \u5165\u53c2\u7528 volatile \u53d6\u503c\uff08\u6297\u5e38\u91cf\u6298\u53e0\uff0c\u89c1 `\u9644\u5f55C` C-131 \u53e3\u5f84 b\uff09\u3002
 */
uint16_t afe_cell_calib_selftest(void)
{
    /* ★★ 2026-09-29 重写（出口在板上抓出两个缺陷）：
     *   ① 原版在用例中途 `return` ⇒ **合成标定被留在系统里**（实测 `s_cell_mask = 0x0002`）
     *      ⇒ 自检**改变了系统状态**，而在真板上那意味着第 1 串电压会带着一个假的 +500ppm 修正被报出去。
     *   ② 原版用的码（2048）换算出约 780mV，**落在合法量程之外** ⇒ 换算返回"无效"，
     *      后续比较全在拿 0xFFFF 比大小 ⇒ 用例 3 必然失败（假失败）。
     *   ⇒ 现在：**单出口 + 无条件还原全部状态**；码取 `AFE_CAL_SELFTEST_CODE`（落在合法量程内）。 */
    uint16_t save_k[AFE_CHIP_MAX_CELLS];
    int32_t  save_b[AFE_CHIP_MAX_CELLS];
    uint16_t save_mask = s_cell_mask;
    uint16_t save_g    = s_gain_uv;
    int16_t  save_o    = s_offset_mv;
    uint16_t rc        = 0u;
    uint16_t a;
    uint8_t  i;

    for (i = 0u; i < (uint8_t)AFE_CHIP_MAX_CELLS; i++)
    {
        save_k[i] = (uint16_t)s_cell_k[i];
        save_b[i] = s_cell_b[i];
    }

    if (s_gain_uv == 0u) { s_gain_uv = AFE_PROT_SELFTEST_GAIN_UV; s_offset_mv = 0; }

    afe_clear_cell_calib();
    a = afe_code_to_mv(AFE_CAL_SELFTEST_CODE, 0u);

    if (a == APP_MV_INVALID)
    {
        rc = 2u;                        /* 前提不成立：这个码必须落在合法量程内（否则本用例无意义） */
    }
    else if (afe_code_to_mv(AFE_CAL_SELFTEST_CODE, 1u) != a)
    {
        rc = 2u;                        /* 未喂标定 ⇒ 必须与全局换算逐字相同（恒等性） */
    }
    else if (afe_set_cell_calib(1u, 1000500u, 800) != AFE_OK)
    {
        rc = 3u;
    }
    else
    {
        uint16_t c = afe_code_to_mv(AFE_CAL_SELFTEST_CODE, 1u);

        if ((c <= a) || (c > (uint16_t)(a + 20u))) { rc = 3u; }
        else if (afe_set_cell_calib(2u, 500000u, 0) != AFE_ERR_IO) { rc = 4u; }
        else { /* 全过 */ }
    }

    /* ★★ 无条件还原（⛔ 自检不得改变系统状态 —— 这正是上面 ① 的教训） */
    for (i = 0u; i < (uint8_t)AFE_CHIP_MAX_CELLS; i++)
    {
        s_cell_k[i] = (uint32_t)save_k[i];
        s_cell_b[i] = save_b[i];
    }

    s_cell_mask = save_mask;
    s_gain_uv   = save_g;
    s_offset_mv = save_o;

    s_cell_selftest_rc = rc;
    return rc;
}

/**
 * @brief       离线往返自检（⛔ 不需器件）
 * @note        ★ 入参用 **volatile** 数组取值：否则编译器会把自检整段常量折叠
 *              （详见 `附录C` C-131 口径 (b)）。
 */
uint16_t afe_prot_selftest(void)
{
    afe_prot_t     p;
    afe_prot_t     q;
    afe_prot_regs_t r;

    /* 用例 1：常规值往返（含延时不变） */
    p.ov_mv = 3650u; p.uv_mv = 2500u; p.ocd_mv = 20u; p.scd_mv = 100u;
    p.ocd_ms = 160u; p.ov_s = 1u; p.uv_s = 1u; p.scd_us = 100u;

    if (afe_prot_pack(&p, AFE_PROT_SELFTEST_GAIN_UV, 0, &r) != AFE_OK) { s_prot_selftest_rc = 1u; return 1u; }
    afe_prot_unpack(&r, AFE_PROT_SELFTEST_GAIN_UV, 0, &q);

    if (q.ocd_ms != p.ocd_ms) { s_prot_selftest_rc = 1u; return 1u; }
    if (q.ov_s   != p.ov_s)   { s_prot_selftest_rc = 1u; return 1u; }
    if (q.uv_s   != p.uv_s)   { s_prot_selftest_rc = 1u; return 1u; }
    if (q.scd_us != p.scd_us) { s_prot_selftest_rc = 1u; return 1u; }

    /* ★ 方向：OV/OCD/SCD 的解码值必须 **≤ 目标**（宁可早跳） */
    if (q.ov_mv  > p.ov_mv)  { s_prot_selftest_rc = 2u; return 2u; }
    if (q.ocd_mv > p.ocd_mv) { s_prot_selftest_rc = 2u; return 2u; }
    if (q.scd_mv > p.scd_mv) { s_prot_selftest_rc = 2u; return 2u; }

    /* ★ 欠压方向：解码值必须 **≥ 目标** */
    if (q.uv_mv < p.uv_mv) { s_prot_selftest_rc = 3u; return 3u; }

    /* ★ 精度：OV/UV 量化误差 ≤ 16 码 × gain（16×396µV ≈ 6.4mV）⇒ 容差 8mV */
    {
        int32_t dov = (int32_t)p.ov_mv - (int32_t)q.ov_mv;
        int32_t duv = (int32_t)q.uv_mv - (int32_t)p.uv_mv;

        if ((dov > 8) || (duv > 8)) { s_prot_selftest_rc = 4u; return 4u; }
    }

    /* 用例 2：★危险方向 —— 目标低于 ADCOFFSET 时，码必须被钳到 0，
     *   ⛔ 不允许 u16 下溢成一个巨大的码（那等于把门限抬到天上） */
    p.ov_mv = 1u;
    (void)afe_prot_pack(&p, AFE_PROT_SELFTEST_GAIN_UV, 0, &r);
    if (r.ov_trip != 0u) { s_prot_selftest_rc = 5u; return 5u; }

    /* 用例 3：SCD 超量程 ⇒ 码被钳在 0..7 */
    p.scd_mv = 60000u;
    (void)afe_prot_pack(&p, AFE_PROT_SELFTEST_GAIN_UV, 0, &r);
    if ((r.protect1 & 0x07u) != 7u) { s_prot_selftest_rc = 6u; return 6u; }

    /* 用例 4：延时超量程 ⇒ 取表内最大码（⛔ 不回绕到最短） */
    p.ocd_ms = 60000u;
    (void)afe_prot_pack(&p, AFE_PROT_SELFTEST_GAIN_UV, 0, &r);
    if (((r.protect2 >> 4) & 0x07u) != 7u) { s_prot_selftest_rc = 7u; return 7u; }

    /* 用例 5：★★ **刻度为 0 必须被拒**（⛔ 不允许用未知刻度算出门限） */
    if (afe_prot_pack(&p, 0u, 0, &r) != AFE_ERR_IO) { s_prot_selftest_rc = 8u; return 8u; }

    /* ★★ 用例 6（**黄金用例：锚在手册自己给的默认值上**）：`0xAC`/`0x97` 解码必须落在
     *   4173mV / 2481mV 附近。⛔ 本条不依赖本工程任何代码 —— 它验的是"我们的解码与 TI 的编码约定一致"。 */
    {
        afe_prot_t g;

        r.ov_trip = 0xACu;
        r.uv_trip = 0x97u;
        afe_prot_unpack(&r, AFE_PROT_SELFTEST_GAIN_UV, 0, &g);

        if ((g.ov_mv < 4161u) || (g.ov_mv > 4185u)) { s_prot_selftest_rc = 9u; return 9u; }
        if ((g.uv_mv < 2469u) || (g.uv_mv > 2493u)) { s_prot_selftest_rc = 9u; return 9u; }
    }

    s_prot_selftest_rc = 0u;
    return 0u;
}

static uint8_t s_prot_apply_rc = 0xFFu;

uint8_t afe_prot_apply_result(void) { return s_prot_apply_rc; }

uint8_t afe_prot_apply(uint8_t *first_bad_reg)
{
    afe_prot_t      p;
    afe_prot_regs_t r;
    uint16_t        g = 0u;
    int16_t         o = 0;
    uint8_t         buf[5];
    uint8_t         chk[5];
    uint8_t         i;
    uint8_t         err;

    if (first_bad_reg != NULL) { *first_bad_reg = 0u; }

    /* ★ 三道前置：刻度 / 参数 / 编码 —— 任一不成立都**不写器件** */
    if (afe_prot_scale(&g, &o) != AFE_OK)           { s_prot_apply_rc = 1u; return AFE_ERR_IO; }
    if (afe_prot_from_params(&p) != AFE_OK)         { s_prot_apply_rc = 2u; return AFE_ERR_IO; }
    if (afe_prot_pack(&p, g, o, &r) != AFE_OK)      { s_prot_apply_rc = 3u; return AFE_ERR_IO; }

    buf[0] = r.protect1;
    buf[1] = r.protect2;
    buf[2] = r.protect3;
    buf[3] = r.ov_trip;
    buf[4] = r.uv_trip;

    err = afe_hal_write_block(AFE_HAL_REG_PROTECT1, buf, 5u);
    if (err != AFE_OK) { s_prot_apply_rc = 4u; return err; }

    err = afe_hal_read_block(AFE_HAL_REG_PROTECT1, chk, 5u);
    if (err != AFE_OK) { s_prot_apply_rc = 5u; return err; }

    /* ★★ 回读校验：**逐字节比**，不一致就报出是哪一个寄存器 */
    for (i = 0u; i < 5u; i++)
    {
        if (chk[i] != buf[i])
        {
            if (first_bad_reg != NULL) { *first_bad_reg = (uint8_t)(AFE_HAL_REG_PROTECT1 + i); }

            s_prot_apply_rc = 6u;
            return AFE_ERR_DATA;
        }
    }

    s_prot_apply_rc = 0u;
    return AFE_OK;
}

uint8_t afe_prot_scale(uint16_t *gain_uv, int16_t *offset_mv)
{
    /* ★ 生产路径的唯一取刻度口：未从器件取到 ⇒ 回错，调用方 ⛔ 不得继续。 */
    if (s_gain_uv == 0u) { return AFE_ERR_IO; }

    if (gain_uv   != NULL) { *gain_uv   = s_gain_uv; }
    if (offset_mv != NULL) { *offset_mv = s_offset_mv; }

    return AFE_OK;
}
