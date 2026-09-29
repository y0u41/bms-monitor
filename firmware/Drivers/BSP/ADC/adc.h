/**
 ****************************************************************************************************
 * @file        adc.h
 * @brief       ADC1 轮询驱动（本工程用它采两路 NTC：PA5 = ADC1_IN5、PA6 = ADC1_IN6）
 * @note        1, ★ 为什么是**轮询**而不是 DMA：两路、100ms 一拍、每次两个数 ⇒ 轮询足够，
 *                  而 DMA 会多出一条"搬运错误"的失效路径（本项目 `AGENTS.md` §2.3 的取舍）。
 *              2, ★ `stime = 7`（239.5 周期）是 `Tech_Design.md` §3.1.1 对 NTC 通道指定的采样时间 ——
 *                  NTC 分压并了 100nF，采样时间短了会读到"上一个通道的残压"。
 *              3, ⛔ 本文件不换算温度（那是 `ntc.c` 的事）、⛔ 不判告警。
 *              4, ★ 超时用**循环计数**而不是 `HAL_GetTick()`：本驱动要能在**调度器之前**跑
 *                  （启动自检）—— `HAL_GetTick()` 在启动期可能是冻住的（`附录C` C-107 的教训）。
 ****************************************************************************************************
 */

#ifndef __ADC_H
#define __ADC_H

#include "./SYSTEM/sys/sys.h"

#define ADC_CH_NTC1     5u      /* PA5 = ADC1_IN5（电芯表面 NTC #1） */
#define ADC_CH_NTC2     6u      /* PA6 = ADC1_IN6（电芯表面 NTC #2） */

#define ADC_CODE_INVALID    0xFFFFu     /* 转换超时/入参非法 —— ★ 与"读到 0"是两件事 */
#define ADC_STIME_NTC       7u          /* 239.5 周期（§3.1.1 指定） */

void        adc_init(void);

/**
 * @brief       读一个通道（轮询，有界等待）
 * @param       ch  ADC 通道号（0~17）
 * @retval      **12 位码**（0~4095）；★ 超时或入参非法回 `ADC_CODE_INVALID`。
 *              ⛔ 调用方必须区分"`ADC_CODE_INVALID`"与"0"：0 是**合法读数**（可能真短路）。
 */
uint16_t    adc_read_ch(uint8_t ch);

/** @brief ★ 自检（⛔ 不需要外部器件）：转换能完成且落在 12 位内 ⇒ 外设活着 */
uint16_t    adc_selftest(void);

/** 取证出口（⛔ 不参与判据） */
uint16_t    adc_selftest_result(void);

#endif
