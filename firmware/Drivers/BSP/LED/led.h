/**
 ****************************************************************************************************
 * @file        led.h
 * @brief       LED 驱动头文件 (LED0 -> PB5, LED1 -> PE5, 低电平点亮)
 ****************************************************************************************************
 */

#ifndef __LED_H
#define __LED_H

#include "./SYSTEM/sys/sys.h"

/* LED0 / LED1 控制宏: x=0 点亮, x=1 熄灭 */
#define LED0(x)     do{ (x) ? HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_SET) : \
                             HAL_GPIO_WritePin(LED0_GPIO_Port, LED0_Pin, GPIO_PIN_RESET); }while(0)
#define LED1(x)     do{ (x) ? HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_SET) : \
                             HAL_GPIO_WritePin(LED1_GPIO_Port, LED1_Pin, GPIO_PIN_RESET); }while(0)

#define LED0_TOGGLE()   do{ HAL_GPIO_TogglePin(LED0_GPIO_Port, LED0_Pin); }while(0)
#define LED1_TOGGLE()   do{ HAL_GPIO_TogglePin(LED1_GPIO_Port, LED1_Pin); }while(0)

void led_init(void);

#endif
