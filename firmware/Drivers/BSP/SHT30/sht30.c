/**
 ****************************************************************************************************
 * @file        sht30.c
 * @brief       SHT30 / SHT31 温湿度传感器驱动（调 `g_bus_sens` 软件 I2C, PB6/PB7）
 * @note        移植来源 / 改动点 / 验证方式见 `sht30.h` 文件头（AGENTS §3 G5）。
 *              本文件里 ⛔ 不得出现位操作与 `delay_ms`：时序归 `SOFTI2C`，节拍归调用方。
 ****************************************************************************************************
 */

#include "./BSP/SHT30/sht30.h"
#include "./BSP/SOFTI2C/softi2c.h"
#include <stddef.h>

/* ===================== SHT30 命令 ===================== */
#define SHT30_CMD_MEAS_HIGH     0x2400      /* 单次测量, 高重复性, 不使能时钟拉伸 */

/* ===================== 测量流水（两阶段, S12） ===================== */
static uint8_t  s_pending;                  /* 1 = 命令已发出，正在等器件转换完成 */
static uint32_t s_trig_tick;

/**
 * @brief       向 SHT30 写一条 16 位命令（一次独立事务：S addr+W cmd_hi cmd_lo P）
 * @retval      SHT30_OK / SHT30_ERR_NACK
 */
static uint8_t sht30_write_cmd(uint16_t cmd)
{
    uint8_t buf[2];

    buf[0] = (uint8_t)(cmd >> 8);
    buf[1] = (uint8_t)(cmd & 0xFF);

    return (softi2c_write(&g_bus_sens, SHT30_I2C_ADDR, buf, 2) == I2C_OK) ? SHT30_OK : SHT30_ERR_NACK;
}

/**
 * @brief       纯读 6 字节原始数据（S addr+R [6] P，没有写阶段）
 * @param       buf: 温度MSB/温度LSB/温度CRC/湿度MSB/湿度LSB/湿度CRC
 * @retval      SHT30_OK / SHT30_ERR_NACK
 */
static uint8_t sht30_read_raw(uint8_t *buf)
{
    return (softi2c_read_only(&g_bus_sens, SHT30_I2C_ADDR, buf, 6) == I2C_OK) ? SHT30_OK : SHT30_ERR_NACK;
}

/**
 * @brief       SHT30 数据 CRC-8 校验
 * @note        多项式 0x31, 初值 0xFF, 无反转
 */
static uint8_t sht30_crc8(const uint8_t *dat, uint8_t len)
{
    uint8_t crc = 0xFF;
    uint8_t i, b;

    for (i = 0; i < len; i++)
    {
        crc ^= dat[i];

        for (b = 0; b < 8; b++)
        {
            if (crc & 0x80)
            {
                crc = (uint8_t)((crc << 1) ^ 0x31);
            }
            else
            {
                crc = (uint8_t)(crc << 1);
            }
        }
    }

    return crc;
}

/**
 * @brief       探测器件在位（只发地址；不初始化总线、不复位、不清计数）
 * @retval      SHT30_OK / SHT30_ERR_NACK
 */
uint8_t sht30_probe(void)
{
    /* len=0 的纯地址写：SHT30 在位就一定回 ACK */
    return (softi2c_write(&g_bus_sens, SHT30_I2C_ADDR, 0, 0) == I2C_OK) ? SHT30_OK : SHT30_ERR_NACK;
}

/**
 * @brief       读温湿度（两阶段、非阻塞）
 * @retval      SHT30_OK / SHT30_BUSY / SHT30_ERR_NACK / SHT30_ERR_CRC / SHT30_ERR_PARAM
 */
uint8_t sht30_measure(int16_t *temp_x10, uint16_t *humi_x10)
{
    uint8_t  buf[6];
    uint16_t raw_t, raw_h;
    uint8_t  err;

    if ((temp_x10 == NULL) || (humi_x10 == NULL))
    {
        return SHT30_ERR_PARAM;
    }

    if (!s_pending)
    {
        /* 第①拍：只发命令，不等结果 —— 器件最大转换 15ms，忙等它就是在任务里睡 20ms（S12） */
        if (sht30_write_cmd(SHT30_CMD_MEAS_HIGH) != SHT30_OK)
        {
            return SHT30_ERR_NACK;
        }

        s_pending = 1;
        s_trig_tick = HAL_GetTick();

        return SHT30_BUSY;
    }

    /* 无符号相减：HAL_GetTick 绕回时依然正确 */
    if ((HAL_GetTick() - s_trig_tick) < SHT30_MEAS_WAIT_MS)
    {
        return SHT30_BUSY;
    }

    s_pending = 0;

    err = sht30_read_raw(buf);

    if (err != SHT30_OK)
    {
        return err;
    }

    if ((sht30_crc8(buf, 2) != buf[2]) || (sht30_crc8(buf + 3, 2) != buf[5]))
    {
        return SHT30_ERR_CRC;
    }

    raw_t = (uint16_t)(((uint16_t)buf[0] << 8) | buf[1]);
    raw_h = (uint16_t)(((uint16_t)buf[3] << 8) | buf[4]);

    /* 官方换算: T = -45 + 175 * raw/65535 ; RH = 100 * raw/65535
     * 换成 0.1 单位并用整数运算, 避免引入浮点库:
     *   T_x10  = -450 + 1750 * raw_t / 65535
     *   RH_x10 =        1000 * raw_h / 65535
     */
    *temp_x10 = (int16_t)(-450 + (int32_t)(1750L * raw_t) / 65535L);
    *humi_x10 = (uint16_t)((int32_t)(1000L * raw_h) / 65535L);

    /* ★ 取读与下一次触发在同一次调用里串好：稳态下每拍都返回一帧 20ms 内的新数据，
     *   而不是"隔拍才有数"（那会让有效采样率直接减半）。 */
    if (sht30_write_cmd(SHT30_CMD_MEAS_HIGH) == SHT30_OK)
    {
        s_pending = 1;
        s_trig_tick = HAL_GetTick();
    }

    return SHT30_OK;
}
