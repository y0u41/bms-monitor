/**
 ****************************************************************************************************
 * @file        key.c
 * @brief       按键驱动 (板载 KEY0=PE4, KEY1=PE3, 按下为低电平)
 * @note        PE3/PE4 未在 CubeMX 中配置, 这里自己开时钟并配成上拉输入。
 ****************************************************************************************************
 */

#include "./BSP/KEY/key.h"
#include "./SYSTEM/delay/delay.h"

/**
 * @brief       初始化 KEY0 / KEY1(上拉输入)
 * @param       无
 * @retval      无
 */
void key_init(void)
{
    GPIO_InitTypeDef gpio_init_struct = {0};

    KEY_GPIO_CLK_ENABLE();

    gpio_init_struct.Pin   = KEY0_GPIO_PIN | KEY1_GPIO_PIN;
    gpio_init_struct.Mode  = GPIO_MODE_INPUT;
    gpio_init_struct.Pull  = GPIO_PULLUP;
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(KEY0_GPIO_PORT, &gpio_init_struct);
}

/**
 * @brief       扫描按键(非阻塞)
 * @param       无
 * @retval      KEY0_PRES / KEY1_PRES 的按位或; 0 = 无按键
 * @note        两个键各自独立消抖。返回"按位或"而不是单个键, 是为了让"报警消音"和
 *              "翻报警历史页"两件事能各自判断, 不用互相抢 key_scan() 的返回值。
 */
uint8_t key_scan(void)
{
    static uint8_t key0_up = 1;         /* KEY0 松开标志 */
    static uint8_t key1_up = 1;         /* KEY1 松开标志 */
    uint8_t        ret = 0;

    if (key0_up && (KEY0_READ() == GPIO_PIN_RESET))
    {
        delay_ms(10);                   /* 消抖 */

        key0_up = 0;

        if (KEY0_READ() == GPIO_PIN_RESET)
        {
            ret |= KEY0_PRES;
        }
    }
    else if (KEY0_READ() == GPIO_PIN_SET)
    {
        key0_up = 1;
    }

    if (key1_up && (KEY1_READ() == GPIO_PIN_RESET))
    {
        delay_ms(10);                   /* 消抖 */

        key1_up = 0;

        if (KEY1_READ() == GPIO_PIN_RESET)
        {
            ret |= KEY1_PRES;
        }
    }
    else if (KEY1_READ() == GPIO_PIN_SET)
    {
        key1_up = 1;
    }

    return ret;
}
