/**
 ****************************************************************************************************
 * @file        at24cxx.h
 * @brief       AT24C02（2Kbit / 256B EEPROM）驱动，挂在 `g_bus_sens` = PB6/PB7 上
 * @note        1, ★ 本驱动的**首要用途是 D3 的"同总线多从机共存"验证载体**（`Build.md` D3 判据
 *                  "同一总线上 24C02 仍可读写"）：BMS 的参数区与事件日志一律走**内部 Flash**
 *                  （`app_config.h` §13 / `Tech_Design.md` §8），⛔ 不是 24C02。
 *                  PRD 里没有"用 24C02 存东西"的功能项 —— 不要把它当成存储层往上加接口。
 *              2, 位带在 `Drivers/BSP/SOFTI2C/` 的 `g_bus_sens` 实例里（C-39 单实现·双实例）；
 *                  ⛔ 不得改调 `g_bus_afe`（S6：AFE 独占总线）。
 *              3, 器件口径：AT24C02 = 256 字节、**页 8 字节**、器件地址 0xA0/0xA1（7 位 0x50），
 *                  写周期 `tWR` 最大 5ms。本驱动只做**单字节写 + 单字节读**：页写与顺序读
 *                  在"共存验证"里用不到，⛔ 不要为了"以后可能要"提前实现（要用时再来加）。
 *              4, ★ `at24cxx_write_byte()` 内部是 `delay_ms(6)`（> 手册 tWR 5ms）而不是 ACK 轮询：
 *                  轮询会把"器件忙时 NACK"记进**整条总线**的失败计数（C-39 的账本就按总线记），
 *                  一次写污染 50 次计数，等于自己把传感总线的链路健康判据搞坏。
 *                  ⚠️ 6ms 忙等只在启动自测里跑；将来若要在任务里连写，改两阶段（写→下一拍读校验）。
 * @note        移植来源：本项目自写。**⛔ 两个工程树里都没有现成的 24C02 驱动可移植**
 *                  （`firmware/Drivers/BSP/` 无 AT24CXX；旧工程 `atk_f103/Drivers/BSP/` 亦无），
 *                  所以这不是移植而是新写 —— 时序全部交给 `softi2c`，本文件只有协议层。
 *              改动点：不适用（新建）。
 *              验证方式：D3 台架 `at24cxx_selftest()` 上电实跑 + 逻辑分析仪抓波形。
 *                  ⚠️ 截至本次落盘**未上机**（硬件在路上）。
 ****************************************************************************************************
 */

#ifndef __AT24CXX_H
#define __AT24CXX_H

#include "./SYSTEM/sys/sys.h"

/* ===================== 器件地址（7 位，不含 R/W） ===================== */
#define AT24C02_I2C_ADDR        0x50

/* ===================== 容量与写周期 ===================== */
#define AT24C02_SIZE            256u
#define AT24C02_PAGE_SIZE       8u              /* 仅作口径记录（超页写会回卷），本驱动不做页写 */
#define AT24C02_WR_TIME_MS      6u              /* 手册 tWR max 5ms，多留 1ms */

/* ===================== 自测用的临时单元 ===================== */
/* 选**最后一个字节**（0xFF）：本项目不拿 24C02 存业务数据，落在末尾可把"万一以后存了东西"
 * 的破坏面压到最小；自测写完不恢复原值（理由同上，且该位置本就没有约定内容）。 */
#define AT24C02_SCRATCH_ADDR    0xFFu

/* ===================== 返回码 ===================== */
#define AT24C_OK                0
#define AT24C_ERR_NACK          1       /* 器件无应答（没接 / 地址错 / 总线被卡） */
#define AT24C_ERR_PARAM         2
#define AT24C_ERR_VERIFY        3       /* 写回读不一致 */

/**
 * @brief       探测器件是否在位（只发地址；不初始化总线、不清失败计数）
 * @retval      AT24C_OK / AT24C_ERR_NACK
 */
uint8_t at24cxx_probe(void);

/**
 * @brief       写一个字节（内部等 tWR）
 * @param       addr 0~255
 * @retval      AT24C_OK / AT24C_ERR_NACK / AT24C_ERR_PARAM
 */
uint8_t at24cxx_write_byte(uint16_t addr, uint8_t dat);

/**
 * @brief       读一个字节（随机地址读：S addr+W reg Sr addr+R dat P）
 * @param       addr 0~255
 * @retval      AT24C_OK / AT24C_ERR_NACK / AT24C_ERR_PARAM
 */
uint8_t at24cxx_read_byte(uint16_t addr, uint8_t *dat);

/**
 * @brief       共存自测：探测 → 写 0xA5 → 回读比对
 * @retval      AT24C_OK / AT24C_ERR_*
 * @note        D3 判据"同一总线上 24C02 仍可读写"的实机出口；与 SHT30 同线，
 *              两者各自的 NACK 只记在 `g_bus_sens` 上，不影响 AFE 链路判据（C-39）。
 */
uint8_t at24cxx_selftest(void);

#endif
