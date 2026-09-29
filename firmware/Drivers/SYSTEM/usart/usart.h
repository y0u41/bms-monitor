/**
 ****************************************************************************************************
 * @file        usart.h
 * @brief       调试串口(USART1)头文件
 * @note        串口外设本身由 CubeMX 生成的 usart.c 初始化; 本文件只提供 printf 重定向声明。
 ****************************************************************************************************
 */

#ifndef __USART_H
#define __USART_H

#include "./SYSTEM/sys/sys.h"
#include <stdio.h>

/* ★★ 2026-09-30（v0.65/v0.66）：打印路径的对外出口（⛔ 取证用，不参与判据）
 *   · `bootlog` = **上电段打印的原文**（预调度期逐字节抓进 RAM）⇒ 真冷启动抓不到开机行时，
 *     可以用串口命令 `?` **在任意时刻回放**（`附录C` C-144 的还清形式）。 */
uint16_t usart_retarget_tx_timeouts(void);
uint8_t  usart_retarget_tx_dead(void);
uint8_t  usart_retarget_print_drops(void);
uint8_t  usart_retarget_print_splits(void);
uint8_t  usart_retarget_print_ctx_full(void);
void     usart_retarget_bootlog_replay(void);
uint16_t usart_retarget_bootlog_len(void);
uint8_t  usart_retarget_bootlog_overflow(void);

#endif
