/**
 ******************************************************************************
 * @file    app_nvic.c
 * @brief   NVIC 优先级收口实现（AGENTS.md §3 固定补丁 14 / 附录C · C-35、C-65）
 * @note    移植来源：无（本工程新写，不来自前序工程）
 *          改动点：新增文件；把 CubeMX 写在 `stm32f1xx_hal_msp.c`（GROUP_2）与
 *                  `usart.c` 的 MspInit（抢占 2）里的最终值统一覆盖一次
 *          验证方式：**静态 + 编译级**（本轮未上台架）。① 编译期断言保证两个数字
 *                  落在 FreeRTOS 合法区间；② D5 联调时用 `DIS` 窗口读
 *                  `SCB->AIRCR[10:8]` 应为 `100`（GROUP_4）、
 *                  `NVIC->IP[USART1]` 应为 `0x60`、`NVIC->IP[EXTI15_10]` = `0x50`
 *
 * ★ 为什么必须在应用层再写一遍（C-35 / C-65）：
 *   CubeMX 的生成物只当"初值"。`HAL_NVIC_SetPriorityGrouping()` 落在
 *   `HAL_MspInit()`（重新生成即覆盖），优先级数字落在各 `MspInit()` 尾部 ——
 *   而本工程的规则"调用 xxxFromISR 的中断，抢占优先级数值必须 ≥ 5"
 *   在 CubeMX 默认的 **GROUP_2 下根本不可表达**（GROUP_2 只有 0~3 四档抢占）。
 *   更糟的是 `USE_FULL_ASSERT` 未开，`assert_param` 被编译掉，
 *   于是"填 5"会被**静默**编成原始值 `0x40`（低于内核上限 `0x50`）——
 *   表现为"代码看着对、跑起来在 ISR 里撞 configASSERT 死循环"。
 *   ⇒ 先把分组钉成 GROUP_4（数字与寄存器位一比一），再设优先级。
 *
 * ⛔ 不要挂到任何 `MspInit()` 尾部，也不要"顺手"写进 CubeMX 生成函数体内。
 ******************************************************************************
 */
#include "app_nvic.h"
#include "app_config.h"         /* APP_STATIC_ASSERT */
#include "stm32f1xx_hal.h"
#include "FreeRTOS.h"           /* configLIBRARY_MAX/LOWEST_INTERRUPT_PRIORITY */

/* 优先级数字必须落在 FreeRTOS 允许 ISR 调 API 的区间 [5, 15]。
 * 写小了 → 中断优先级高于内核上限，port.c 的 configASSERT 在调度器启动时挂；
 * 写大了（>15）→ 4 位优先级位放不下，GROUP_4 下会截断。两种都编译不过。 */
APP_STATIC_ASSERT(APP_IRQ_PRIO_AFE_ALERT >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, irq_alert_below_freeRTOS_max);
APP_STATIC_ASSERT(APP_IRQ_PRIO_AFE_ALERT <= configLIBRARY_LOWEST_INTERRUPT_PRIORITY,      irq_alert_below_lowest_prio);
APP_STATIC_ASSERT(APP_IRQ_PRIO_UART      >= configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY, irq_uart_below_freeRTOS_max);
APP_STATIC_ASSERT(APP_IRQ_PRIO_UART      <= configLIBRARY_LOWEST_INTERRUPT_PRIORITY,      irq_uart_above_lowest_prio);

void app_nvic_init(void)
{
    /* 1) 分组：4 位全给抢占，数字 = 寄存器高位，一比一，不与 CubeMX 的 GROUP_2 混用 */
    HAL_NVIC_SetPriorityGrouping(NVIC_PRIORITYGROUP_4);

    /* 2) 已启用的中断（EnableIRQ 在各自的 MspInit 里，本处只覆盖优先级）。
     * ⚠️ ★ **2026-09-28 就地更正一条与本条代码直接相关的口径错**：本行原写
     *    "USART1 = 调试串口 + ESP8266；**USART3 = RS485-Modbus**" —— 那是错的，
     *    两句都错：USART1 = 纯调试口（`printf` 重定向），**USART3 = ESP8266**
     *    （`Drivers/BSP/ESP8266/esp8266.c` 的两个 HAL 回调都按 `Instance == USART3` 守卫），
     *    而**板载 RS485 走 USART2**（`Tech_Design.md` §2 引脚表："板载 RS485 · Modbus |
     *    PA2 / PA3 + PD7(RE/DE) | **USART2**"）。⇒ 按 §2（引脚口径唯一权威）更正。
     *    ★ 这条错的危害正是它所在的位置：本函数是"哪些中断要设优先级"的唯一清单，
     *      照着它办会**漏配真正服务 Modbus 的那条串口**，而漏配的表现是"Modbus 偶尔不应答"
     *      （优先级不同 ⇒ 抢占关系不同），与线序/波特率问题完全同形。
     *    ★ 口径：本工程**所有**可能调用 `xxxFromISR()` 的 ISR 统一走 ≥5 这一档；
     *      当前 USART1 的 ISR 里没有 FromISR 调用（printf 是轮询发送），但给它
     *      更小的数字并不会带来任何收益 —— 所以三条串口一起设成 6，一次到位。
     *    ⚠️ USART2 与两个 DMA 通道（RS485 那三个中断）**今天一次 FromISR 都不调**
     *      （`rs485.c` 的回调只写 volatile 标志与寄存器，见其头注第 4 条）⇒ 它们对 5 这个
     *      下限其实**无约束**；仍按同一档设置，是为了"同一类东西只有一个数字"这一条更好查。 */
    HAL_NVIC_SetPriority(USART1_IRQn, APP_IRQ_PRIO_UART, 0u);
    HAL_NVIC_SetPriority(USART3_IRQn, APP_IRQ_PRIO_UART, 0u);
    /* ★ 2026-09-28（Modbus 从机轮）：**新增**下面三行 —— 板载 RS485 的 USART2 与它的
     * 两个 DMA 通道（RX = CH6 / TX = CH7，出处与真机判据见 `Drivers/BSP/RS485/rs485.h`）。
     * 这三行的落点是**本文件**而不是 `rs485.c`：`app_nvic_init()` 是"优先级唯一收口"
     * （见 `app_nvic.h` 头注），而 `rs485.c` 只负责"什么时候允许它触发"（EnableIRQ）。 */
    HAL_NVIC_SetPriority(USART2_IRQn,        APP_IRQ_PRIO_UART, 0u);
    HAL_NVIC_SetPriority(DMA1_Channel6_IRQn, APP_IRQ_PRIO_UART, 0u);
    HAL_NVIC_SetPriority(DMA1_Channel7_IRQn, APP_IRQ_PRIO_UART, 0u);

    /* 3) AFE ALERT（PD13 → EXTI15_10）：★ 此处**只定优先级，不 EnableIRQ**。
     *    EnableIRQ 属于 D8 的保护链路联调内容；提前开中断会在没接板子时被浮空
     *    引脚触发，而 S26 的解码逻辑还没接上（先解码再处置，禁止一响就锁定）。
     *
     * ★ D8 接这条中断时必须同时满足以下三条（附录C · C-68，缺一 ALERT 就静默失效）：
     *   a) PD13 配**浮空输入** —— 禁止 GPIO_PULLUP。ALERT 在 AFE 内部驱动"低"时只有
     *      0.8~8MΩ 弱下拉，任何上拉（含 STM32 内部约 40K）都会把"低"顶到 ≈3.2V；
     *   b) **上升沿**触发，且板上 ALERT 只接 **500K~1M 下拉到 VSS**、不得挂电容；
     *      否则 AFE 会把它当成"外部强制拉高"→ 置**覆盖告警位** → **自动关闭
     *      CHG/DSG 驱动**（电池包不能充放，且不报错，与"芯片坏""未 boot"同形）；
     *   c) ISR 只置标志；处理侧读完 AFE 状态寄存器（符号名见 `afe.h` 与
     *      `Drivers/BSP/AFE/afe_hal_bq76920.c`，⛔ 不得在本文件出现）后**必须回写
     *      清那个覆盖位**，不清则电平一直挂着，下一个上升沿再也等不到。 */
    HAL_NVIC_SetPriority(EXTI15_10_IRQn, APP_IRQ_PRIO_AFE_ALERT, 0u);
}
