/**
 ****************************************************************************************************
 * @file        ina226.c
 * @brief       INA226 驱动实现（`g_bus_sens` = PB6/PB7，地址 0x40）
 * @note        移植来源 / 改动点 / 验证方式 / 寄存器取证出处 —— 全部见 `ina226.h` 文件头（AGENTS §3 G5）。
 *              本文件里 ⛔ **不得出现位操作与 `delay_ms`**：时序归 `Drivers/BSP/SOFTI2C/`，
 *              节拍归调用方（S12）。⛔ 也不得自带锁：事务仲裁在 `softi2c.c` 的三个入口里。
 ****************************************************************************************************
 */

#include "./BSP/INA226/ina226.h"
#include "./BSP/SOFTI2C/softi2c.h"
#include "./App/app_config.h"
#include <stddef.h>

/* ===================== 跨层编译期断言（★ 只有 .c 里同时看得见两边） =====================
 * ⚠️ 为什么这几条**不放在 `app_config.h`**：它们同时引用器件常量（`ina226.h`）与工程参数
 *    （`app_config.h`），而 `app_config.h` 明令**不 include BSP 头**（分层，§4.2；
 *    `app_config.h` §12.5 那条关于 SHT30 转换窗的断言就是因此写成字面量的）。
 *    放在这里既不破分层，又让"器件量程装不下工程电流"变成编译错误而不是一个静默的饱和值。 */

/* ① 分流器电压满量程必须盖住设计上限电流：I(mA) × R(mΩ) = µV（⚠️ 单位当场推：mA×mΩ = µV）。
 *    15000 × 5 = 75000µV ≤ 81920µV ✓。★ 把 `APP_INA226_RSHUNT_MOHM` 误留成模块自带的 100
 *    时本条先响（15000×100 = 1500000 > 81920）= "忘了换 5mΩ 电阻"当场编译不过，
 *    而不会变成一个"读数只有真值 1/20"的静默错误。 */
APP_STATIC_ASSERT(APP_DESIGN_CURRENT_MA * APP_INA226_RSHUNT_MOHM <= INA226_SHUNT_FS_UV,
                  ina226_shunt_fs_below_design);

/* ② 总线电压量程必须盖住"合法组压上限"：`APP_BMS_PACK_MV_VALID_MAX` = 16000mV
 *    = 16,000,000µV ≤ 40,960,000µV ✓。它挡的是"用 INA226 当冗余组压通道，但量程不够" ——
 *    那会让满载时冗余通道先饱和，而互校判据（≤1%）仍然算得出一个数（假绿）。 */
APP_STATIC_ASSERT(APP_BMS_PACK_MV_VALID_MAX * 1000u <= INA226_BUS_FS_UV,
                  ina226_bus_fs_below_pack);

/* ===================== 本驱动最近一次读的两个形态（取证出口） ===================== */
static int16_t  s_last_shunt_10ma;      /* 形态 1：分流器电压反算（不依赖 CAL） */
static int16_t  s_last_cal_10ma;        /* 形态 2：电流寄存器反算（依赖 CAL 已写对） */
static uint16_t s_last_vbus_10mv;
static uint32_t s_xcheck_fail;

/**
 * @brief       把 `softi2c_*` 的返回码**逐个映射**成本驱动的返回码
 * @note        ⛔ 关键口径：**不许把 `I2C_ERR_LOCK` / `I2C_ERR_BUS` 折叠成 NACK**。
 *              "没拿到锁 ⇒ 本次事务没发生"与"器件无应答"是**两种完全不同的现场**：
 *              折叠之后，一个被抢占的总线会被读成"INA226 坏了"（C-96 ⑥ 已经把
 *              `I2C_ERR_LOCK` 单列出来，驱动侧不能在映射时把它又糊回去）。
 */
static uint8_t ina226_map_i2c(uint8_t rc)
{
    switch (rc)
    {
        case I2C_OK:        return INA226_OK;
        case I2C_ERR_NACK:  return INA226_ERR_NACK;
        case I2C_ERR_BUS:   return INA226_ERR_BUS;
        case I2C_ERR_LOCK:  return INA226_ERR_LOCK;
        default:            return INA226_ERR_PARAM;    /* I2C_ERR_PARAM = 我们传错了参数 = 代码 bug */
    }
}

/**
 * @brief       对称舍入的整数除法：round-half-away-from-zero，⛔ 不是朝 0 截断
 * @note        为什么必须显式写：C99 的 `/` 对负数**朝 0 截断** ⇒ `-5/20 = 0` 而 `5/20 = 0`，
 *              看似对称；但 `-25/20 = -1` 而 `25/20 = 1`，误差随分母放大时两侧就偏得不一致了。
 *              定标是"每一条读数都要过一遍"的乘法链，偏一点就是 SOC 积分里的系统误差。
 */
static int32_t ina226_div_round(int32_t num, int32_t den)
{
    if (den <= 0)
    {
        return 0;                       /* 配置错（分母由宏反算，静态断言已挡住 ≤0 的可能） */
    }

    if (num >= 0)
    {
        return (num + den / 2) / den;
    }

    return (num - den / 2) / den;
}

/**
 * @brief       形态 1：分流器电压原始码 → 电流（单位 10mA）
 * @note        推导（⛔ 不许凭记忆写常数）：
 *                U[nV]  = raw × INA226_SHUNT_LSB_NV
 *                每 10mA 对应的分流器电压 = I×R = 10mA × R[mΩ] = R × 10⁴ nV
 *                （当场推单位：10mA × 1mΩ = 10⁻²A × 10⁻³Ω = 10⁻⁵V = 10⁴ nV ✓）
 *              R = 5 时：每单位 = 50000nV ⇒ raw=1000（= 500mA）→ 2500000/50000 = 50 ✓
 */
static int16_t ina226_shunt_to_10ma(int16_t raw)
{
    int32_t num = (int32_t)raw * (int32_t)INA226_SHUNT_LSB_NV;
    int32_t den = (int32_t)APP_INA226_RSHUNT_MOHM * 10000;

    return (int16_t)ina226_div_round(num, den);
}

/**
 * @brief       形态 2：电流寄存器原始码 → 电流（单位 10mA）
 * @note        手册 §6.5.1：电流寄存器的 1 LSB = `Current_LSB`（本版 1mA/bit，由
 *              `APP_INA226_CAL` 的推导钉住）⇒ 原始码本身就是 mA ⇒ 除以 10 得到 10mA 单位。
 *              ⚠️ 本形态**依赖 CALIBRATION 已写对**；形态 1 不依赖 —— 这正是"两种形态"的意义：
 *              两者一致时，CAL 写错、分流器阻值与宏不符这两类错误**至少有一个会露头**。
 */
static int16_t ina226_cal_to_10ma(int16_t raw)
{
    int32_t num = (int32_t)raw * (int32_t)APP_INA226_I_LSB_UA;
    int32_t den = 10000;                /* 1 单位 = 10mA = 10000µA */

    return (int16_t)ina226_div_round(num, den);
}

/** 写一个 16 位寄存器：S addr+W reg hi lo P */
static uint8_t ina226_write_reg(uint8_t reg, uint16_t val)
{
    uint8_t buf[3];

    buf[0] = reg;
    buf[1] = (uint8_t)(val >> 8);
    buf[2] = (uint8_t)(val & 0xFFu);

    return ina226_map_i2c(softi2c_write(&g_bus_sens, INA226_I2C_ADDR, buf, 3));
}

/** 读一个 16 位寄存器：S addr+W reg Sr addr+R hi lo P（大端，与手册一致） */
static uint8_t ina226_read_reg(uint8_t reg, uint16_t *val)
{
    uint8_t b[2];
    uint8_t rc;

    if (val == NULL)
    {
        return INA226_ERR_PARAM;
    }

    /* ⚠️ 必须把返回码**接住一次**再判：直接写成
     *      if (softi2c_read(...) != I2C_OK) { return map(softi2c_read(...)); }
     *    会把同一笔事务发两遍（第二遍还可能成功），于是"读失败"变成"读成功"，
     *    而失败计数已经加过了 —— 一次读、一份账，是本行的全部意思。 */
    rc = softi2c_read(&g_bus_sens, INA226_I2C_ADDR, &reg, 1, b, 2);

    if (rc != I2C_OK)
    {
        return ina226_map_i2c(rc);
    }

    *val = (uint16_t)(((uint16_t)b[0] << 8) | (uint16_t)b[1]);

    return INA226_OK;
}

/** 写后回读比对：返回 INA226_OK 表示"写进去了"这件事被**寄存器现状**证明（不是被断言证明） */
static uint8_t ina226_write_verify(uint8_t reg, uint16_t val)
{
    uint16_t rd = 0x0000u;
    uint8_t  err;

    err = ina226_write_reg(reg, val);

    if (err != INA226_OK)
    {
        return err;
    }

    err = ina226_read_reg(reg, &rd);

    if (err != INA226_OK)
    {
        return err;
    }

    return (rd == val) ? INA226_OK : INA226_ERR_VERIFY;
}

uint8_t ina226_probe(void)
{
    /* len=0 的纯地址写：器件在位就一定回 ACK（同 `sht30_probe()` / `at24cxx_probe()` 口径） */
    return ina226_map_i2c(softi2c_write(&g_bus_sens, INA226_I2C_ADDR, 0, 0));
}

uint8_t ina226_id_read(uint16_t *mfg_id, uint16_t *die_id)
{
    uint16_t v = 0x0000u;
    uint8_t  err;

    err = ina226_read_reg(INA226_REG_MANUFACTURER_ID, &v);

    if (err != INA226_OK)
    {
        return err;
    }

    if (mfg_id != NULL)
    {
        *mfg_id = v;
    }

    err = ina226_read_reg(INA226_REG_DIE_ID, &v);

    if (err != INA226_OK)
    {
        return err;
    }

    if (die_id != NULL)
    {
        *die_id = v;
    }

    return INA226_OK;
}

uint8_t ina226_init(void)
{
    uint16_t cfg;
    uint8_t  err;

    /* CONFIG = AVG | VBUSCT | VSHCT | MODE。
     * ⛔ RST 位保持 0（软复位会把配置打回默认值，多出一段必须靠等待越过的窗口，见 ina226.h）。
     * ⛔ bit14~12 保持 0：手册把它们标为固定 100b，但**本版没有取到"写 0 也安全"的原文**，
     *    所以既不写 1 也不依赖它们 —— 整字回读比对会告诉我们写进去的到底是什么。 */
    cfg = (uint16_t)(((uint16_t)APP_INA226_AVG_CODE    << INA226_CFG_AVG_SHIFT)    |
                     ((uint16_t)APP_INA226_VBUSCT_CODE << INA226_CFG_VBUSCT_SHIFT) |
                     ((uint16_t)APP_INA226_VSHCT_CODE  << INA226_CFG_VSHCT_SHIFT)  |
                     (uint16_t)INA226_MODE_SHUNT_BUS_CONT);

    err = ina226_write_verify(INA226_REG_CONFIG, cfg);

    if (err != INA226_OK)
    {
        return err;
    }

    /* CALIBRATION：CAL = 0.00512 / (Current_LSB[A] × R[Ω])，整数式见 `app_config.h` §12.12。
     * ⚠️ 本步失败不影响形态 1（分流器电压反算不读 CAL），但形态 2 就不可信了 ⇒ 由 `_read()`
     *    的比对来暴露（这正是"两个形态"要解决的事，而不是"两个都别信"）。 */
    err = ina226_write_verify(INA226_REG_CALIBRATION, (uint16_t)APP_INA226_CAL);

    if (err != INA226_OK)
    {
        return err;
    }

    /* MASK_ENABLE = 0：★ 本版 **不接 ALERT**（板上 PD13 的 ALERT 是 AFE 的，见 `Tech_Design.md` §2）。
     * 写 0 = 关掉所有告警屏蔽位的使能与转换就绪标志，让这颗芯片**只能被动读**，
     * 不可能通过 ALERT 引脚影响任何别的电路（S4：不得出现"绕过/干扰硬件保护"的通路）。 */
    err = ina226_write_verify(INA226_REG_MASK_ENABLE, 0x0000u);

    if (err != INA226_OK)
    {
        return err;
    }

    return INA226_OK;
}

uint8_t ina226_read(int16_t *curr_10ma, uint16_t *vbus_10mv)
{
    uint16_t raw_sh = 0x0000u;
    uint16_t raw_cu = 0x0000u;
    uint16_t raw_vb = 0x0000u;
    uint8_t  err;
    int32_t  diff;

    if ((curr_10ma == NULL) && (vbus_10mv == NULL))
    {
        return INA226_ERR_PARAM;
    }

    /* 三笔独立事务：器件不支持跨寄存器自增读（手册 §6.5.5.3），照实发三次。
     * ⚠️ 三次之间可能被别的任务插进来（同一条 `g_bus_sens`）—— 每次事务各自持锁，
     *    所以最坏情况是"三笔来自三个不同瞬间"，而不是"帧撕裂"。这个偏差对 100ms 级的
     *    电流/组压读数无意义，但必须写在这里：否则下一个人会以为这是一次原子快照。 */
    err = ina226_read_reg(INA226_REG_SHUNT_VOLTAGE, &raw_sh);

    if (err != INA226_OK)
    {
        return err;
    }

    err = ina226_read_reg(INA226_REG_CURRENT, &raw_cu);

    if (err != INA226_OK)
    {
        return err;
    }

    err = ina226_read_reg(INA226_REG_BUS_VOLTAGE, &raw_vb);

    if (err != INA226_OK)
    {
        return err;
    }

    s_last_shunt_10ma = ina226_shunt_to_10ma((int16_t)raw_sh);
    s_last_cal_10ma   = ina226_cal_to_10ma((int16_t)raw_cu);
    /* 总线电压寄存器是**无符号**（只有正向），LSB 1.25mV → 10mV 单位：
     *   raw × 1250µV / 10000µV = raw / 8，四舍五入 (raw + 4) / 8。
     * 上界：32767 / 8 ≈ 4095（40.95V）⇒ 与 `APP_MV_INVALID(w 0xFFFF)` 不可能撞值。 */
    s_last_vbus_10mv  = (uint16_t)((raw_vb + 4u) / 8u);

    if (curr_10ma != NULL)
    {
        *curr_10ma = s_last_shunt_10ma;
    }

    if (vbus_10mv != NULL)
    {
        *vbus_10mv = s_last_vbus_10mv;
    }

    diff = (int32_t)s_last_shunt_10ma - (int32_t)s_last_cal_10ma;

    if (diff < 0)
    {
        diff = -diff;
    }

    if (diff > (int32_t)APP_INA226_XCHECK_10MA)
    {
        s_xcheck_fail++;

        return INA226_ERR_XCHECK;
    }

    return INA226_OK;
}

uint8_t ina226_selftest(void)
{
    uint8_t  err;
    int16_t  cu = 0;
    uint16_t vb = 0;

    err = ina226_probe();

    if (err != INA226_OK)
    {
        return err;                     /* 不在位：后续三步没有意义，⛔ 不要用"再写一次试试"掩盖 */
    }

    err = ina226_init();

    if (err != INA226_OK)
    {
        return err;
    }

    /* 读一次，让两个形态都有值（同时把"寄存器确实可读"这件事一并证明）。
     * ⚠️ 此处**不把 `INA226_ERR_XCHECK` 当自检失败**：自检跑在"配置刚写完、器件刚进入连续
     *    转换"的时刻，电流寄存器与分流器电压寄存器**可能来自两个不同的转换周期**（各 1.1ms），
     *    静态小电流下两者差 1~2 个单位是正常的。真正的两条判据（方向符号 / 互校 ≤1%）
     *    是 D10 的台架项，⛔ 不得用自检这一行把它记成"已经验过"。 */
    (void)ina226_read(&cu, &vb);

    return INA226_OK;
}

void ina226_last_read(int16_t *shunt_10ma, int16_t *cal_10ma, uint16_t *vbus_10mv)
{
    if (shunt_10ma != NULL)
    {
        *shunt_10ma = s_last_shunt_10ma;
    }

    if (cal_10ma != NULL)
    {
        *cal_10ma = s_last_cal_10ma;
    }

    if (vbus_10mv != NULL)
    {
        *vbus_10mv = s_last_vbus_10mv;
    }
}

uint32_t ina226_xcheck_fail_count(void)
{
    return s_xcheck_fail;
}
