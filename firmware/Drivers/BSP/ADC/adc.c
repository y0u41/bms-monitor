/**
 ****************************************************************************************************
 * @file        adc.c
 * @brief       ADC1 轮询驱动（规格见 `adc.h`）
 ****************************************************************************************************
 */

#include "./BSP/ADC/adc.h"
#include "./SYSTEM/delay/delay.h"

static ADC_HandleTypeDef s_adc;
static uint16_t          s_selftest_rc = 0xFFFFu;

/** 转换等待上限（循环计数，⛔ 不依赖 tick） */
#define ADC_WAIT_MAX    100000u

void adc_init(void)
{
    GPIO_InitTypeDef gpio;

    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA5 / PA6：模拟输入（⛔ 不上拉/下拉 —— 模拟通道上下拉会拖偏分压） */
    gpio.Pin  = GPIO_PIN_5 | GPIO_PIN_6;
    gpio.Mode = GPIO_MODE_ANALOG;
    gpio.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &gpio);

    s_adc.Instance                   = ADC1;
    s_adc.Init.DataAlign             = ADC_DATAALIGN_RIGHT;     /* 右对齐 ⇒ 直接读 12 位 */
    s_adc.Init.ScanConvMode          = ADC_SCAN_DISABLE;        /* 单通道：转换前手动选道 */
    s_adc.Init.ContinuousConvMode    = DISABLE;                 /* 单次：每次读都重新起 */
    s_adc.Init.DiscontinuousConvMode = DISABLE;
    s_adc.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    s_adc.Init.NbrOfConversion       = 1;
    (void)HAL_ADC_Init(&s_adc);

    /* ★ 校准一次（F1 的 ADC 必须先校准，否则前几个数会偏） */
    (void)HAL_ADCEx_Calibration_Start(&s_adc);
}

uint16_t adc_read_ch(uint8_t ch)
{
    ADC_ChannelConfTypeDef cfg;
    uint32_t waited;

    if (ch > 17u) { return ADC_CODE_INVALID; }

    cfg.Channel      = ch;
    cfg.Rank         = ADC_REGULAR_RANK_1;
    cfg.SamplingTime = (uint32_t)((ADC_STIME_NTC == 7u) ? ADC_SAMPLETIME_239CYCLES_5
                                                        : ADC_SAMPLETIME_239CYCLES_5);
    (void)HAL_ADC_ConfigChannel(&s_adc, &cfg);

    if (HAL_ADC_Start(&s_adc) != HAL_OK) { return ADC_CODE_INVALID; }

    /* ★ 有界等待（循环计数 ⇒ 启动期也能用，见 `adc.h` 注 4） */
    for (waited = 0u; waited < ADC_WAIT_MAX; waited++)
    {
        if (HAL_ADC_PollForConversion(&s_adc, 0u) == HAL_OK)
        {
            uint32_t v = HAL_ADC_GetValue(&s_adc);

            (void)HAL_ADC_Stop(&s_adc);

            return (uint16_t)(v & 0x0FFFu);
        }
    }

    (void)HAL_ADC_Stop(&s_adc);

    return ADC_CODE_INVALID;            /* ⛔ 超时 ≠ 0 */
}

uint16_t adc_selftest_result(void)
{
    return s_selftest_rc;
}

uint16_t adc_selftest(void)
{
    uint16_t a;
    uint16_t b;

    /* 用例 1：两路都能完成转换（⛔ 不判读数是否合理 —— 悬空/短路的读数本来就可能是任何值； */
    /*         这里判的是"外设活着"，合理性判定在 `ntc.c` 的量程判据里） */
    a = adc_read_ch(ADC_CH_NTC1);
    b = adc_read_ch(ADC_CH_NTC2);

    if ((a == ADC_CODE_INVALID) || (b == ADC_CODE_INVALID)) { s_selftest_rc = 1u; return 1u; }

    /* 用例 2：连续读同一个通道两次，两次都必须是合法 12 位码（抓"只成功一次"的假活） */
    a = adc_read_ch(ADC_CH_NTC1);
    if (a > 4095u) { s_selftest_rc = 2u; return 2u; }

    s_selftest_rc = 0u;
    return 0u;
}
