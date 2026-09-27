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
#include "./BSP/AFE/afe_hal.h"
#include "./BSP/SOFTI2C/softi2c.h"
#include "./App/app_config.h"

static uint16_t s_gain_uv   = 0;        /* 0 = 尚未取到标定值，任何测量都不许开始 */
static int16_t  s_offset_mv = 0;
static uint8_t  s_ready     = 0;

/* ===================== 换算 ===================== */

static uint16_t afe_code_to_mv(uint16_t code)
{
    int32_t uv = (int32_t)code * (int32_t)s_gain_uv + (int32_t)s_offset_mv * 1000;
    int32_t mv = uv / 1000;

    if ((mv < (int32_t)APP_BMS_CELL_MV_VALID_MIN) || (mv > (int32_t)APP_BMS_CELL_MV_VALID_MAX))
    {
        return APP_MV_INVALID;      /* 量程外一律判无效，不"就近取整"（§4.4 无效值口径） */
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
        mv[i] = afe_code_to_mv(code[i]);

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
        mv[i] = (err == AFE_OK) ? afe_code_to_mv(code[i]) : APP_MV_INVALID;
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
