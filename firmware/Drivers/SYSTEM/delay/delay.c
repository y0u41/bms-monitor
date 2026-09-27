/**
 ****************************************************************************************************
 * @file        delay.c
 * @brief       SysTick 精确延时（µs 级）
 * @note        采用"时钟摘取法"读取 SysTick 递减计数器, 不占用中断, 可与 HAL_Delay 共存。
 *              代码移植自正点原子 delay.c, 去掉了 uC/OS 支持部分。
 *              前提: HAL_Init() 已把 SysTick 配置为 1ms 周期(HCLK 不分频)。
 ****************************************************************************************************
 */

#include "./SYSTEM/delay/delay.h"

static uint32_t g_fac_us = 0;       /* us 延时倍乘数, 等于系统主频(MHz) */

/**
 * @brief     初始化延时函数
 * @param     sysclk: 系统主频(MHz), 本工程为 72
 * @retval    无
 */
void delay_init(uint16_t sysclk)
{
    g_fac_us = sysclk;              /* HAL_Init 已配置好 SysTick, 这里只记录倍乘数 */
}

/**
 * @brief     延时 nus 微秒
 * @param     nus: 要延时的微秒数
 * @retval    无
 */
void delay_us(uint32_t nus)
{
    uint32_t ticks;
    uint32_t told, tnow, tcnt = 0;
    uint32_t reload = SysTick->LOAD;        /* LOAD 的值 */

    ticks = nus * g_fac_us;                 /* 需要的节拍数 */
    told = SysTick->VAL;                    /* 刚进入时的计数器值 */

    while (1)
    {
        tnow = SysTick->VAL;

        if (tnow != told)
        {
            if (tnow < told)
            {
                tcnt += told - tnow;        /* SysTick 是递减计数器 */
            }
            else
            {
                tcnt += reload - tnow + told;
            }

            told = tnow;

            if (tcnt >= ticks)
            {
                break;
            }
        }
    }
}

/**
 * @brief     延时 nms 毫秒
 * @param     nms: 要延时的毫秒数
 * @retval    无
 */
void delay_ms(uint16_t nms)
{
    delay_us((uint32_t)nms * 1000U);
}
