/**
 ****************************************************************************************************
 * @file        lsens.h
 * @brief       光敏传感器驱动头文件 (板载 LS1, PF8 = ADC3_IN6)
 ****************************************************************************************************
 */

#ifndef __LSENS_H
#define __LSENS_H

#include "./SYSTEM/sys/sys.h"

/* 若实测发现"光照越强数值越小", 把这里改成 1 即可翻转 */
#define LSENS_INVERT        0

#define LSENS_ADC_CH        ADC_CHANNEL_6       /* ADC3 通道 6 -> PF8 */
#define LSENS_ADC_SAMPLES   10                  /* 每次读取的采样次数 */
#define LSENS_ADC_POLL_MS   2                   /* 单次转换的等待上限：F1 的 ADC 转换 ≈0.2µs，
                                                 *  2ms 已是 4 个数量级余量。⛔ 不得回填成 10ms ——
                                                 *  10 次 × 10ms = 100ms 忙等，正好落在迁移容器
                                                 *  80ms 的判活窗口里（2026-09-27 实机同轮，
                                                 *  见 esp8266.c 文件头注 ②）。 */

void    lsens_init(void);
uint8_t lsens_get_val(void);                    /* 返回 0~100, 0=最暗, 100=最亮 */

#endif
