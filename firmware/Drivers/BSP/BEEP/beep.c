/**
 ****************************************************************************************************
 * @file        beep.c
 * @brief       蜂鸣器驱动 (板载 BEEP, PB8, 高电平鸣叫)
 * @note        PB8 未在 CubeMX 中配置(它是板载有源蜂鸣器, 不参与外设复用),
 *              所以这里自己开时钟并配成推挽输出, 与 LCD/SHT30 的做法一致。
 ****************************************************************************************************
 */

#include "./BSP/BEEP/beep.h"

/**
 * @brief       初始化蜂鸣器(推挽输出, 默认不响)
 * @param       无
 * @retval      无
 */
void beep_init(void)
{
    GPIO_InitTypeDef gpio_init_struct = {0};

    BEEP_GPIO_CLK_ENABLE();

    gpio_init_struct.Pin = BEEP_GPIO_PIN;
    gpio_init_struct.Mode = GPIO_MODE_OUTPUT_PP;
    gpio_init_struct.Pull = GPIO_NOPULL;
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(BEEP_GPIO_PORT, &gpio_init_struct);

    BEEP(0);
}
