/**
 ******************************************************************************
 * @file    app_nvic.h
 * @brief   NVIC 优先级分组与中断优先级的唯一落点（AGENTS.md §3 固定补丁 14）
 * @note    对应设计：Tech_Design.md §4.3；审查记录：附录C · C-35 / C-65
 *
 * 为什么要有这个文件：
 *   CubeMX 把 `HAL_NVIC_SetPriorityGrouping()` 写在 `stm32f1xx_hal_msp.c`、
 *   把各外设的 `HAL_NVIC_SetPriority()` 写在 `xxx_MspInit()` 里 —— 两者都在
 *   **重新生成必被覆盖**的区域。所以本工程约定：CubeMX 生成值只是初值，
 *   **最终值一律由 `app_nvic_init()` 在所有 `MX_*_Init()` 之后覆盖一次**。
 ******************************************************************************
 */
#ifndef __APP_NVIC_H
#define __APP_NVIC_H

/* ===================== 优先级数值约定（★ 先看这段再改数） =====================
 * F103 只有 4 位优先级位，"抢占/子优先级怎么分"由分组决定，而**同一个数字在不同
 * 分组下编出来的寄存器值不同**：
 *     GROUP_2(2 位抢占)：preempt=2 → 原始值 0x80 ；preempt=5 → 越界（HAL 的
 *                        assert_param 因 USE_FULL_ASSERT 未开而被编译掉 → **静默**
 *                        编成 0x40，反而低于内核上限）—— 见附录C · C-65
 *     GROUP_4(4 位抢占)：preempt=n → 原始值 = n<<4，与文档里说的"数字"一比一对应
 * 本工程的 `AGENTS.md` §2.3 规定"调用 xxxFromISR 的中断，抢占优先级数值必须 ≥ 5"，
 * 该规则**只有在 GROUP_4 下才可表达**（GROUP_2 只有 0~3 档）。
 * ⇒ `app_nvic_init()` 第一件事就是把分组钉成 GROUP_4，然后才谈优先级数字。
 * ============================================================================*/

/* 必须 ≥ configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY(=5) 才允许调 FreeRTOS 的
 * ...FromISR() API；本文件底部有编译期断言，写小了编译不过。 */
#define APP_IRQ_PRIO_AFE_ALERT        5u      /* PD13 → EXTI15_10：AFE 保护动作，D8 启用 */
/* ⚠️ ★ 2026-09-28 就地更正：本行原写 "USART1(调试/ESP8266) / **USART3(RS485-Modbus)**" —— 错。
 *   真值（`Tech_Design.md` §2，引脚口径唯一权威）：**USART1 = 调试口**、
 *   **USART3 = ESP8266**（`esp8266.c` 的 HAL 回调按该实例守卫）、
 *   **USART2 = 板载 RS485 / Modbus**（PA2/PA3 + PD7 方向脚）。详见 `app_nvic.c` 里的更正说明。 */
#define APP_IRQ_PRIO_UART             6u      /* USART1(调试) / USART2(RS485·Modbus) / USART3(ESP8266)
                                               *   ＋ RS485 的两个 DMA 通道（DMA1_CH6/CH7）
                                               *   ⚠️ RS485 那三个 ISR **不调任何 FromISR API**
                                               *      （`rs485.h` 第 4 条）⇒ 对 5 这个下限无约束，
                                               *      同档只是为了让"同一类东西只有一个数字"。 */

/**
 * @brief       钉分组 + 覆盖全部本工程中断的优先级（含为 D8 预留的 ALERT）
 * @note        ★ 调用时机：`HAL_Init()` 与**所有** `MX_*_Init()` 之后、
 *              `vTaskStartScheduler()` 之前。当前落在 `main.c` 的 USER CODE 2 区。
 *              ⛔ 不要挂到某个 `MspInit()` 尾部 —— 重新生成即失效。
 */
void app_nvic_init(void);

#endif /* __APP_NVIC_H */
