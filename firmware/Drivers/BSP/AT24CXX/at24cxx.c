/**
 ****************************************************************************************************
 * @file        at24cxx.c
 * @brief       AT24C02（256B EEPROM）驱动，挂 `g_bus_sens`（PB6/PB7）
 * @note        移植来源 / 改动点 / 验证方式见 `at24cxx.h` 文件头（AGENTS §3 G5）。
 *              本文件里 ⛔ 不得出现位操作：时序全部归 `Drivers/BSP/SOFTI2C/`。
 ****************************************************************************************************
 */

#include "./BSP/AT24CXX/at24cxx.h"
#include "./BSP/SOFTI2C/softi2c.h"
#include "./SYSTEM/delay/delay.h"

/* 自测写入的图形：与"全 0/全 FF"两种空片状态都可区分 */
#define AT24C_SELFTEST_PAT      0xA5u

/**
 * @brief       探测器件在位（只发地址，len=0 → 不写任何东西）
 * @retval      AT24C_OK / AT24C_ERR_NACK
 */
uint8_t at24cxx_probe(void)
{
    return (softi2c_write(&g_bus_sens, AT24C02_I2C_ADDR, 0, 0) == I2C_OK) ? AT24C_OK : AT24C_ERR_NACK;
}

/**
 * @brief       写一个字节：S addr+W [mem_addr] [dat] P，然后等写周期结束
 * @param       addr 0~255（AT24C02 只有 256 字节，地址是一个字节）
 * @retval      AT24C_OK / AT24C_ERR_NACK / AT24C_ERR_PARAM
 */
uint8_t at24cxx_write_byte(uint16_t addr, uint8_t dat)
{
    uint8_t buf[2];

    if (addr >= AT24C02_SIZE)
    {
        return AT24C_ERR_PARAM;
    }

    buf[0] = (uint8_t)(addr & 0xFFu);
    buf[1] = dat;

    if (softi2c_write(&g_bus_sens, AT24C02_I2C_ADDR, buf, 2) != I2C_OK)
    {
        return AT24C_ERR_NACK;
    }

    /* 等 tWR（手册 max 5ms）。⛔ 不用 ACK 轮询：轮询期间的 NACK 是"器件忙"不是"链路坏"，
     * 而失败计数是按**总线**记的（C-39），一次写污染几十个计数。 */
    delay_ms(AT24C02_WR_TIME_MS);

    return AT24C_OK;
}

/**
 * @brief       随机地址读一个字节：S addr+W [mem_addr] Sr addr+R [dat] P
 * @param       addr 0~255
 * @param       dat  输出
 * @retval      AT24C_OK / AT24C_ERR_NACK / AT24C_ERR_PARAM
 */
uint8_t at24cxx_read_byte(uint16_t addr, uint8_t *dat)
{
    uint8_t mem;

    if ((dat == 0) || (addr >= AT24C02_SIZE))
    {
        return AT24C_ERR_PARAM;
    }

    mem = (uint8_t)(addr & 0xFFu);

    if (softi2c_read(&g_bus_sens, AT24C02_I2C_ADDR, &mem, 1, dat, 1) != I2C_OK)
    {
        return AT24C_ERR_NACK;
    }

    return AT24C_OK;
}

/**
 * @brief       共存自测：探测 → 写 → 回读比对
 * @retval      AT24C_OK / AT24C_ERR_NACK / AT24C_ERR_VERIFY / AT24C_ERR_PARAM
 */
uint8_t at24cxx_selftest(void)
{
    uint8_t rd = 0x00u;
    uint8_t err;

    err = at24cxx_probe();

    if (err != AT24C_OK)
    {
        return err;
    }

    err = at24cxx_write_byte(AT24C02_SCRATCH_ADDR, AT24C_SELFTEST_PAT);

    if (err != AT24C_OK)
    {
        return err;
    }

    err = at24cxx_read_byte(AT24C02_SCRATCH_ADDR, &rd);

    if (err != AT24C_OK)
    {
        return err;
    }

    return (rd == AT24C_SELFTEST_PAT) ? AT24C_OK : AT24C_ERR_VERIFY;
}
