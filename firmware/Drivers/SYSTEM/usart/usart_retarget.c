/**
 ****************************************************************************************************
 * @file        usart.c
 * @brief       printf 重定向到 USART1（调试串口）
 * @note        USART1 的初始化由 CubeMX 生成的 Src/usart.c 完成(MX_USART1_UART_Init)。
 *              本文件只负责把 printf 的输出送到 USART1 的数据寄存器。
 *              同时兼容 AC5 / AC6 两种编译器, 并关闭半主机模式。
 *
 * ★★ 2026-09-30（v0.65）：**整行单写者** —— 见下面 `fputc()` 前的长注。
 ****************************************************************************************************
 */

#include "./SYSTEM/usart/usart.h"
#include "usart.h"      /* CubeMX 生成: 提供 huart1 句柄 */
#include "./App/app_config.h"   /* APP_PRINT_LINE_MAX / APP_PRINT_SLOTS（参数单一来源） */
#include "FreeRTOS.h"           /* 只为 `xTaskGetSchedulerState()` / `xTaskGetCurrentTaskHandle()` */
#include "task.h"

/* 判断编译器类型, 如果是 AC6 编译器则执行 __use_no_semihosting */
#if defined ( __ARMCC_VERSION ) && ( __ARMCC_VERSION >= 6010050 )
__asm(".global __use_no_semihosting\n\t");   /* 声明不使用半主机模式 */
__asm(".global __ARM_use_no_argv \n\t");     /* AC6 下声明 main 为无参数格式 */
#else
#pragma import(__use_no_semihosting)

struct __FILE
{
    int handle;
};

#endif

/* 不使用半主机模式, 需要重定义以下函数(同时兼容 AC5 / AC6) */
int _ttywrch(int ch)
{
    (void)ch;
    return ch;
}

void _sys_exit(int x)
{
    (void)x;
}

char *_sys_command_string(char *cmd, int len)
{
    (void)cmd;
    (void)len;
    return NULL;
}

FILE __stdout;

/**
 * @brief       printf 底层输出函数
 * @param       ch: 待发送字符
 * @param       f : 文件句柄(未使用)
 * @retval      ch
 * @note        直接写 USART1 的 SR/DR 寄存器, 不依赖 HAL 中断状态, 可在任何时刻调用。
 */
/* ★★ 2026-09-29：原实现是**无上限** TC 忙等（`开工执行清单.md` §7 那笔债 ／ `附录C` C-107）。
 *   它的**优点**必须保留：不依赖中断 ⇒ "tick 已死而串口还活着"那类场景靠它才能打出字。
 *   **风险**：USART1 时钟没开 / 波特率配错 / 外设被复位时，它自己就是下一个同款无限循环，
 *   而且**连一行都打不出来**（比原来那种更难查）。
 *   ⇒ 改成**有限等待 + 快速失败 + 留痕**（形态照 `app_protect.c` 的 `wdg_wait_flag()`：轮询 + 上限 + **超时要有出口**）。
 *   ★ 一旦超时就置 `s_tx_dead` ⇒ 后续字符**不再等**（否则每个字符都白等一次，一行 printf 就是 N × 上限）。
 * ⛔ 不得把超时用 `(void)` 丢掉——那等于把"口吐死了"这件事从现场抹掉。 */
#define USART_RETARGET_WAIT_MAX   200000u   /* 空循环上限：@72MHz 约十几 ms，远大于 115200 下一个字符的 ~87µs */

static volatile uint8_t  s_tx_dead;         /* 1 = 已判死（出口） */
static volatile uint16_t s_tx_timeouts;     /* 超时累计（出口） */

/* ==========================================================================================
 *  ★★ 2026-09-30（v0.65）：**把"一行"做成原子单位** —— 多任务共享 printf 的单写者
 *
 *  **为什么必须改**：调度器接管（D5）之后，`printf` 变成**多个任务共享的一个字节流**，
 *    而下面 `retarget_putc_raw()` 是**逐字节**写寄存器 ⇒ 高优先级任务可以在**一行中间**抢占
 *    ⇒ 抓回来的日志里两行被劈开、拼在一起（`清单` §7 登记的那笔「运行期日志行序不可信」；
 *    它的后果不是"难看"：**判据、时间线、归因全都建立在这条字节流上**）。
 *
 *  **做法（三条）**：
 *   ① **调度器未启动**（开机单线程段）⇒ **直接逐字节写**：行为与旧版**逐字节相同**，
 *      ⇒ 开机 banner / `[AFE] init` / `WDG :` / 自检行 / 日志导出的字节序**一个字都不变**。
 *   ② **调度器已启动** ⇒ 每个"生产者"（按 `xTaskGetCurrentTaskHandle()` 惰性分配一个槽）
 *      各有一条**行缓冲**：遇 `'\n'`（或缓冲满）才把**整行**一次写出。
 *   ③ 写之前抢一个"整行写者"令牌（**关中断两条指令**的测试并置位；单核上这就不存在
 *      "测试与置位之间被抢占"）—— 抢不到就**整行丢弃并计数**（`s_print_drops`）。
 *      ⛔ 不阻塞（保护任务 ／ 任何任务都不会因为打印而被卡住）、⛔ 不交错（宁可丢一行，不劈一行）。
 *
 *  **为什么不是"加锁"**：本项目 S6/§2.3 的口径是**保护路径不得等锁**；而打印是**尽力而为**的
 *    诊断通道 ⇒ "丢弃 + 可数的代价" 比"让任务在锁上排队"更符合本项目的取舍。
 *
 *  ⚠️ **代价与边界**（都要能被数出来，⛔ 不许静默）：
 *    · `s_print_drops` = 抢不到令牌而整行丢弃的次数；
 *    · `s_print_splits` = 一段超过 `APP_PRINT_LINE_MAX-1` 字节而被切段的次数；
 *    · `s_print_ctx_full` = 生产者超过 `APP_PRINT_SLOTS` 个（退化直写，可能交错）。
 *  ⚠️ **本机制只覆盖任务上下文**：⛔ 中断里不得 `printf`（本工程目前没有这种调用；
 *     真要在 ISR 里输出，必须走"置标志 + 任务打印"，否则会破坏单写者假设）。
 * ========================================================================================== */
static volatile uint8_t  s_print_drops;      /* 整行丢弃次数（诊断出口，⛔ 不参与判据） */
static volatile uint8_t  s_print_splits;     /* 超长切段次数（诊断出口，⛔ 不参与判据） */
static volatile uint8_t  s_print_ctx_full;   /* 生产者槽位用尽（1 = 已退化直写） */

/* ── ★★ 2026-09-30（v0.66）：把**上电段打印的原文**留在 RAM 里 ──────────────────────────────
 *  为什么：真·冷启动的开机诊断行在 COM6 上**抓不到**（CH340 与 MCU 同电 ⇒ USB 枚举 ≈1s 晚于开机打印，
 *  见 `附录C` C-144）⇒ 冷上电那一刻的 `WDG :` / `[AFE] init` / 自检行**没有任何观测形态**（除屏与调试器）。
 *  做法：**预调度期**（单线程、逐字节直写那一段）顺手把每个字符存进这块缓冲；调度器一启动就不再抓
 *  （⇒ ⛔ 不会把运行期输出、也不会把"回放本身"抓进来）。
 *  出口：`usart_retarget_bootlog_replay()`（串口命令 `?` 调用）＋ 长度/溢出两个计数。
 *  ⚠️ 满了就停（不覆盖旧内容）＋ `s_bootlog_ovf` 置位 ⇒ "被截断"这件事是**看得见**的。 */
static char     s_bootlog[APP_BOOTLOG_BUF];
static uint16_t s_bootlog_len;
static uint8_t  s_bootlog_ovf;

static TaskHandle_t s_print_owner[APP_PRINT_SLOTS];      /* 惰性分配：谁在用哪个槽 */
static char         s_print_buf[APP_PRINT_SLOTS][APP_PRINT_LINE_MAX];
static uint8_t      s_print_len[APP_PRINT_SLOTS];
static TaskHandle_t s_line_writer;                        /* 当前正在写整行的生产者（NULL = 空闲） */

/** 逐字节写（有限等待版）：开机段与"退化直写"都用它 */
static int retarget_putc_raw(int ch)
{
    uint32_t waited;

    if (s_tx_dead != 0u)
    {
        USART1->DR = (uint8_t)ch;           /* 已判死：不再等，也不报错（判死那一次已计入 s_tx_timeouts） */
        return ch;
    }

    for (waited = 0u; waited < USART_RETARGET_WAIT_MAX; waited++)
    {
        if ((USART1->SR & 0X40U) != 0U)     /* TC 置位 = 上一个字符发完 */
        {
            USART1->DR = (uint8_t)ch;
            return ch;
        }
    }

    /* ★ 超时：留痕 + 判死（⛔ 不是 `(void)` 丢掉） */
    s_tx_dead = 1u;

    if (s_tx_timeouts < 0xFFFFu)
    {
        s_tx_timeouts++;
    }

    USART1->DR = (uint8_t)ch;               /* 仍写一次：USART 正常时它无害，异常时也不会更坏 */
    return ch;
}

/** 找（或分配）当前任务的行缓冲槽；返回 -1 = 槽位用尽（调用方退化直写） */
static int retarget_slot(void)
{
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    int i, free_i = -1;

    for (i = 0; i < (int)APP_PRINT_SLOTS; i++)
    {
        if (s_print_owner[i] == me)
        {
            return i;
        }

        if ((s_print_owner[i] == NULL) && (free_i < 0))
        {
            free_i = i;
        }
    }

    if (free_i >= 0)
    {
        s_print_owner[free_i] = me;
        s_print_len[free_i]   = 0u;
        return free_i;
    }

    s_print_ctx_full = 1u;
    return -1;
}

/** 把槽里已有的整行一次写出；抢不到令牌 ⇒ 整行丢弃（可数） */
static void retarget_flush(int slot)
{
    uint8_t      n = s_print_len[slot];
    uint8_t      k;
    uint32_t     primask;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();

    s_print_len[slot] = 0u;                 /* 无论成功与否，缓冲区先归还 */

    if (n == 0u)
    {
        return;
    }

    primask = __get_PRIMASK();
    __disable_irq();

    if ((s_line_writer == NULL) || (s_line_writer == me))
    {
        s_line_writer = me;                 /* 抢到（或本来就是自己）：继续把这一行写完 */
        k = 1u;
    }
    else
    {
        k = 0u;                             /* 别人正在写整行 */
    }

    if (primask == 0u)
    {
        __enable_irq();
    }

    if (k == 0u)
    {
        if (s_print_drops < 0xFFu)
        {
            s_print_drops++;                /* ★ 代价可数：宁可丢一行，也不劈开两行 */
        }
        return;
    }

    for (k = 0u; k < n; k++)
    {
        (void)retarget_putc_raw((int)(uint8_t)s_print_buf[slot][k]);
    }

    s_line_writer = NULL;                   /* 释放令牌 */
}

int fputc(int ch, FILE *f)
{
    int slot;

    (void)f;

    /* ① 开机段（调度器未启动）：单线程 ⇒ 直接写，行为与旧版逐字节相同
     *    ★ 顺手把原文抓进 `s_bootlog`（真冷启动抓不到开机行 ⇒ 见文件末的回放出口） */
    if (xTaskGetSchedulerState() != taskSCHEDULER_RUNNING)
    {
        if (s_bootlog_len < (uint16_t)(APP_BOOTLOG_BUF - 1u))
        {
            s_bootlog[s_bootlog_len] = (char)ch;
            s_bootlog_len++;
        }
        else
        {
            s_bootlog_ovf = 1u;
        }

        return retarget_putc_raw(ch);
    }

    slot = retarget_slot();

    if (slot < 0)
    {
        return retarget_putc_raw(ch);       /* ② 槽位用尽：退化直写（s_print_ctx_full 已置位） */
    }

    /* ③ 缓冲满且这一字符不是行尾 ⇒ 先把已有内容作为"一段"写出（切段可数），再收这个字符 */
    if ((ch != '\n') && (s_print_len[slot] >= (uint8_t)(APP_PRINT_LINE_MAX - 1u)))
    {
        retarget_flush(slot);

        if (s_print_splits < 0xFFu)
        {
            s_print_splits++;
        }
    }

    if (s_print_len[slot] < (uint8_t)(APP_PRINT_LINE_MAX - 1u))
    {
        s_print_buf[slot][s_print_len[slot]] = (char)ch;
        s_print_len[slot]++;
    }

    if (ch == '\n')
    {
        retarget_flush(slot);               /* ④ 行尾 ⇒ 整行原子写出（或整行丢弃） */
    }

    return ch;
}

/** ★ 取证出口（⛔ 不参与判据）：超时累计次数 */
uint16_t usart_retarget_tx_timeouts(void) { return s_tx_timeouts; }

/** ★ 取证出口（⛔ 不参与判据）：1 = 口已判死（后续不再等） */
uint8_t  usart_retarget_tx_dead(void)     { return s_tx_dead; }

/** ★ 取证出口（⛔ 不参与判据）：整行丢弃 / 超长切段 / 槽位用尽 —— 三个"代价可数"的计数 */
uint8_t  usart_retarget_print_drops(void) { return s_print_drops; }
uint8_t  usart_retarget_print_splits(void){ return s_print_splits; }
uint8_t  usart_retarget_print_ctx_full(void){ return s_print_ctx_full; }

/** ★ 取证出口：把**上电段打印的原文**整段重放（串口命令 `?` 的主人；⛔ 只读，不改任何状态）
 *  ⚠️ 必须**走 `fputc()`（= 整行单写者路径）而不是裸写**：第一版走裸写 ⇒ 实测回放被运行期的行
 *     **插开**（抓到 `…harnessESP >> AT)` 这种形态）—— 回放的内容本身就是"一行行文本"，
 *     交给单写者路径才会**整行原子**（抢不到就整行丢、可数），⛔ 不会与别的任务交错。 */
void usart_retarget_bootlog_replay(void)
{
    uint16_t i;

    for (i = 0u; i < s_bootlog_len; i++)
    {
        (void)fputc((int)(uint8_t)s_bootlog[i], &__stdout);   /* ⛔ 不许传 NULL：库头把该参数标了 nonnull（实测 1 warning） */
    }
}

/** ★ 取证出口：上电段原文的字节数（0 = 还没抓到） */
uint16_t usart_retarget_bootlog_len(void)      { return s_bootlog_len; }

/** ★ 取证出口：1 = 上电段原文曾超过缓存（被截断，⛔ 不得当成"全都在这"） */
uint8_t  usart_retarget_bootlog_overflow(void) { return s_bootlog_ovf; }
