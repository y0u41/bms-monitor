/**
 ****************************************************************************************************
 * @file        delay.h
 * @brief       SysTick 精确延时（µs 级）头文件
 ****************************************************************************************************
 */

#ifndef __DELAY_H
#define __DELAY_H

#include "./SYSTEM/sys/sys.h"

void delay_init(uint16_t sysclk);   /* 初始化延时函数, sysclk = 系统主频(MHz), 本工程传 72 */
void delay_us(uint32_t nus);        /* 延时 nus 微秒 */
void delay_ms(uint16_t nms);        /* 延时 nms 毫秒 */

#endif
