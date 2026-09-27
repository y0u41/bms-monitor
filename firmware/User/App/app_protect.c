/**
 ****************************************************************************************************
 * @file        app_protect.c
 * @brief       TaskProtect 主体：告警帧唯一写者 + 条件喂狗 + 四态骨架（D5 / §8.4 M4+M6）
 * @note        口径与"为什么是现在建"见 `app_protect.h` 头注。本文件只写实现侧的三条约束。
 *
 * @note        ★ 本任务里 ⛔ 不得 `printf`。
 *              D5 之前整台机器只有一个打印者（超循环），多线程后 `printf` 变成共享资源：
 *              两个任务同时打印会让字节流从中间互相插入（乱码是轻的，重则把 `alarm_reason()`
 *              这类共享串缓冲区撕成半截）。今天 protect 不打印 ⇒ 交错面为 0；
 *              将来要打印必须先做输出互斥（排进 D9 协议层的串口收口，⛔ 不要临时加一句）。
 *              ⚠️ **唯一的例外**在 `app_protect_wdg_start()`：它由 `app_main()` 在
 *              `vTaskStartScheduler()` **之前**调用（单线程、无交错），打印的是"狗实际配成了
 *              多少"的回读 —— 那是 C-104 一类缺陷的现场证据，省掉它就等于回到靠猜。
 *              ⛔ 除那一处之外，本文件任何函数都不得再出现 `printf`。
 *
 * @note        ★ 本任务里 ⛔ 不得写 Flash（S25/S8）、不得碰 I²C、不得等任何锁。
 *              TaskProtect 是优先级最高的任务，它一旦阻塞，保护链路就整条停摆；
 *              落盘统一由 `app_log_add()` 入队、TaskLog 消费（`app_log.h` 纪律①）。
 *
 * @note        ⚠️ 状态机今天恒为 `BMS_STATE_SAFE_OFF`：没有执行器（D6）、没有三级判据（D8）。
 *              真值表（D6/D8 落地时逐条核对，⛔ 不得为了"看起来跑通了"提前放开）：
 *                SAFE_OFF      → PRECHARGE   需要"上层请求闭合"且 `V_SW` 通道有效（D6/D11）
 *                PRECHARGE     → ON          需要 `v_sw_10mv` 达标 + `tFET_ON` 余量（D6）
 *                ON/PRECHARGE  → FAULT_LATCHED  越限型 L3 或预充超限（D8/S11）
 *                FAULT_LATCHED → SAFE_OFF    只有长按 `APP_CTR_LATCH_HOLD_MS` 一条路（S11）
 ****************************************************************************************************
 */

#include "./App/app_protect.h"
#include "./App/app_config.h"
#include "./App/app_log.h"
#include "./App/app_tasks.h"
#include "./SYSTEM/delay/delay.h"        /* `wdg_wait_flag()` 的轮询式延时（⛔ 不用 HAL_GetTick，见函数注） */
#include <stdio.h>                      /* 仅 `app_protect_wdg_start()` 的回读打印用（调度器之前） */

/* ==================== 独立看门狗（★ 全工程唯一允许出现 `IWDG->KR` 的文件） ====================
 * 24 小时运行的设备，"死机后不报警"比"报警误响"危险得多 ⇒ 狗不可关（S13，`APP_ENABLE_IWDG` 已删）。
 * ★ 超时由 8s 收到 2s（C-36）：8s 的问题不是"太长会漏检"，而是**保护链路在窗口里是瞎的** ——
 *   复位期间软件层不参与判断，全靠 AFE 硬件层兜底（S1），窗口越短越好。
 * ⚠️ LSI 标称 40kHz，F1 实际 30~60kHz ⇒ 2.0s 是**理论值**，`Build.md` §8.4 V4 必须实测回填
 *   `APP_LSI_HZ`（不回填则实际超时 1.3~2.7s，而三个窗口的一致性断言全按标称算）。 */
#define IWDG_FEED()     do { IWDG->KR = 0xAAAA; } while (0)

/* 上一次"判活 + 喂狗"的时刻（HAL_GetTick 口径，与 `now_ms` 同钟）*/
static uint32_t s_last_feed_ms;

/**
 * @brief       等 `IWDG->SR` 指定位清零（= 新值已同步进计数器时钟域）
 * @param       flag_mask : 要等的位（`IWDG_SR_PVU` / `IWDG_SR_RVU`）
 * @param       timeout_ms: 轮询上限（ms）
 * @retval      0 = 已清零; 1 = 超时（由调用方的回读去定性）
 * @note        ★★ 这里 ⛔ 不得用 `HAL_GetTick()` 计时 —— 本函数跑在"调度器启动之前"，
 *              而 `app_main.c` 在更早处已投过开机事件日志（`app_log_add()` 的临界区），
 *              FreeRTOS 的 `uxCriticalNesting` 哨兵值（`port.c:135` = 0xaaaaaaaa）使得
 *              调度器前的那一次 `taskEXIT_CRITICAL()` 不会把 BASEPRI 降回 0；
 *              SysTick 优先级 = 15 > `configMAX_SYSCALL_INTERRUPT_PRIORITY` = 5 ⇒ 被屏蔽
 *              ⇒ HAL_GetTick 冻住 ⇒ "有界等待"退化成无限循环，而狗还没启动 ⇒ 永不自救。
 *              实测证据：v0.12 串口停在 `LOG :` 之后、`WDG :` 之前，屏停在 `lcd_init()` 的白。
 *              口径：`附录C` C-107。
 * @note        `delay_ms()` 走的是轮询 `SysTick->VAL`（`Drivers/SYSTEM/delay/delay.c`），
 *              不依赖任何中断，所以在本函数所处的"临界区未退出"窗口里仍然在走。
 * @note        不用 `HAL_IWDG_Init()`：那会引入一个 `IWDG_HandleTypeDef`，而本工程
 *              刻意不经过 HAL 配狗 —— 目的就是要让"写进寄存器的值"与"断言算的值"
 *              是同一个量（C-104 的教训：中间多一层换算就多一处可能静默丢写）。
 *              但等待这一条照抄 HAL，因为它是 RM0008 的明文要求，不是风格问题。
 */
static uint8_t wdg_wait_flag(uint32_t flag_mask, uint32_t timeout_ms)
{
    uint32_t waited;

    for (waited = 0u; waited < timeout_ms; waited++)
    {
        if ((IWDG->SR & flag_mask) == 0u)
        {
            return 0u;
        }

        delay_ms(1);
    }

    return ((IWDG->SR & flag_mask) == 0u) ? 0u : 1u;
}

/**
 * @brief       确保 LSI 在转（IWDG 的唯一时钟源）
 * @param       timeout_ms: 起振上限
 * @retval      0 = LSIRDY 已置位; 1 = 超时
 * @note        ★ 必须在写 PR/RLR 之前做。RM0008：PVU/RVU 由**计数器时钟域（= LSI）**清掉，
 *              而 LSI 平时是关的（`0xCCCC` 也会开它，但那一步在配置之后）。
 *              冷上电时 LSI 没转 ⇒ `wdg_wait_flag(PVU)` 必然白等 50ms 超时 ⇒ 紧接着的
 *              `IWDG->RLR` 写入照旧被丢弃 ⇒ C-106 的 6.55s 窗口原封不动。
 *              也就是说：本函数缺位时，"等 PVU 再写 RLR"这个修法形同没修。
 * @note        只置 `LSION`、从不关：LSI 一旦开启只能靠复位关闭，且 RTC 若选了 LSI 会共用它。
 */
static uint8_t wdg_start_lsi(uint32_t timeout_ms)
{
    uint32_t waited;

    RCC->CSR |= RCC_CSR_LSION;

    for (waited = 0u; waited < timeout_ms; waited++)
    {
        if ((RCC->CSR & RCC_CSR_LSIRDY) != 0u)
        {
            return 0u;
        }

        delay_ms(1);
    }

    return ((RCC->CSR & RCC_CSR_LSIRDY) != 0u) ? 0u : 1u;
}

void app_protect_wdg_start(void)
{
    uint8_t  pr_rd;
    uint16_t rlr_rd;
    uint32_t real_ms;
    uint8_t  lsi_to;
    uint8_t  pvu_to;
    uint8_t  rvu_to;

    DBGMCU->CR |= DBGMCU_CR_DBG_IWDG_STOP;  /* 调试器暂停时冻结狗，否则单步必被复位（"见鬼"第一来源） */

    lsi_to = wdg_start_lsi(100u);           /* ★ 时钟不在，后面两位"更新完成"标志永远不清（见函数注） */

    IWDG->KR  = 0x5555;                     /* 解除写保护 */
    IWDG->PR  = APP_IWDG_PR;                /* ★ 写编码 4（= 64 分频），⛔ 不是分频数 64：
                                              *   PR 只有 [2:0] 三位，写 64 会被静默截成 0 → 实际 /4
                                              *   → 窗口 125ms < 喂狗周期 500ms → 必然复位循环。
                                              *   口径与断言见 `app_config.h` §11 / `附录C` C-104。 */

    /* ★★ 必须等 PVU 清零之后再写 RLR（RM0008 IWDG_SR 的明文要求）。
     *   不等的后果 = 这一次 `IWDG->RLR` 写入被静默丢弃，重载值停在复位默认的 0x0FFF
     *   ⇒ 真实窗口 = 4095 × 64 ÷ 40kHz ≈ 6.5s，而设计值仍然写着 2s（`附录C` C-106）。
     *   ⚠️ 与 C-104 同族、且更隐蔽：§12.6 那 10 条编译期断言算的是宏，
     *      宏永远是 1250 ⇒ 一条都不响。所以本函数下面加了"回读 + 打印"，
     *      把窗口从"断言证明自洽"升级成"寄存器现状被看见"。
     *   上限 50ms：LSI 40kHz 下 PVU 只需 3 个 LSI 周期 ≈ 4.8ms，这里是 10 倍余量。
     *   ★ 超时不再 `(void)` 丢掉：超时就等于"这一笔没写进去"，必须被看见（C-106 的出口）。 */
    pvu_to = wdg_wait_flag(IWDG_SR_PVU, 50u);

    IWDG->RLR = APP_IWDG_RLR;               /* 625Hz × 2s = 1250（⚠️ 待 LSI 实测标定，V4） */

    /* 等 RVU 再启动：否则第一次计数用的是旧重载值，开机第一个窗口不是 2s 而是上一版的值 */
    rvu_to = wdg_wait_flag(IWDG_SR_RVU, 50u);

    IWDG->KR  = 0xCCCC;                     /* 启动 */
    IWDG->KR  = 0xAAAA;                     /* 立即喂一次：消除"启动即满窗"，⛔ 这不是喂狗点 */

    /* ---------- 回读：唯一能证明"窗口真的是设计值"的观测 ----------
     * ⚠️ 本文件头注"⛔ 不得 printf"管的是**任务里**（多线程共享打印通路）；
     *    本函数由 `app_main()` 在 `vTaskStartScheduler()` **之前**调用，单线程、无交错。
     *    而这一行恰好是"狗到底配成了多少"的唯一现场证据 —— 省掉它 = 回到 C-104 的排查方式。
     *    ★ 三个 `*_to` 一起打印：`ok` 只说明 PR/RLR 读回来对得上，而对不上时必须能立刻
     *      分辨"是 LSI 没起（lsi=1）"还是"起了但同步超时（pvu/rvu=1）" —— 这三种失败
     *      的修法完全不同，缺这一列就又要靠猜。 */
    pr_rd    = (uint8_t)(IWDG->PR & IWDG_PR_PR);
    rlr_rd   = (uint16_t)(IWDG->RLR & IWDG_RLR_RL);
    real_ms  = (uint32_t)rlr_rd * (4u << pr_rd) * 1000u / APP_LSI_HZ;

    printf("WDG  : pr=%u(/%u) rlr=%u -> %ums  %s  lsi=%u pvu=%u rvu=%u\r\n",
           (unsigned)pr_rd, (unsigned)(4u << pr_rd), (unsigned)rlr_rd, (unsigned)real_ms,
           ((pr_rd == (uint8_t)APP_IWDG_PR) && (rlr_rd == (uint16_t)APP_IWDG_RLR))
               ? "ok" : "MISMATCH(check LSI)",
           (unsigned)lsi_to, (unsigned)pvu_to, (unsigned)rvu_to);

    s_last_feed_ms = HAL_GetTick();
}

/* ==================== 状态与请求标志 ==================== */
/* 状态变量的**唯一写者**是本文件（S9）；界面与 Modbus 只经 `app_protect_state()` 读。 */
static bms_state_t s_state = BMS_STATE_SAFE_OFF;

/* 单词标志：置位者 = UI 任务（`app_ui.c` 的按键状态机），消费并清零者 = 本任务。
 * ⛔ 不加锁、不用队列：跨任务传"发生过一次按键"这种单向事件，单词标志天然幂等。
 *    代价 = 连续两次长按可能被并成一次 —— 而这里要的效果就是"解除锁定"，合并无害；
 *    加锁换来的却是保护路径可能阻塞（S12 / AGENTS §2.3"单向数据用双缓冲或标志，不用锁"）。 */
static volatile uint8_t s_unlock_req;

bms_state_t app_protect_state(void)
{
    return s_state;
}

void app_protect_request_unlock(void)
{
    s_unlock_req = 1u;
}

/* ==================== 周期体 ==================== */
static void protect_consume_unlock(void)
{
    if (s_unlock_req == 0u)
    {
        return;
    }

    s_unlock_req = 0u;

    /* ★ 今天必然走"无可解除"这一支：锁定要到 D8（三级告警 L3）才产生。
     *   通路从 D5 就端到端建好（`Build.md` §2 判据⑥b），D8 只需往 `s_state` 里填值，
     *   不必再动"按键 → 请求 → 属主"这条链 —— 链上每一环晚建一天就多一处漏。 */
    if (s_state == BMS_STATE_FAULT_LATCHED)
    {
        s_state = BMS_STATE_SAFE_OFF;
        app_log_add(EV_LATCH_CLEAR, 1u, 0u, 0u);     /* code 1 = 长按 5s 解除（协议 §6.1 已定义） */
    }
}

/**
 * @brief       判活 + 条件喂狗（验收项 WDG-2 / WDG-3）
 * @note        ★ 判据是"逐任务 deadline"（C-34），不是"每个任务都跑过一遍"：后者对
 *              检查节拍与任务周期的比值敏感，会把健康的机器判成卡死 ⇒ 正常运行也 2s 一次复位。
 * @note        ⛔ 不许在别处无条件喂：那等于"任何一个任务卡死都检不出来"。
 */
static void protect_wdg_check(uint32_t now_ms)
{
    uint8_t stalled;

    if ((uint32_t)(now_ms - s_last_feed_ms) < (uint32_t)APP_WDG_CHECK_MS)
    {
        return;
    }

    stalled        = app_tasks_stalled_mask();
    s_last_feed_ms = now_ms;        /* 计时照推进：卡死时不推进会让每次 10ms 都重判一遍 */

    if (stalled == 0u)
    {
        IWDG_FEED();
        return;
    }

    /* 不喂 ⇒ ≤2s 复位。复位前把"是哪个任务卡的"投进日志（非阻塞队列，`app_log.h` 纪律①）：
     * TaskLog 每 50ms 落一条，窗口有 2000ms ⇒ 来得及；复位后靠
     * `EV_WDG_RESET`（复位原因）+ 这条 `EV_TASK_STALLED`（位图）配对还原现场。 */
    app_log_add(EV_TASK_STALLED, stalled, 0u, 0u);
}

void app_protect_step(uint32_t now_ms)
{
    /* ① 告警帧唯一写者（M6 交接过来的身份，C-97 F1）*/
    app_data_link_check(now_ms);

    /* ② 解锁请求处置 */
    protect_consume_unlock();

    /* ③ 判活 + 条件喂狗 */
    protect_wdg_check(now_ms);
}
