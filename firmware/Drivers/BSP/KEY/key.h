/**
 ****************************************************************************************************
 * @file        key.h
 * @brief       按键驱动 (板载 KEY0=PE4, KEY1=PE3, 按下为低电平)
 * @note        KEY0 在主页 = 报警消音; KEY1 在主页 = 进入报警历史页(见 app_main.c)。
 ****************************************************************************************************
 */

#ifndef __KEY_H
#define __KEY_H

#include "./SYSTEM/sys/sys.h"

#define KEY0_GPIO_PORT          GPIOE
#define KEY0_GPIO_PIN           GPIO_PIN_4
#define KEY1_GPIO_PORT          GPIOE
#define KEY1_GPIO_PIN           GPIO_PIN_3
#define KEY_GPIO_CLK_ENABLE()   do{ __HAL_RCC_GPIOE_CLK_ENABLE(); }while(0)

#define KEY0_READ()             HAL_GPIO_ReadPin(KEY0_GPIO_PORT, KEY0_GPIO_PIN)
#define KEY1_READ()             HAL_GPIO_ReadPin(KEY1_GPIO_PORT, KEY1_GPIO_PIN)

#define KEY0_PRES               0x01        /* KEY0 刚被按下 */
#define KEY1_PRES               0x02        /* KEY1 刚被按下 */

void    key_init(void);
uint8_t key_scan(void);     /* 返回 KEY0_PRES / KEY1_PRES 的按位或; 无按键返回 0 (带 10ms 消抖) */

#endif
