/**
 ****************************************************************************************************
 * @file        afe_hal.h
 * @brief       `afe.c`(通用层) 与 `afe_hal_<型号>.c`(寄存器层) 之间的内部契约
 * @note        ⛔ User/App 目录下的 .c **不得** include 本文件 —— 这里出现的是"原始 ADC 码"和型号相关量，
 *              应用层只认 `afe.h`（S15 / G6）。
 *              换 AFE 型号 = 换掉 `afe_hal_*.c` 一个文件，本头文件与 `afe.c` 不动（§4.6 决定 2）。
 ****************************************************************************************************
 */

#ifndef __AFE_HAL_H
#define __AFE_HAL_H

#include "./SYSTEM/sys/sys.h"
#include "./BSP/AFE/afe.h"

/* 型号能力（AFE_CHIP_MIN/MAX_CELLS）与串数（APP_CELL_COUNT）的唯一来源是 `app_config.h` §12；
 * 本文件只用不做定义（§5.1 唯一来源规则）。`afe.h` 已经把 app_config.h 带进来。 */

/* ===================== 原始量接口 =====================
 * 寄存器层只负责"把字节取回来 / 发出去"，不做单位换算、不做有效性判断 ——
 * 那两件事在通用层，这样换型号时不会因为换算口径不同而漂移。
 */

/**
 * @brief       启动：PE6 → 1K → TS1 发一个 >`VBOOT` 的上升沿，再等 ≥`tBOOTREADY`
 * @note        ★ C-64：bq769x0 **每次 POR 必进 SHIP 模式**，此时 ADC/比较器/I2C 全不上电。
 *              ⛔ 禁止把 boot 脚**持续**拉高（会污染热敏读数）；本函数返回后该脚必须是低且高阻。
 */
uint8_t afe_hal_boot(void);

/**
 * @brief       地址探测：只发地址看有没有 ACK
 * @retval      AFE_OK / AFE_ERR_NACK
 */
uint8_t afe_hal_probe(void);

/**
 * @brief       上电配置：确认 ADC 通道开着（⛔ 本函数**只允许**打开，禁止关闭任何保护）、
 *              关闭库仑计（本版电流走 INA226）、把充/放通道清 0（S3 常态断开）
 */
uint8_t afe_hal_setup(void);

/**
 * @brief       连读 `count` 串单体电压的 14 位 ADC 码
 * @param       code  输出，每个元素 0~0x3FFF
 * @note        ★ 高低字节必须在**同一事务**里靠地址自增连读，否则读到"高字节旧/低字节新"的撕裂值
 */
uint8_t afe_hal_read_vc(uint16_t *code, uint8_t count);

/**
 * @brief       读组电压寄存器（16 位原始码，= Σ单体/4，**不是**独立测量）
 */
uint8_t afe_hal_read_bat(uint16_t *code);

/**
 * @brief       读出厂标定的 ADC 换算系数
 * @param       gain_uv    µV/LSB，365~396（= 365 + `ADCGAIN<4:0>`，§8.3.1.1 表 8-21/8-22）
 * @param       offset_mv  mV，2 的补码，-128~127（表 8-23）
 * @note        换算式（§8.3.1.1）：`V(cell) = code*GAIN + OFFSET`；`V(BAT) = 4*GAIN*code + N*OFFSET`
 */
uint8_t afe_hal_read_scale(uint16_t *gain_uv, int16_t *offset_mv);

/**
 * @brief       读 `SYS_STAT` 并**写 1 清位**，返回解码结果（S26 的落地点）
 */
uint8_t afe_hal_read_hw_stat(afe_hw_t *st);

uint8_t afe_hal_read_bal(uint8_t *mask);
uint8_t afe_hal_write_bal(uint8_t mask);

/**
 * @brief       写充/放通道使能位（0=断开）
 */
uint8_t afe_hal_set_channel(uint8_t chg_on, uint8_t dsg_on);

#endif
