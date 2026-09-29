/**
 ****************************************************************************************************
 * @file        afe_hal_bq76920.c
 * @brief       bq76920（bq769x0 直寄存器模型）寄存器层：BOOT 脉冲 + I2C + **CRC-8** + 位解码
 * @note        寄存器地址/位定义的唯一来源：`datasheet/BQ769x0_数据手册_SLUSBK2I_2022.pdf` §8.5
 *              （已与 `Tech_Design.md` §3.1 的寄存器映射表逐项核对；⛔ 那张表用 `pdftotext -layout`
 *              抽取会**整行错位**，地址一律以表题里的 `(0xNN)` 为准 —— 见附录C · C-64）。
 *
 *              ★ 三条硬约束（不满足就"通信根本不成立"或"保护静默缺失"）：
 *              ① **BOOT**（C-64）：POR 后必进 SHIP，ADC/比较器/I2C 全不上电；必须给 TS1 一个
 *                 > `VBOOT`(MIN 300 / MAX 1000 mV) 的上升沿，再等 `tBOOTREADY`=10ms 才能碰 I2C；
 *                 ⛔ 脉冲结束后本脚必须离开驱动状态（持续拉高会污染热敏读数）。
 *              ② **CRC**：`03` 后缀 CRC 出厂固化开启、**不可关**；多项式 x⁸+x²+x+1、初值 0。
 *                 写事务首字节 CRC 覆盖「从机地址+W / 寄存器地址 / 数据」，其后每字节只覆盖自身；
 *                 读事务首字节覆盖「从机地址+R / 数据」。坏 CRC → 从机 NACK。
 *              ③ **只开不关**：本文件任何写操作**禁止**清掉 `ADC_EN`（=0 会连 OV 保护一起关掉，S4）、
 *                 禁止写 `SHUT_A/SHUT_B`（会进 SHUTDOWN）。
 *
 *              ⚠️ 待实测：`tBOOT` 脉宽上限（手册该行 PDF 列位错乱，现取 1ms，处于 10~2000µs 区间内）；
 *                 CRC 覆盖范围（须抓 I²C 波形确认，"读寄存器能通"不证明 CRC 正确 —— C-63 自检项）。
 ****************************************************************************************************
 */

#include "./BSP/AFE/afe_hal.h"
#include "./BSP/SOFTI2C/softi2c.h"
#include "./App/app_config.h"
#include "./SYSTEM/delay/delay.h"

/* 选错型号时这个文件根本不该参与编译（C-01 的编译期守门） */
#if !APP_AFE_CHIP_BQ76920
#error "afe_hal_bq76920.c does not match the APP_AFE_CHIP_* selection: pick the chip in app_config.h 12.3"
#endif

/* ===================== BOOT 脚（Tech_Design.md §2：PE6 → 1K → TS1） ===================== */
#define AFE_BOOT_GPIO_PORT      GPIOE
#define AFE_BOOT_GPIO_PIN       GPIO_PIN_6
#define AFE_BOOT_GPIO_CLK_EN()  do{ __HAL_RCC_GPIOE_CLK_ENABLE(); }while(0)

#define AFE_BOOT_PULSE_US       1000u   /* > VBOOT(≤1000mV) 的高电平持续时间；区间 10~2000µs */
#define AFE_BOOT_READY_US       12000u  /* tBOOTREADY = 10ms，留 20% 余量后才允许第一次 I2C */

/* ===================== 寄存器地址（§8.5 表 8-14~8-23，已逐项核对） ===================== */
#define REG_SYS_STAT            0x00    /* R/W，写 1 清位 */
#define REG_CELLBAL1            0x01    /* bit4:0 = CB5..CB1 */
#define REG_SYS_CTRL1           0x04
#define REG_SYS_CTRL2           0x05
#define REG_ADCGAIN1            0x50    /* bit3:2 = ADCGAIN<4:3> */
#define REG_ADCOFFSET           0x51    /* 有符号 mV，2 的补码 */
#define REG_ADCGAIN2            0x59    /* bit7:5 = ADCGAIN<2:0> */
#define REG_VC1_HI              0x0C    /* 之后 0x0D=VC1_LO ... 自增，一串 2 字节 */
#define REG_BAT_HI              0x2A

/* SYS_CTRL1 */
#define CTRL1_ADC_EN            0x10    /* ⛔ 禁止清 0：会连 OV 保护一起关掉（S4） */
#define CTRL1_SHUT_MASK         0x03    /* ⛔ 禁止写：bit1:0 SHUT_A/SHUT_B，误按顺序写会进 SHUTDOWN */
/* SYS_CTRL2 */
#define CTRL2_CC_EN             0x40    /* 本版电流用 INA226，故关掉（也避免 ALERT 每 250ms 抖动） */
#define CTRL2_DSG_ON            0x02
#define CTRL2_CHG_ON            0x01
/* SYS_STAT */
#define STAT_CC_READY           0x80
#define STAT_UV                 0x08
#define STAT_OV                 0x04
#define STAT_SCD                0x02
#define STAT_OCD                0x01

/* 一次事务最多搬运的"数据字节"数（不含 CRC）；VC 连读 = 2 × 串数 */
#define AFE_HAL_BLK_MAX         (2u * AFE_CHIP_MAX_CELLS)

/* ===================== CRC-8（§8.3：x⁸+x²+x+1，初值 0，MSB first） ===================== */
#if AFE_USE_CRC
static uint8_t afe_crc8(const uint8_t *p, uint8_t n)
{
    uint8_t crc = 0;
    uint8_t i;

    while (n--)
    {
        crc = (uint8_t)(crc ^ *p++);

        for (i = 0; i < 8; i++)
        {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07u) : (uint8_t)(crc << 1);
        }
    }

    return crc;
}
#endif

/* ===================== 事务封装 ===================== */

/**
 * @brief       写寄存器（块写：地址自增，每字节各带一个 CRC）
 * @param       reg  起始寄存器
 * @param       dat  数据字节
 * @param       n    数据字节数 1~AFE_HAL_BLK_MAX
 */
static uint8_t afe_hal_write(uint8_t reg, const uint8_t *dat, uint8_t n)
{
    uint8_t buf[1 + 2 * AFE_HAL_BLK_MAX];
    uint8_t k = 0;
    uint8_t i;
    uint8_t err;

    if ((n == 0) || (n > AFE_HAL_BLK_MAX))
    {
        return AFE_ERR_IO;
    }

    buf[k++] = reg;

    for (i = 0; i < n; i++)
    {
        buf[k++] = dat[i];
#if AFE_USE_CRC

        if (i == 0)
        {
            /* 首字节 CRC 覆盖「从机地址+W / 寄存器地址 / 数据」（§8.3 图 8-3） */
            uint8_t hdr[3];

            hdr[0] = (uint8_t)(AFE_I2C_ADDR << 1);
            hdr[1] = reg;
            hdr[2] = dat[0];
            buf[k++] = afe_crc8(hdr, 3);
        }
        else
        {
            buf[k++] = afe_crc8(&dat[i], 1);    /* 其后每字节只覆盖自身 */
        }

#endif
    }

    err = softi2c_write(&g_bus_afe, AFE_I2C_ADDR, buf, k);

    if (err == I2C_ERR_NACK)
    {
        /* ★ 写事务里从机对坏 CRC 的响应就是 NACK（§8.3）—— 与"器件不在位"同形，必须靠读事务区分 */
        return AFE_ERR_NACK;
    }

    return (err == I2C_OK) ? AFE_OK : ((err == I2C_ERR_BUS) ? AFE_ERR_BUS : AFE_ERR_IO);
}

static uint8_t afe_hal_write1(uint8_t reg, uint8_t val)
{
    return afe_hal_write(reg, &val, 1);
}

/**
 * @brief       连读寄存器（地址自增，数据与 CRC 交替）
 * @param       reg  起始寄存器
 * @param       dat  输出，`n` 个数据字节
 * @param       n    1~AFE_HAL_BLK_MAX
 */
static uint8_t afe_hal_read(uint8_t reg, uint8_t *dat, uint8_t n)
{
#if AFE_USE_CRC
    uint8_t wire[2 * AFE_HAL_BLK_MAX];
    uint8_t rd = (uint8_t)((AFE_I2C_ADDR << 1) | 1u);
#endif
    uint8_t i;
    uint8_t err;

    if ((dat == 0) || (n == 0) || (n > AFE_HAL_BLK_MAX))
    {
        return AFE_ERR_IO;
    }

    err = softi2c_read(&g_bus_afe, AFE_I2C_ADDR, &reg, 1,
#if AFE_USE_CRC
                       wire, (uint8_t)(2 * n));
#else
                       dat, n);
#endif

    if (err == I2C_ERR_NACK)
    {
        return AFE_ERR_NACK;
    }

    if (err != I2C_OK)
    {
        return (err == I2C_ERR_BUS) ? AFE_ERR_BUS : AFE_ERR_IO;
    }

#if AFE_USE_CRC
    for (i = 0; i < n; i++)
    {
        uint8_t hdr[2];
        uint8_t want;

        dat[i] = wire[2 * i];

        /* 首字节 CRC 覆盖「从机地址+R / 数据」，其后各字节只覆盖自身（§8.3 图 8-4） */
        hdr[0] = rd;
        hdr[1] = dat[i];
        want = (i == 0) ? afe_crc8(hdr, 2) : afe_crc8(&dat[i], 1);

        if (want != wire[2 * i + 1])
        {
            return AFE_ERR_CRC;
        }
    }
#endif

    return AFE_OK;
}

static uint8_t afe_hal_read1(uint8_t reg, uint8_t *val)
{
    return afe_hal_read(reg, val, 1);
}

/* ===================== 启动（C-64） ===================== */

uint8_t afe_hal_boot(void)
{
    GPIO_InitTypeDef gpio;

    AFE_BOOT_GPIO_CLK_EN();

    gpio.Pin   = AFE_BOOT_GPIO_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;     /* 推挽：要能真正把 TS1 顶过 VBOOT(MAX 1000mV) */
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(AFE_BOOT_GPIO_PORT, &gpio);

    HAL_GPIO_WritePin(AFE_BOOT_GPIO_PORT, AFE_BOOT_GPIO_PIN, GPIO_PIN_RESET);
    delay_us(100);                        /* 先确认低，保证接下来是一个"上升沿"而不是电平 */

    HAL_GPIO_WritePin(AFE_BOOT_GPIO_PORT, AFE_BOOT_GPIO_PIN, GPIO_PIN_SET);
    delay_us(AFE_BOOT_PULSE_US);
    HAL_GPIO_WritePin(AFE_BOOT_GPIO_PORT, AFE_BOOT_GPIO_PIN, GPIO_PIN_RESET);

    /* ★ 交还引脚：持续驱动（哪怕低电平）都会把 TS1 拉住，污染热敏读数（§9.1.6） */
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(AFE_BOOT_GPIO_PORT, &gpio);

    delay_us(AFE_BOOT_READY_US);           /* tBOOTREADY 之后 I2C 才可用（tI2CSTARTUP=1ms） */

    return AFE_OK;
}

/* ===================== 探测 / 配置 ===================== */

uint8_t afe_hal_probe(void)
{
    /* 只发地址（len=0）。CRC 开启时地址字节不参与 CRC 计算，所以这一步不依赖 CRC 是否正确 ——
     * 正好用来把"器件根本不在位/未启动"与"CRC 口径不对"分开。 */
    uint8_t err = softi2c_write(&g_bus_afe, AFE_I2C_ADDR, 0, 0);

    return (err == I2C_OK) ? AFE_OK : ((err == I2C_ERR_NACK) ? AFE_ERR_NACK : AFE_ERR_BUS);
}

uint8_t afe_hal_setup(void)
{
    uint8_t v;
    uint8_t err;

    /* ① ADC_EN 只允许置 1（禁止清 0：那会连 OV 保护一起关掉，S4） */
    err = afe_hal_read1(REG_SYS_CTRL1, &v);

    if (err != AFE_OK)
    {
        return err;
    }

    if ((v & CTRL1_ADC_EN) == 0)
    {
        err = afe_hal_write1(REG_SYS_CTRL1, (uint8_t)(v | CTRL1_ADC_EN));

        if (err != AFE_OK)
        {
            return err;
        }
    }

    /* ② SYS_CTRL2 全部回到"断开 + 库仑计关"：S3 常态断开，同时消掉 CC_EN 引起的 ALERT 周期抖动。
     *    写整字节而不是读改写：其余位(DELAY_DIS/CC_ONESHOT)默认 0，显式写 0 更可预期。 */
    return afe_hal_write1(REG_SYS_CTRL2, 0);
}

/* ===================== 采集 ===================== */

uint8_t afe_hal_read_vc(uint16_t *code, uint8_t count)
{
    uint8_t raw[2 * AFE_CHIP_MAX_CELLS];
    uint8_t i;
    uint8_t err;

    if ((code == 0) || (count == 0) || (count > AFE_CHIP_MAX_CELLS))
    {
        return AFE_ERR_IO;
    }

    /* ★ 高低字节必须在同一事务内连读：手册明确"同一事务才保证原子"（表 8-15） */
    err = afe_hal_read(REG_VC1_HI, raw, (uint8_t)(2 * count));

    if (err != AFE_OK)
    {
        return err;
    }

    for (i = 0; i < count; i++)
    {
        /* VCx_HI 的 bit3:0 = D13:D8，VCx_LO = D7:D0 → 14 位码 */
        code[i] = (uint16_t)((((uint16_t)raw[2 * i] & 0x3Fu) << 8) | (uint16_t)raw[2 * i + 1]);
    }

    return AFE_OK;
}

uint8_t afe_hal_read_bat(uint16_t *code)
{
    uint8_t raw[2];
    uint8_t err = afe_hal_read(REG_BAT_HI, raw, 2);

    if (err != AFE_OK)
    {
        return err;
    }

    *code = (uint16_t)(((uint16_t)raw[0] << 8) | (uint16_t)raw[1]);

    return AFE_OK;
}

uint8_t afe_hal_read_scale(uint16_t *gain_uv, int16_t *offset_mv)
{
    uint8_t g1, g2, off;
    uint8_t err = afe_hal_read1(REG_ADCGAIN1, &g1);

    if (err == AFE_OK)
    {
        err = afe_hal_read1(REG_ADCGAIN2, &g2);
    }

    if (err == AFE_OK)
    {
        err = afe_hal_read1(REG_ADCOFFSET, &off);
    }

    if (err != AFE_OK)
    {
        return err;
    }

    /* GAIN = 365 µV/LSB + ADCGAIN<4:0>（表 8-21/8-22：ADCGAIN<4:3> 在 0x50 的 bit3:2，
     * ADCGAIN<2:0> 在 0x59 的 bit7:5） */
    *gain_uv = (uint16_t)(365u + (uint16_t)((((uint16_t)(g1 >> 2) & 0x03u) << 3) |
                                            ((uint16_t)g2 >> 5)));
    /* OFFSET 是 2 的补码 mV（表 8-23：0x80 = -128，0xFF = -1） */
    *offset_mv = (int16_t)((int8_t)off);

    return AFE_OK;
}

/* ===================== 硬件层状态解码（S26 的落地点） ===================== */

uint8_t afe_hal_read_hw_stat(afe_hw_t *st)
{
    uint8_t v;
    uint8_t err;

    if (st == 0)
    {
        return AFE_ERR_IO;
    }

    err = afe_hal_read1(REG_SYS_STAT, &v);

    if (err != AFE_OK)
    {
        return err;
    }

    st->ov       = (v & STAT_OV) ? 1 : 0;
    st->uv       = (v & STAT_UV) ? 1 : 0;
    st->ocd      = (v & STAT_OCD) ? 1 : 0;
    st->scd      = (v & STAT_SCD) ? 1 : 0;
    st->cc_ready = (v & STAT_CC_READY) ? 1 : 0;

    /* ★ 写 1 清位：把刚读到的位原样写回去，只清这些位，别的位写 0 无效（表 8-14）。
     *   不清的话 ALERT 会一直挂着，下一次上升沿就再也等不到了。 */
    if (v != 0)
    {
        err = afe_hal_write1(REG_SYS_STAT, v);

        if (err != AFE_OK)
        {
            return err;
        }
    }

    return AFE_OK;
}

/* ===================== 均衡 / 通道使能 ===================== */

uint8_t afe_hal_read_bal(uint8_t *mask)
{
    uint8_t v;
    uint8_t err = afe_hal_read1(REG_CELLBAL1, &v);

    if (err != AFE_OK)
    {
        return err;
    }

    *mask = (uint8_t)(v & 0x1Fu);     /* bit4:0 = CB5..CB1，高 3 位 RSVD */

    return AFE_OK;
}

uint8_t afe_hal_write_block(uint8_t reg, const uint8_t *dat, uint8_t n)
{
    return afe_hal_write(reg, dat, n);
}

uint8_t afe_hal_read_block(uint8_t reg, uint8_t *dat, uint8_t n)
{
    return afe_hal_read(reg, dat, n);
}

uint8_t afe_hal_write_bal(uint8_t mask)
{
    uint8_t v;
    uint8_t err = afe_hal_read1(REG_CELLBAL1, &v);

    if (err != AFE_OK)
    {
        return err;
    }

    /* 读改写：RSVD 位保持原值（§8.5 未定义位不得擅动） */
    return afe_hal_write1(REG_CELLBAL1, (uint8_t)((v & (uint8_t)~0x1Fu) | (mask & 0x1Fu)));
}

uint8_t afe_hal_set_channel(uint8_t chg_on, uint8_t dsg_on)
{
    uint8_t v = 0;

    if (chg_on)
    {
        v |= CTRL2_CHG_ON;
    }

    if (dsg_on)
    {
        v |= CTRL2_DSG_ON;
    }

    /* ⛔ 这里**不**读改写：SYS_CTRL2 的其余位（DELAY_DIS/CC_EN/CC_ONESHOT）由 setup() 归零，
     *   且本函数是唯一写这寄存器的路径，显式写出来比"保持上一次值"更容易审计。 */
    return afe_hal_write1(REG_SYS_CTRL2, v);
}
