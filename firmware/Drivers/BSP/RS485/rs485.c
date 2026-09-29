/**
 ****************************************************************************************************
 * @file        rs485.c
 * @brief       板载 RS485 物理层实现（USART2 + PD7；IDLE+DMA 收 / DMA 发 / TC 释放 DE）
 * @note        移植来源 / 改动点 / 验证方式 / **DMA 通道号的两条出处** —— 全部见 `rs485.h` 文件头（AGENTS §3 G5）。
 *              本文件里 ⛔ 不得出现 CRC、功能码、寄存器地址：协议层在 `User/App/app_modbus.c`。
 ****************************************************************************************************
 */

#include "./BSP/RS485/rs485.h"
#include "./App/app_config.h"           /* APP_MB_ADU_MAX / APP_485_DE_SETTLE_US */
#include "./SYSTEM/delay/delay.h"       /* delay_us：DE 建立时间；delay_ms：自检的有界等待 */
#include <string.h>

/* 发送"卡住"的兜底上限（ms）：正常一帧 256 字节 @9600 也要 267ms ⇒ 取 400ms。
 * ⚠️ 它不是超时判据，而是**恢复手段**：TC 中断只要丢一次，DE 就会永久停在发送态，
 *    现象 = "从机再也不应答"（而 RX 侧一切正常）。⛔ 不得因为"从没卡过"就删掉它。 */
#define RS485_TX_GUARD_MS       400u
/* 自检的有界等待（2 字节 @19200 ≈ 1.1ms，20ms 已是 18 倍余量） */
#define RS485_SELFTEST_WAIT_MS  20u
/* 自检用的两个字节：`0x55/0xAA` 的位图一正一反，任何"位被吞/被移位"的错都能看出来（⛔ 不是 0x00） */
#define RS485_SELFTEST_PAT_0    0x55u
#define RS485_SELFTEST_PAT_1    0xAAu

/* ===================== 模块状态（全静态，⛔ 无 malloc —— G7-②） ===================== */
static UART_HandleTypeDef s_uart;
static DMA_HandleTypeDef  s_dma_rx;
static DMA_HandleTypeDef  s_dma_tx;

static uint8_t  s_rx_dma[RS485_RX_DMA_SIZE];            /* DMA 落点（ISR 只读它） */
static uint8_t  s_rx_frame[APP_MB_ADU_MAX];             /* 最近一帧（按 IDLE 事件整块拷过来） */
static volatile uint16_t s_rx_len;
static volatile uint8_t  s_rx_ready;                    /* 1 = 有一帧待取 */

static uint8_t  s_tx_buf[APP_MB_ADU_MAX];
static volatile uint8_t  s_tx_busy;
static volatile uint8_t  s_tx_done;                     /* TC 事件锁存（自检等它） */
static volatile uint8_t s_tx_proof;  /* ★ 必须 volatile：只写不读的静态量会被编译器
                                      *   **整个优化掉**（本工程实测发生过：map 里找不到符号）⇒
                                      *   而"消失"与"没写"在板现场同形。 */         /* ★ 自检走的是哪一支：0=未判 / 1=中断通路 / 2=硬件寄存器 */
static volatile uint32_t s_tx_start_ms;                 /* 兜底计时（只有任务上下文读它） */

static volatile uint32_t s_rx_frames;
static volatile uint32_t s_rx_overrun;
static volatile uint32_t s_tx_errors;
static volatile uint16_t s_last_rx_len;

/* ===================== DE 方向控制（★ 全工程只有本文件写 PD7） ===================== */
#define RS485_DE_TO_TX()   HAL_GPIO_WritePin(RS485_DE_GPIO_PORT, RS485_DE_PIN, GPIO_PIN_SET)
#define RS485_DE_TO_RX()   HAL_GPIO_WritePin(RS485_DE_GPIO_PORT, RS485_DE_PIN, GPIO_PIN_RESET)

/* ===================== HAL 回调（⛔ 一个 FreeRTOS API 都不出现，见 `rs485.h` 第 4 条） ===================== */

/**
 * @brief       RX 事件（IDLE / HT / TC 三种都会进来）
 * @param       Size  HAL 给出的"本次事件对应的字节数"；IDLE 事件下它就是**本帧总长**
 * @note        ★ 只认 IDLE：**HT 与 TC 不是帧界**。
 *              用 DMA 正常模式接收时，缓冲半满（HT）与写满（TC）都会触发本回调 ——
 *              若把它们也当帧界，一个 >128 字节的 0x10 请求会被从中间切开（现象 = 偶发 CRC 错，
 *              而且只在长帧上出现，最难查的一类）。IDLE 才是"线路静默"这个物理量。
 * @note        ⚠️ 重新武装必须在**本回调里**做（HAL 在 IDLE 事件里已经把 DMA 收尾了）；
 *              下一拍再武装的代价是"两帧之间必丢一帧"。
 */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    if ((huart->Instance != RS485_UART_INSTANCE) || (huart->RxEventType != HAL_UART_RXEVENT_IDLE))
    {
        return;
    }

    if (Size == 0u)
    {
        /* 只有 IDLE、一个字节都没收到（线被拉低又恢复）—— 不是帧，直接重新武装 */
    }
    else if ((Size > APP_MB_ADU_MAX) || (s_rx_ready != 0u))
    {
        /* 比 ADU 上限还长 ⇒ 不可能是合法的 Modbus 帧；或者上一帧还没被取走 ⇒ 丢新的。
         * ⛔ 不截断、不排队：截断的帧有极小概率"看起来合法"，那比丢帧更坏。 */
        s_rx_overrun++;
    }
    else
    {
        memcpy(s_rx_frame, s_rx_dma, (size_t)Size);
        s_last_rx_len = Size;
        s_rx_len      = Size;
        s_rx_ready    = 1u;
        s_rx_frames++;
    }

    (void)HAL_UARTEx_ReceiveToIdle_DMA(&s_uart, s_rx_dma, (uint16_t)RS485_RX_DMA_SIZE);
}

/**
 * @brief       ★★ **全工程唯一释放 DE 的地方**（`Tech_Design.md` §4.3 注②："DE/RE 用 TC 中断释放"）
 * @note        `HAL_UART_Transmit_DMA()` 返回时最后一个字节还在**移位寄存器**里；
 *              只有 TC（Transmit Complete）才代表"停止位已经上过线"。
 *              ⛔ 早放的后果 = 最后一字节的停止位被切掉 ⇒ 主机偶发 CRC 错，
 *              而现象随帧长/波特率漂移，靠读代码几乎不可能想到。
 */
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance != RS485_UART_INSTANCE)
    {
        return;
    }

    RS485_DE_TO_RX();
    s_tx_busy = 0u;
    s_tx_done = 1u;
}

/**
 * @brief       通信错误回调（ORE/FE/NE/PE 等）
 * @note        ★ 本模块**不定义** `HAL_UART_ErrorCallback` —— 它已经被 `esp8266.c` 定义了
 *              （两个定义会直接链接冲突），而那一份是**按实例守卫**的（只处理 USART3）。
 *              ⇒ USART2 的错误恢复由本模块在**任务上下文**里做（`rs485_recover()`，每拍一次）。
 *              这正是"驱动里不做兜底、兜底放在唯一能看清全局的那一层"的同一种思路：
 *              错误恢复要读的标志位与状态机，在 ISR 里反而看不清。
 */
static void rs485_recover(void)
{
    uint32_t elapsed;

    /* ① 发送卡死：TC 丢一次就会永久停在发送态 ⇒ 强制放回接收并计数 */
    if (s_tx_busy != 0u)
    {
        elapsed = (uint32_t)(HAL_GetTick() - s_tx_start_ms);

        if (elapsed > RS485_TX_GUARD_MS)
        {
            RS485_DE_TO_RX();
            s_tx_busy = 0u;
            s_tx_errors++;
        }
    }

    /* ② 接收没挂着：HAL 在错误路径里会把 RxState 置回 READY 而**不会**自动重挂
     *    （错误回调按实例守卫、不认 USART2）⇒ 这里补挂，否则"一次噪声 = 永久收不到"。
     *    ⚠️ 只在**没有**待取帧时补挂：待取帧还在的时候重挂会把它直接冲掉。 */
    if ((s_rx_ready == 0u) && (s_uart.RxState != HAL_UART_STATE_BUSY_RX))
    {
        __HAL_UART_CLEAR_OREFLAG(&s_uart);
        __HAL_UART_CLEAR_FEFLAG(&s_uart);
        __HAL_UART_CLEAR_NEFLAG(&s_uart);
        __HAL_UART_CLEAR_PEFLAG(&s_uart);
        (void)HAL_UARTEx_ReceiveToIdle_DMA(&s_uart, s_rx_dma, (uint16_t)RS485_RX_DMA_SIZE);
    }
}

/* ===================== 对外接口 ===================== */

uint8_t rs485_init(uint32_t baud)
{
    GPIO_InitTypeDef gpio;

    /* ---- 1) 时钟：本模块自己开（⛔ 不依赖 CubeMX 的 MspInit，见 `rs485.h` 第 2 条） ---- */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOD_CLK_ENABLE();
    __HAL_RCC_AFIO_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    /* ---- 2) PD7 = DE/RE，推挽输出，**初值低 = 接收态**（上电默认不许驱动总线） ---- */
    HAL_GPIO_WritePin(RS485_DE_GPIO_PORT, RS485_DE_PIN, GPIO_PIN_RESET);
    gpio.Pin   = RS485_DE_PIN;
    gpio.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(RS485_DE_GPIO_PORT, &gpio);

    /* ---- 3) PA2 = TX（复用推挽）/ PA3 = RX
     * ★ RX 用**上拉**而不是 CubeMX 默认的 NOPULL：半双工时 DE 一高，收发器的 RO 就变成高阻，
     *   引脚会浮空 ⇒ 浮空脚上的噪声会被当成起始位，制造一堆"帧错误 + 假 IDLE 事件"。
     *   上拉把空闲态钉在高电平（与 UART 空闲电平一致）。⛔ 这不是"外部没接时的临时兜底"，
     *   是半双工接线的固有要求。 ---- */
    gpio.Pin   = RS485_TX_PIN;
    gpio.Mode  = GPIO_MODE_AF_PP;
    gpio.Pull  = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(RS485_TX_GPIO_PORT, &gpio);

    gpio.Pin  = RS485_RX_PIN;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(RS485_RX_GPIO_PORT, &gpio);

    /* ---- 4) 两个 DMA 通道（★ 通道号的出处与真机判据见 `rs485.h` 第 3 条） ---- */
    s_dma_rx.Instance                 = RS485_DMA_RX_CHANNEL;
    s_dma_rx.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    s_dma_rx.Init.PeriphInc           = DMA_PINC_DISABLE;
    s_dma_rx.Init.MemInc              = DMA_MINC_ENABLE;
    s_dma_rx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    s_dma_rx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    s_dma_rx.Init.Mode                = DMA_NORMAL;      /* ⛔ 不用 CIRCULAR：环回会让"帧长"无从判定 */
    s_dma_rx.Init.Priority            = DMA_PRIORITY_HIGH;
    if (HAL_DMA_Init(&s_dma_rx) != HAL_OK)
    {
        return RS485_ERR_HAL;
    }
    __HAL_LINKDMA(&s_uart, hdmarx, s_dma_rx);

    s_dma_tx.Instance                 = RS485_DMA_TX_CHANNEL;
    s_dma_tx.Init.Direction           = DMA_MEMORY_TO_PERIPH;
    s_dma_tx.Init.PeriphInc           = DMA_PINC_DISABLE;
    s_dma_tx.Init.MemInc              = DMA_MINC_ENABLE;
    s_dma_tx.Init.PeriphDataAlignment = DMA_PDATAALIGN_BYTE;
    s_dma_tx.Init.MemDataAlignment    = DMA_MDATAALIGN_BYTE;
    s_dma_tx.Init.Mode                = DMA_NORMAL;
    s_dma_tx.Init.Priority            = DMA_PRIORITY_MEDIUM;
    if (HAL_DMA_Init(&s_dma_tx) != HAL_OK)
    {
        return RS485_ERR_HAL;
    }
    __HAL_LINKDMA(&s_uart, hdmatx, s_dma_tx);

    /* ---- 5) USART2：8N1，收发双工（半双工由 DE 脚决定，⛔ 不是 USART 的 HDSEL） ----
     * ⚠️ 用的是**普通 USART 模式 + 外部方向脚**，不是 STM32 的 HDSEL 单线半双工：
     *    HDSEL 只把 TX 引脚变成双向，它**不会**去驱动板上那颗收发器的 DE/RE，
     *    而板载 RS485 是"USART 全双工 ↔ 收发器 ↔ A/B"的经典拓扑。 */
    s_uart.Instance          = RS485_UART_INSTANCE;
    s_uart.Init.BaudRate     = baud;
    s_uart.Init.WordLength   = UART_WORDLENGTH_8B;
    s_uart.Init.StopBits     = UART_STOPBITS_1;
    s_uart.Init.Parity       = UART_PARITY_NONE;
    s_uart.Init.Mode         = UART_MODE_TX_RX;
    s_uart.Init.HwFlowCtl    = UART_HWCONTROL_NONE;
    s_uart.Init.OverSampling = UART_OVERSAMPLING_16;
    if (HAL_UART_Init(&s_uart) != HAL_OK)
    {
        return RS485_ERR_HAL;
    }

    /* ---- 6) NVIC：优先级**不在这里设**（`app_nvic_init()` 是唯一收口，见其头注），
     *      这里只负责"什么时候允许它触发"。 ---- */
    HAL_NVIC_EnableIRQ(USART2_IRQn);
    HAL_NVIC_EnableIRQ(RS485_DMA_RX_IRQn);
    HAL_NVIC_EnableIRQ(RS485_DMA_TX_IRQn);

    /* ---- 7) 武装接收（此后一直挂着，靠 `rs485_recover()` 自愈） ---- */
    if (HAL_UARTEx_ReceiveToIdle_DMA(&s_uart, s_rx_dma, (uint16_t)RS485_RX_DMA_SIZE) != HAL_OK)
    {
        return RS485_ERR_HAL;
    }

    return RS485_OK;
}

uint8_t rs485_set_baud(uint32_t baud)
{
    s_uart.Init.BaudRate = baud;

    return (HAL_UART_Init(&s_uart) == HAL_OK) ? RS485_OK : RS485_ERR_HAL;
}

uint16_t rs485_rx_take(uint8_t *dst, uint16_t dstsz)
{
    uint16_t n;

    rs485_recover();                    /* ★ 先自愈再取帧：一次噪声不该变成"永久收不到" */

    if ((dst == NULL) || (s_rx_ready == 0u))
    {
        return 0u;
    }

    n = s_rx_len;
    s_rx_ready = 0u;
    s_rx_len   = 0u;

    if (n > dstsz)
    {
        s_rx_overrun++;                 /* 缓冲不够 = 丢帧（⛔ 不截断，理由见回调里） */
        return 0u;
    }

    memcpy(dst, s_rx_frame, (size_t)n);

    return n;
}

uint8_t rs485_tx_start(const uint8_t *src, uint16_t len)
{
    if ((src == NULL) || (len == 0u) || (len > APP_MB_ADU_MAX))
    {
        return RS485_ERR_PARAM;
    }

    if (s_tx_busy != 0u)
    {
        return RS485_ERR_BUSY;
    }

    memcpy(s_tx_buf, src, (size_t)len);

    s_tx_done     = 0u;
    s_tx_busy     = 1u;
    s_tx_start_ms = HAL_GetTick();

    RS485_DE_TO_TX();
    /* ★ DE 建立时间：置方向脚到启动 DMA 之间必须留出收发器导通时间（`app_config.h` §12.13.2）。
     *   `delay_us()` 走 `SysTick->VAL` 轮询、不依赖中断 ⇒ 在调度器前后都成立（C-107 口径）。 */
    delay_us(APP_485_DE_SETTLE_US);

    if (HAL_UART_Transmit_DMA(&s_uart, s_tx_buf, len) != HAL_OK)
    {
        RS485_DE_TO_RX();               /* ⛔ 启动失败必须自己把方向脚放回去，否则总线被永久占住 */
        s_tx_busy = 0u;
        s_tx_errors++;
        return RS485_ERR_HAL;
    }

    return RS485_OK;
}

uint8_t rs485_tx_busy(void)
{
    rs485_recover();

    return s_tx_busy;
}

uint8_t rs485_selftest(void)
{
    static const uint8_t pat[2] = { RS485_SELFTEST_PAT_0, RS485_SELFTEST_PAT_1 };
    uint32_t waited;

    if (s_tx_busy != 0u)
    {
        return RS485_ERR_BUSY;
    }

    s_tx_done = 0u;

    /* ★★ 直接调 HAL、**不经过 `rs485_tx_start()`**：本自检要把 DE 留在**接收**态，
     *    这样收发器的差分驱动是关的，测试字节只走到 MCU 的 TX 引脚 ——
     *    A/B 总线上**不会有任何波形**（⛔ 不打扰可能挂在上面的主机）。
     *    代价写在 `rs485.h` 的 `rs485_selftest()` 注里：这一格只证明 TX 通路。 */
    if (HAL_UART_Transmit_DMA(&s_uart, (uint8_t *)pat, 2u) != HAL_OK)
    {
        return RS485_ERR_HAL;
    }

    /* ★★ 2026-09-29 修（⛔ 不能只等中断里的 `s_tx_done`）。**真机证据**：
     *   本自检在**启动序列里恒返回 `RS485_ERR_BUSY`(2)**，而事后读
     *   `s_tx_done = 1` 且 `s_tx_errors = 0` ⇒ **通路是好的、TC 也真的来了**，只是
     *   **来得比 20ms 晚**。根因：启动序列到 `vTaskStartScheduler()` 之前，`BASEPRI` 停在
     *   FreeRTOS 的临界区哨兵值上（`app_main.c` 那段注释已写明 SysTick 因此被屏蔽）
     *   ⇒ 优先级 ≥ `configMAX_SYSCALL_INTERRUPT_PRIORITY` 的**一切中断都进不来**（USART2 / DMA 通道正是这个量级）
     *   ⇒ 靠中断置位的 `s_tx_done` 在启动期**结构上不可达**。
     *   ⇒ 判据改成**先看硬件事实**：DMA 剩余传输计数归零 **且** USART 的 TC 标志置位
     *     —— 这两条**不受中断屏蔽影响**；中断里的 `s_tx_done` 作为**第二形态**
     *     （它只在任务上下文里才可能为真）。★ 这也更符合“**验硬件，不验处理器**”。 */
    if (s_uart.hdmatx == NULL)
    {
        return RS485_ERR_HAL;           /* 没挂 TX DMA 句柄 = 配置不完整，⛔ 不当“超时” */
    }

    for (waited = 0u; waited < RS485_SELFTEST_WAIT_MS; waited++)
    {
        if (s_tx_done != 0u)
        {
            s_tx_proof = 1u;            /* 1 = 中断通路（启动序列里通常不可达，见上） */
            return RS485_OK;
        }

        if ((s_uart.hdmatx->Instance->CNDTR == 0u) &&
            (__HAL_UART_GET_FLAG(&s_uart, UART_FLAG_TC) != 0u))
        {
            s_tx_proof = 2u;            /* 2 = 硬件寄存器（启动序列里应当走这一支） */
            return RS485_OK;
        }

        delay_ms(1);
    }

    return RS485_ERR_BUSY;              /* 超时 = DMA 通道映射 / USART DMA 请求 / 两者都不通 */
}

/* ===================== 中断入口 =====================
 * ★ 定义在**本文件**而不是 `User/stm32f1xx_it.c`，两条理由：
 *   ① `stm32f1xx_it.c` 是 CubeMX 生成物，而 `app_nvic.c` 头注已定口径"生成物只当初值"；
 *      本工程的外设归属是按**模块**划的 —— USART2 与 DMA1_CH6/CH7 只服务 RS485，
 *      入口跟实现放一起，将来删/换这个模块时不会在生成物里留下一个悬空的中断函数。
 *   ② 向量表只认**符号名**，定义在哪个编译单元与链接结果无关（这一点必须写下来，
 *      否则下一个人会以为"中断必须写在 it.c 里"）。
 * ⚠️ 工程里原本没有 USART2 / DMA1_CH6 / DMA1_CH7 的任何入口（grep 实测 0 命中），
 *    所以这里是**新增**，不是覆盖。 */
void USART2_IRQHandler(void)
{
    HAL_UART_IRQHandler(&s_uart);
}

void DMA1_Channel6_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_dma_rx);
}

void DMA1_Channel7_IRQHandler(void)
{
    HAL_DMA_IRQHandler(&s_dma_tx);
}

/* ===================== 取证出口（⛔ 不参与任何判据） ===================== */
uint32_t rs485_rx_frames(void)  { return s_rx_frames; }
uint32_t rs485_rx_overrun(void) { return s_rx_overrun; }
uint32_t rs485_tx_errors(void)  { return s_tx_errors; }
uint16_t rs485_last_rx_len(void){ return s_last_rx_len; }
