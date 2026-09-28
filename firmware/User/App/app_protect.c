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
 * @note        ★ 本任务里 ⛔ 不得写 Flash（S25/S8）。
 *              TaskProtect 是优先级最高的任务，一旦它停顿，保护链路就整条停摆；
 *              落盘统一由 `app_log_add()` 入队、TaskLog 消费（`app_log.h` 纪律①）。
 *
 * @note        ⚠️ **2026-09-28（D6）就地更正本文件头注原来那句"不得碰 I²C、不得等任何锁"**：
 *              状态机的四条转移必须写 AFE 的通道位（§5.1 的与逻辑），而那一笔写落在
 *              `g_bus_afe` 上 ⇒ "不得碰 I²C"若照字面执行，状态机就永远走不出 `SAFE_OFF`。
 *              原话真正要禁的是**两件别的东西**，分开说清才不会把纪律改成"看情况"：
 *                ① ⛔ **不得等任何锁**：`g_bus_afe` 按 S6 是**独占总线、无互斥量**（唯一加了锁的是
 *                   传感侧 `g_bus_sens`，C-96 ⑥），所以这一笔天生不会阻塞在锁上；
 *                ② ⛔ **不得有无限阻塞**：软 I²C 每次事务自带 `WAIT_LIMIT` 有界超时，
 *                   最坏几十 ms 级 ⇒ 它小于判活窗口，而**大于** 10ms 周期 ——
 *                   ⚠️ 这条是本文件新引入的**已知代价**，见 `actuator_close()` 调用点的注。
 *              仍然禁止：写 Flash、蜂鸣器/屏幕、任何 `delay_ms(≥11)` 忙等（G7-③ 机械守住）。
 *
 * @note        ★ 状态机与 §5.2 转移表（2026-09-28 D6 软件侧前置落的结构半）。
 *              ⛔ **今天这台机器恒为 `BMS_STATE_SAFE_OFF`，而那是正确状态、不是没写完**
 *              （`附录C` **C-103②**：为演示放开 = 同时违反 S3 / S4 / S13）。它停在原地是因为
 *              四条输入还没有真值：意图通道（Modbus 线圈，D9）｜`V_SW` 采样（D6 硬件）｜
 *              AFE 通道位（D7/D8，且 `afe_set_channel()` 在未就绪时**只允许保持断开**）｜
 *              三级判据与 `request_*`（D14）。⇒ 本轮交付 = **判据的骨架**（双判据、重试与冷却、
 *              锁定分类、事件出口）+ 编译期断言 + 门禁 G7-⑦；行为半属 **D6/D8/D12/D13**。
 *              转移表（与 `Tech_Design.md` §5.2 图逐条对应，⛔ 不得为了"看起来跑通了"提前放开）：
 *                SAFE_OFF      → PRECHARGE   意图位 && 无保护请求 && `V_SW`/AFE 通道有效 &&
 *                                    判据① `V_SW > 0.8×V_pack` && 组压落在可闭合区间 && S10 冷却已足
 *                PRECHARGE     → ON          判据② `V_SW < APP_CTR_PRECHARGE_DV_MV` 且未超时
 *                                    ⇒ ★ **先合主、后断预充**（make-before-break，§4.5 ④）
 *                PRECHARGE     → SAFE_OFF    超时 / 通道中途失效 / 执行器返回非 OK ⇒ 重试 +1
 *                PRECHARGE     → FAULT_LATCHED  重试超 `APP_CTR_PRECHARGE_RETRY`（§5.2 A 类）
 *                ON/PRECHARGE  → FAULT_LATCHED  `request_latch`（越限型，S11）
 *                ON            → ON(方向断)     `request_open`（环境型，**不锁定**，C-38）
 *                FAULT_LATCHED → SAFE_OFF      只有长按 `APP_CTR_LATCH_HOLD_MS` 一条路（S11）
 ****************************************************************************************************
 */

#include "./App/app_protect.h"
#include "./App/app_config.h"
#include "./App/app_log.h"
#include "./App/app_tasks.h"
#include "./BSP/ACTUATOR/actuator.h"   /* §4.5 契约：执行器拓扑对本层不可见（S15） */
#include "./SYSTEM/delay/delay.h"        /* `wdg_wait_flag()` 的轮询式延时（⛔ 不用 HAL_GetTick，见函数注） */
#include <stdio.h>                      /* 仅 `app_protect_wdg_start()` 的回读打印用（调度器之前） */

/* ==================== 独立看门狗（★ 全工程唯一允许出现 `IWDG->KR` 的文件） ====================
 * 24 小时运行的设备，"死机后不报警"比"报警误响"危险得多 ⇒ 狗不可关（S13，`APP_ENABLE_IWDG` 已删）。
 * ★ 超时由 8s 收到 2s（C-36）：8s 的问题不是"太长会漏检"，而是**保护链路在窗口里是瞎的** ——
 *   复位期间软件层不参与判断，全靠 AFE 硬件层兜底（S1），窗口越短越好。
 * ✅ LSI 已于 2026-09-28 实测回填：本颗芯片 **38604 Hz**（三次开机 38606/38604/38604，
 *   通路 = v0.15 取证构建 + `Tools/lsi_readout.sh`，`Build.md` §8.25）⇒ `APP_IWDG_RLR` 由它反算 = 1206。
 *   ⚠️ 手册给的仍是 30~60kHz ⇒ 本数是"这颗芯片、当时温度"的实测值，冷/热下的真实窗口仍会漂；
 *   要的是"别再按标称 40000 算"，不是"从此有了一个全温度域保证的数"。 */
#define IWDG_FEED()     do { IWDG->KR = 0xAAAA; } while (0)

/* 上一次"判活 + 喂狗"的时刻（HAL_GetTick 口径，与 `now_ms` 同钟）*/
static uint32_t s_last_feed_ms;

/* ==================== ★ C-110：卡死现场携带（备份域信箱） ====================
 * 口径、为什么必须是备份域、槽位占用的三处出处全在 `app_config.h` §12.6b，这里只写实现。
 * ⛔ 不得改用 RAM 静态变量携带：狗复位 = 系统复位，RAM 内容随之丢失（本项目 203 次咬狗实测：
 *   复位后能活下来的只有 VBAT 域 = 备份域寄存器 + Flash）。 */

/** @brief 取携带槽位（n = BKP DR 序号，从 1 数）—— 偏移出处 `stm32f103xe.h:199~200`（RESERVED0@0x00 ⇒ DR1@0x04）*/
#define CARRY_REG(n)    (*(__IO uint16_t *)(BKP_BASE + 0x04u + (4u * ((n) - 1u))))

/** @brief 打开备份域写通路。⛔ 不得依赖"别人已经开过"—— 调用次序一变就静默写不进去（同 C-106 那一类）*/
static void carry_bkp_unlock(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();
    __HAL_RCC_BKP_CLK_ENABLE();
    HAL_PWR_EnableBkUpAccess();     /* DBP：不置这一位，对 BKP DR 的写被硬件忽略，且不报错 */
}

/**
 * @brief       把"这一轮的卡死现场"投进备份域（卡死当轮调用，复位后那次开机读回）
 * @note        复位前那 ≤2s 里 `APP_WDG_CHECK_MS` 还能判 3~4 轮 ⇒ 只保留**年龄最大**的那一轮：
 *              "刚过线一点"与"超一个数量级"的修法完全不同，而先写进去的那条往往是前者。
 * @note        本函数在 TaskProtect 上下文里跑 ⇒ 只做三笔 16 位写（存储器映射，<1µs），
 *              ⛔ 不得在这里加 Flash 写、I²C、`printf` 或任何等待（S8/S25 + 本文件头注）。
 */
static void protect_stall_carry_write(uint8_t stalled)
{
    uint16_t age = (uint16_t)app_tasks_over_age_ms();     /* tick 口径 1ms/tick ⇒ 即 ms */
    uint16_t dl  = (uint16_t)app_tasks_over_dl_ms();

    carry_bkp_unlock();

    if ((CARRY_REG(APP_STALL_CARRY_SLOT_A) == APP_STALL_CARRY_MAGIC_A) &&
        (CARRY_REG(APP_STALL_CARRY_SLOT_B) == APP_STALL_CARRY_MAGIC_B) &&
        (age <= CARRY_REG(APP_STALL_CARRY_SLOT_AGE)))
    {
        return;                       /* 已有更旧/更狠的现场 ⇒ 不覆盖 */
    }

    CARRY_REG(APP_STALL_CARRY_SLOT_MASK) = stalled;
    CARRY_REG(APP_STALL_CARRY_SLOT_AGE)  = age;
    CARRY_REG(APP_STALL_CARRY_SLOT_DL)   = dl;
    /* 魔数最后写：写前三格可能还是半截旧值，读者只认魔数 */
    CARRY_REG(APP_STALL_CARRY_SLOT_A) = APP_STALL_CARRY_MAGIC_A;
    CARRY_REG(APP_STALL_CARRY_SLOT_B) = APP_STALL_CARRY_MAGIC_B;
}

/**
 * @brief       开机读回上一次卡死现场，补投一条 `EV_TASK_STALLED`（★ 本函数只应在调度器之前调用）
 * @param[out]  mask/age_ms/dl_ms 有携带时回填，供调用方打印（⛔ 本文件除 `app_protect_wdg_start()`
 *             外不得 `printf`，见头注 ⇒ 打印这一句的活交给 `app_main.c`）
 * @retval      0 = 没有携带（正常开机，或携带已被上一次开机擦掉）；1 = 已补投
 * @note        ★ **无论有效与否都把五格清零**：断过 VBAT 的备份域内容是"未定义"而不是 0，
 *              而上一版取证构建（v0.15）在 DR3~DR8 写过实测 counts/魔数/'CV' —— v0.16 不再做
 *              `BDRST`，那些垃圾会留在原地。擦一遍就把它变成"最多骗到第一次开机"。
 * @note        aux 语义与 `app_log.h` 对 `EV_TASK_STALLED` 的既有定义一致（aux_a = 年龄、
 *              aux_b = 窗口），⛔ 不得为携带另立语义 ⇒ 历史 1097 条与新读法保持同一把尺子。
 */
uint8_t app_protect_stall_carry_report(uint8_t *mask, uint16_t *age_ms, uint16_t *dl_ms)
{
    uint8_t valid;

    carry_bkp_unlock();

    valid = ((CARRY_REG(APP_STALL_CARRY_SLOT_A) == APP_STALL_CARRY_MAGIC_A) &&
             (CARRY_REG(APP_STALL_CARRY_SLOT_B) == APP_STALL_CARRY_MAGIC_B)) ? 1u : 0u;

    if (valid != 0u)
    {
        *mask   = (uint8_t)CARRY_REG(APP_STALL_CARRY_SLOT_MASK);
        *age_ms = CARRY_REG(APP_STALL_CARRY_SLOT_AGE);
        *dl_ms  = CARRY_REG(APP_STALL_CARRY_SLOT_DL);

        app_log_add(EV_TASK_STALLED, (uint16_t)(*mask), *age_ms, *dl_ms);
    }

    /* 归位（含无效时的垃圾擦除）*/
    CARRY_REG(APP_STALL_CARRY_SLOT_B)    = 0u;
    CARRY_REG(APP_STALL_CARRY_SLOT_A)    = 0u;
    CARRY_REG(APP_STALL_CARRY_SLOT_DL)   = 0u;
    CARRY_REG(APP_STALL_CARRY_SLOT_AGE)  = 0u;
    CARRY_REG(APP_STALL_CARRY_SLOT_MASK) = 0u;

    return valid;
}

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
     *   ⚠️ 与 C-104 同族、且更隐蔽：§12.6 那 11 条编译期断言算的是宏，
     *      宏永远是同一个数 ⇒ 一条都不响。所以本函数下面加了"回读 + 打印"，
     *      把窗口从"断言证明自洽"升级成"寄存器现状被看见"。
     *   上限 50ms：LSI 实测 38.6kHz 下 PVU 只需 3 个 LSI 周期 ≈ 5ms，这里是 10 倍余量。
     *   ★ 超时不再 `(void)` 丢掉：超时就等于"这一笔没写进去"，必须被看见（C-106 的出口）。 */
    pvu_to = wdg_wait_flag(IWDG_SR_PVU, 50u);

    IWDG->RLR = APP_IWDG_RLR;               /* ★ 由实测 LSI 反算：38604/64×2s = 1206（回填前 1250） */

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

/* ==================== 状态机（§5.2 转移表的代码落点，D6 软件侧前置） ==================== */
/* ★ 方向位图**只有一套**：`actuator.h` 的 `ACT_ST_CHG/DSG/PRE`，其位序 = §6.1 的 0x0020
 *   「执行器状态（CHG / DSG / 预充 / 锁定 / AFE 硬件层已动作）」。意图位、`request_open` 位、
 *   事件 code 位**全部复用**它 —— ⛔ 不得再引入第二套"充/放"编号：两个属主描述同一件事
 *   正是 C-97 F1（告警帧双写者）在数据域里的形态，而且它不报错、只在某一次取反时错。 */
#define PRT_MAIN_MASK       ((uint8_t)(ACT_ST_CHG | ACT_ST_DSG))

/* "还允许尝试闭合"的最低组压（10mV 域）= 单体欠压三级 × 串数。
 * ★ 为什么复用 `APP_BMS_CELL_UV_L3_MV` 而不是新写一个数（S14 + §3.1 ⑤）：再低的话 L3 早已
 *   断放并锁定，状态机根本走不到预充；而判据①的分母若取到一个不真实的组压（尤其 = 0），
 *   `V_SW > 0.8 × 0` 会被**任何非零读数**满足 = 判据①静默失效。§12.10.1 的断言用同一个式子。 */
#define PRT_PACK_FLOOR_10MV ((uint32_t)(APP_BMS_CELL_UV_L3_MV * APP_CELL_COUNT / 10))

/* 意图位：写者 = D9 的协议层（线圈 0x0000/0x0001，§6.1"写 1 后仍需过保护状态机，不是直接闭合"），
 * 读者 = 本任务。与 s_unlock_req 同形态（单向字节、天然幂等、⛔ 不加锁）。 */
static volatile uint8_t s_intent_bits;

static uint32_t s_pre_start_ms;                 /* 本次预充的起点（判超时用） */
static uint8_t  s_pre_retry;                    /* 连续失败次数，超限进锁定 */
static uint32_t s_last_close_ms[2];             /* [0]=充电方向 [1]=放电方向 的上次动作时刻（S10）*/
static uint8_t  s_last_act_rc;                  /* ★ 最近一次执行器动作的返回码（取证出口，见 getter 注）*/

/* ⛔ 不放任务栈：`bms_snapshot_t` ≈ 150B，而本项目纪律是全静态分配、栈余量按实测留 30%。
 * 唯一读者是本任务（TaskProtect 单线程）⇒ 不需要第二份副本。 */
static bms_snapshot_t s_snap;
static uint8_t        s_snap_valid;         /* 0 = 从未成功读到过一份可信快照（诊断接口据此"不知道"）*/

/**
 * @brief       S10 防振荡：这一路"通电方向"的动作是否已隔开 ≥ `APP_CTR_MIN_INTERVAL_MS`
 * @note        ★ 冷却只挂在**闭合方向**，⛔ 不得挂在断开：把 S10 套在断上 = 让保护动作延迟 3s
 *              执行，而本项目对"误动作 vs 拒动作"的排序是明确的（AGENTS §1：拒动作 = 起火）。
 *              断开本身幂等，重复断不构成振荡；而"断→合→断"的振荡已经被这一路的合门挡住。
 * @note        同一条判据还兼管 §5.2 要求的**预充重试冷却**（10Ω/10W 电阻不能连续挨 3×500ms）。
 */
static uint8_t prt_cooldown_ok(uint8_t idx, uint32_t now_ms)
{
    uint32_t t = s_last_close_ms[idx];

    if (t == 0u)
    {
        return 1u;                              /* 从未动作过（与 `acq_is_fresh()` 的 0 = 从未采集同口径）*/
    }

    return ((uint32_t)(now_ms - t) >= (uint32_t)APP_CTR_MIN_INTERVAL_MS) ? 1u : 0u;
}

/** @brief 按位图逐方向记动作时刻（合与断共用一张时间表：S10 说的是"动作"） */
static void prt_cooldown_mark(uint8_t dir_mask, uint32_t now_ms)
{
    if ((dir_mask & ACT_ST_CHG) != 0u)
    {
        s_last_close_ms[ACT_DIR_CHG] = now_ms;
    }

    if ((dir_mask & ACT_ST_DSG) != 0u)
    {
        s_last_close_ms[ACT_DIR_DSG] = now_ms;
    }
}

/** @brief 方向位图 → `bms_dir_t`。⛔ 调用前必须保证位图非空（空位图会被解成"放电"）。 */
static bms_dir_t prt_dir_of(uint8_t dir_mask)
{
    if ((dir_mask & PRT_MAIN_MASK) == PRT_MAIN_MASK)
    {
        return ACT_DIR_BOTH;
    }

    return ((dir_mask & ACT_ST_CHG) != 0u) ? ACT_DIR_CHG : ACT_DIR_DSG;
}

/** @brief 断开（A 类 = 全断；B 类 = 只断请求的那一路），并投一条带现场量的事件 */
static void prt_open(uint8_t dir_mask, uint32_t now_ms)
{
    s_last_act_rc = actuator_open(prt_dir_of(dir_mask));
    prt_cooldown_mark(dir_mask, now_ms);

    /* aux 两列 = 当时的组压 / 开关两端压差（§6.1 的 0x0032 同源）：事后复盘最先问的就是
     * "断的那一刻端子两端差多少"，而它无法在事后重测。⛔ 不得拿这两列另作他用。 */
    app_log_add(EV_MOS_OPEN, actuator_get_state(), s_snap.raw.pack_10mv, s_snap.raw.v_sw_10mv);
}

/** @brief 进锁定：先断后锁（§5.2「fault3 → SAFE_OFF + LATCH（先断后锁）」），原因码进事件 */
static void prt_latch(uint32_t now_ms)
{
    prt_open(PRT_MAIN_MASK, now_ms);            /* `actuator_open()` 内已含"无条件断预充" */

    s_state    = BMS_STATE_FAULT_LATCHED;
    s_pre_retry = 0u;

    app_log_add(EV_LATCH, s_snap.alarm.fault_bits, s_snap.raw.pack_10mv, s_snap.raw.v_sw_10mv);
}

/**
 * @brief 预充失败一次：重试 +1，超限进锁定，否则回 `SAFE_OFF` 等冷却后再试
 * @note  ⛔ **本条事件不携带"执行器返回码"**：那一列的语义在 `app_log.h` 已冻结为"第几次尝试"，
 *        改它会让历史与新读法不一致（同 `EV_TASK_STALLED` 的 aux 纪律）。成因由两处组合判读：
 *        aux 两列（组压 / `V_SW`）分辨"电容没充上来"，`app_protect_mos_diag()` 分辨"器件或链路"
 *        —— 后者是**内存量**，复位即失，⛔ 不得当成事后证据；要成证据得等 D9 的状态行把它写出来
 *        （`Build.md` §2 D9 批次①，任务 #69）。
 */
static void prt_precharge_fail(uint32_t now_ms)
{
    s_pre_retry = (uint8_t)(s_pre_retry + 1u);

    prt_open(PRT_MAIN_MASK, now_ms);

    app_log_add(EV_PRECHARGE_FAIL, s_pre_retry, s_snap.raw.pack_10mv, s_snap.raw.v_sw_10mv);

    if (s_pre_retry >= (uint8_t)APP_CTR_PRECHARGE_RETRY)
    {
        s_state = BMS_STATE_FAULT_LATCHED;      /* §5.2 A 类：预充重试超限 ⇒ 锁定（不是无限重试）*/
        s_pre_retry = 0u;
        app_log_add(EV_LATCH, s_snap.alarm.fault_bits, s_snap.raw.pack_10mv, s_snap.raw.v_sw_10mv);
    }
    else
    {
        s_state = BMS_STATE_SAFE_OFF;           /* 冷却由 S10 那张时间表在下一轮入口挡住 */
    }
}

/**
 * @brief       状态机一拍（§5.2 转移表）
 * @note        ⚠️ **本函数每 10ms 跑一次，而一次闭合动作最坏含一笔软 I²C 事务**
 *              （`afe_set_channel` → `g_bus_afe`，无锁但**有界**）。⇒ 判活窗口
 *              `APP_WDG_ALIVE_K × 10ms` 是否吃得住，属 D7/D8 上板后必须实测的一格
 *              （`附录C` C-105 的口径：**按一轮内不泵定点等待之和**，不按最长单段）。
 *              ⛔ 不得因为它"看起来只在异常路径上"就不测 —— C-105 就是这么漏的。
 */
static void protect_state_step(uint32_t now_ms)
{
    const bms_raw_t   *r;
    const bms_alarm_t *a;
    uint8_t            mos_now;
    uint8_t            cut_req;
    uint8_t            want;
    uint8_t            rc;
    uint32_t           pack_mv;
    uint32_t           vsw_mv;

    if (app_data_read(&s_snap) != 0u)
    {
        /* 1 = 从未发布（字段还是"全无效"初值）；2 = 连续重试仍被写者打断。
         * 两种都按"数据不可信"处理（§4.4）：本轮**不做任何动作** —— 既不闭合也不断开。
         * ⛔ 不得在这里"顺手断开"：`s_state` 与引脚现状是两个事实，读不到快照不等于
         *    保护条件成立，凭"读失败"去动作 = 凭空制造一次跳闸（AGENTS §1：误动作也是事故）。 */
        return;
    }

    r = &s_snap.raw;
    a = &s_snap.alarm;
    s_snap_valid = 1u;                  /* ★ 在"读完且可信"之后才立，⛔ 不得在 read 之前置 */

    /* ① 保护请求优先于一切转移（§5.2 的 A/B 两类；等级归属由告警层定，本层⛔ 不推断）*/
    if (a->request_latch != 0u)
    {
        if (s_state != BMS_STATE_FAULT_LATCHED)
        {
            prt_latch(now_ms);
        }

        return;
    }

    cut_req = (uint8_t)(a->request_open & PRT_MAIN_MASK);
    if (cut_req != 0u)
    {
        /* B 类（环境型，如 0℃ 禁充）：**只断请求的那一路、不锁定**（C-38 —— 原设计只有一个
         * request_latch，会把"冬天早上不能充电"锁成"必须长按 5s"）。*/
        prt_open(cut_req, now_ms);

        if (s_state == BMS_STATE_PRECHARGE)
        {
            s_state = BMS_STATE_SAFE_OFF;       /* 预充窗口被环境型切断打断 = 一次未完成，不算失败次数 */
        }

        return;                                 /* 有切断请求时本轮不尝试任何闭合 */
    }

    mos_now = actuator_get_state();             /* ★ 引脚现状，不是软件意图（§4.5 的回读出口）*/

    /* ② 意图 = 授权的来源。撤销授权必须断开（S3 的"需 MCU 明确发出允许"反过来就是
     *    "没有允许就不许导通"），而意图位与引脚现状不符时以**现状**为准去补断。 */
    if ((uint8_t)(mos_now & (uint8_t)~s_intent_bits & PRT_MAIN_MASK) != 0u)
    {
        prt_open((uint8_t)(mos_now & PRT_MAIN_MASK), now_ms);
        return;
    }

    want = (uint8_t)(s_intent_bits & PRT_MAIN_MASK);

    switch (s_state)
    {
        case BMS_STATE_SAFE_OFF:
            if (want == 0u)
            {
                return;                         /* ★ 今天走到这里就结束：没有意图通道（D9）*/
            }

            /* 不可知即不安全（§4.4 / §5.2 失效安全映射表）：三条输入任一缺失都不进预充。
             * V_SW → 禁止闭合；AFE → 单体与组压判据不可信（S2-① 由链路位表达）。 */
            if (((a->link_health & (uint8_t)(BMS_LINK_VSW | BMS_LINK_AFE)) != 0u) ||
                (r->v_sw_valid == 0u)           ||
                (r->pack_10mv == APP_MV_INVALID) ||
                ((uint32_t)r->pack_10mv < PRT_PACK_FLOOR_10MV) ||
                (prt_cooldown_ok(ACT_DIR_CHG, now_ms) == 0u)  ||
                (prt_cooldown_ok(ACT_DIR_DSG, now_ms) == 0u))
            {
                return;
            }

            /* ★ 判据①（§3.2）：V_SW 必须**还**接近组压才允许进预充。
             *   反直觉之处：V_SW ≈ 0 有两种成因 —— 负载电容已被充上（好事，但那不是本状态的
             *   起点）/ 采样线断、负载未接、或保护被大地回路旁路（S24 的失效现场）。
             *   ⛔ 只查判据②的机器正是"假装预充成功"然后合闸的那一台。 */
            vsw_mv  = (uint32_t)r->v_sw_10mv * 10u;
            pack_mv = (uint32_t)r->pack_10mv * 10u;

            if (vsw_mv <= ((pack_mv * (uint32_t)APP_CTR_VSW_RATIO_X10) / 10u))
            {
                return;                         /* 不进 PRECHARGE，也不记失败：这是"没资格开始" */
            }

            rc = actuator_precharge_start();
            if (rc != ACT_OK)
            {
                s_last_act_rc = rc;
                prt_precharge_fail(now_ms);
                return;
            }

            s_state       = BMS_STATE_PRECHARGE;
            s_pre_start_ms = now_ms;
            app_log_add(EV_PRECHARGE_START, (uint8_t)(s_pre_retry + 1u), r->pack_10mv, r->v_sw_10mv);
            break;

        case BMS_STATE_PRECHARGE:
            if (r->v_sw_valid == 0u)
            {
                prt_precharge_fail(now_ms);        /* 通道中途失效 = 不可知 ⇒ 中止，不猜 */
                break;
            }

            vsw_mv = (uint32_t)r->v_sw_10mv * 10u;

            /* ★ 判据②：预充把负载电容充到与组压只差 < ΔV 才算可以合主开关。
             *   ⚠️ 单位陷阱：宏是 **mV**，快照是 **10mV** ⇒ 这里必须 ×10 后再比（§12.10.1 有断言）。 */
            if (vsw_mv < (uint32_t)APP_CTR_PRECHARGE_DV_MV)
            {
                /* ★ make-before-break（§4.5 ④）：**先合主、后断预充**。反过来会在主开关断开的
                 *   那一瞬把全部负载电流从 10Ω 上硬切走 = 自己造一次浪涌。 */
                rc = actuator_close(prt_dir_of(want));
                if (rc != ACT_OK)
                {
                    s_last_act_rc = rc;
                    prt_precharge_fail(now_ms);
                    break;
                }

                prt_cooldown_mark(want, now_ms);

                /* ⛔ 这里不因 `precharge_stop()` 的返回码回滚主开关：主已合、目的已达成，
                 *   断预充失败只是"那颗电阻还在并联"，回滚反而制造一次带载分断。记码、留给出口。 */
                s_last_act_rc = actuator_precharge_stop();
                s_state       = BMS_STATE_ON;
                s_pre_retry   = 0u;
                app_log_add(EV_MOS_CLOSE, actuator_get_state(), r->pack_10mv, r->v_sw_10mv);
            }
            else if ((uint32_t)(now_ms - s_pre_start_ms) >= (uint32_t)APP_CTR_PRECHARGE_MS)
            {
                prt_precharge_fail(now_ms);        /* 超时 = 电容没充上来（电阻虚焊 / 负载短路 / 采样错）*/
            }

            break;

        case BMS_STATE_ON:
            /* 方向级重闭合：环境型切断恢复后（`request_open` 已在 ① 处为 0）意图仍在，
             * 而主回路本就导通 ⇒ 该方向不面对那颗负载电容，**不再走预充**，只受 S10 冷却约束。
             * ⚠️ 这一条到 D13 台架才能验（要真实负载 + 真实 V_SW），今天它是结构。 */
            {
                uint8_t missing = (uint8_t)(want & (uint8_t)~mos_now & PRT_MAIN_MASK);

                if ((missing != 0u) && (prt_cooldown_ok(ACT_DIR_CHG, now_ms) != 0u) &&
                    (prt_cooldown_ok(ACT_DIR_DSG, now_ms) != 0u))
                {
                    rc = actuator_close(prt_dir_of(missing));
                    if (rc == ACT_OK)
                    {
                        prt_cooldown_mark(missing, now_ms);
                        app_log_add(EV_MOS_CLOSE, actuator_get_state(), r->pack_10mv, r->v_sw_10mv);
                    }

                    s_last_act_rc = rc;
                }
            }

            /* ⚠️ **本次有意不实现的一条自检**（不擅自定数值，挂 D13 裁决）：
             *   状态已是 ON 而 `V_SW` 仍 ≈ 组压 ⇒ 主 MOS 实际上没导通（栅极驱动断了 / 管子坏了），
             *   这是"软件以为通着、电流其实不走"的最坏形态之一。它的判据需要一个"多接近组压算没导通"
             *   的门限，而**这个数字在 `PRD.md` §2 里没有** ⇒ ⛔ 不得由实现者顺手写一个百分比
             *   （AGENTS §3.1 ⑤：可抄的期望值必须带出处）。落点见 `Build.md` D13。 */
            break;

        case BMS_STATE_FAULT_LATCHED:
        default:
            /* 只有一条出路：长按解除（`protect_consume_unlock()`）。
             * ⛔ 不得在这里加"条件恢复自动解锁"—— S11 的锁定与 B 类的自动恢复是两类东西。 */
            break;
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

    /* 不喂 ⇒ ≤2s 复位。复位前把"是哪个任务卡的"投进日志（非阻塞队列，`app_log.h` 纪律①）。
     * ⚠️ **原来这里写过"TaskLog 每 50ms 落一条，窗口有 2000ms ⇒ 来得及"—— 那句已被 2026-09-28
     *    实机证伪**（`附录C` C-110）：`app_log_add()` 只进 RAM 队列，唯一的落盘者是 `task_log()`
     *    （prio 1），而卡死源在 prio ≥ 2 时 **TaskLog 根本没有机会跑** ⇒ 那条记录连同位图一起被
     *    狗吃掉。同一片 Flash 镜像里的定量证据 = 1097 条 `EV_TASK_STALLED` **全部 bit4**（容器
     *    prio 1，与 Log 同级才落得了盘）、**0 条带 bit1/2/3**，而 `EV_WDG_RESET` 有 203 条。
     *    ⇒ 同一个现场再走一条**不经过调度器**的通路 = `protect_stall_carry_write()`：写备份域，
     *      复位后那次开机读回来补投（`app_protect_stall_carry_report()`）。下面这句 `app_log_add`
     *      **保留**不删：prio ≤ 1 的停滞它仍然能落盘，而且它是"携带通路是否重复计数"的对照面。
     * ★ D5 归因轮的永久改动：aux 两列以前恒为 0 ⇒ 全区 152 条 TASK_STALLED 只能
     *   回答"谁超了"，答不出"超了多少"，而"刚好超一点"与"超一个数量级"的修法完全不同。
     *   现在把最坏那一路的 年龄/窗口 一起写进 Flash ⇒ 这条记录本身成为可离线判读的证据
     *   （`Tools/log_decode.sh` 直接解出来，不需要串口、不需要拍照）。 */
    {
        uint32_t over_a = app_tasks_over_age_ms();
        uint32_t over_b = app_tasks_over_dl_ms();

        app_log_add(EV_TASK_STALLED, stalled,
                    (over_a > 0xFFFFu) ? 0xFFFFu : (uint16_t)over_a,
                    (over_b > 0xFFFFu) ? 0xFFFFu : (uint16_t)over_b);

        protect_stall_carry_write(stalled);     /* ★ C-110：备份域携带 = 不经过调度器的那条通路 */
    }
}

void app_protect_step(uint32_t now_ms)
{
    /* ① 告警帧唯一写者（M6 交接过来的身份，C-97 F1）*/
    app_data_link_check(now_ms);

    /* ② 解锁请求处置 */
    protect_consume_unlock();

    /* ③ 状态机一拍（★ 必须排在 ① 之后：它读的 `link_health` / `request_*` 就是 ① 刚发布的那一帧。
     *    ⛔ 不得提到 ① 之前 —— 那会让它读到一个"上一拍的链路状态"，而 §5.2 的"禁止闭合"
     *    判据恰好挂在这一位上：晚一拍 = 多闭合一次不该发生的合闸。）*/
    protect_state_step(now_ms);

    /* ④ 判活 + 条件喂狗 */
    protect_wdg_check(now_ms);
}

/* ==================== 执行器属主的对外接口（D6 软件侧前置） ==================== */

/**
 * @brief       启动执行器层：把三路使能钉在"断开"并自检引脚模式
 * @note        ★ 与 `app_protect_wdg_start()` 同一类：**由 `app_main()` 在 `vTaskStartScheduler()`
 *              之前调用**，⛔ 不得放进 `app_protect_step()`。理由有两条，任一条都单独成立：
 *                ① 它内部那行回读打印依赖"当前没有并发"（同 §2.3 b) 的例外口径）；
 *                ② 顺序要求 = **执行器先进安全态，调度器才可能让任何任务去碰它**。
 *              ⚠️ 外部 10K 下拉（S5）本来就能保证"MCU 死了也断着"，本函数不是安全的前提，
 *              而是**把"引脚现在真的是低"变成一条被观测过的事实**（C-106 那一类的修法）。
 */
void app_protect_actuator_start(void)
{
    s_last_act_rc = actuator_init();
}

void app_protect_set_intent(uint8_t chg, uint8_t dsg)
{
    /* 只表达意图，⛔ 不做任何动作（§6.1 线圈的口径 / FR-COM-04 / S9）。
     * 写者 = D9 的协议层，读者 = 本任务的状态机；字节写单指令完成 ⇒ 不需要锁。 */
    s_intent_bits = (uint8_t)(((chg != 0u) ? ACT_ST_CHG : 0u) |
                              ((dsg != 0u) ? ACT_ST_DSG : 0u));
}

/**
 * @brief       ★ 读回当前**意图**（`app_protect_set_intent()` 的读侧，供 Modbus 线圈 0x0000/0x0001）
 * @note        ★ 为什么必须有它（而不是让协议层自己存一份影子）：
 *              "有开始没有结束的成对动作，评审时默认它是缺陷" —— 只写不读的意图通道，
 *              在总线上表现为 FC01 永远回 0（"主机写了允许放电、读回来却是 0"），
 *              而**影子副本**会在状态机某天自己清意图时静默与真值分叉（那时两条路径给出
 *              两个答案，且没有任何报错）。本函数是意图的**唯一读侧**。
 * @note        ⛔ 只读，不改任何状态；`s_intent_bits` 是单字节写 ⇒ 不需要锁。
 */
void app_protect_get_intent(uint8_t *chg, uint8_t *dsg)
{
    if (chg != NULL)
    {
        *chg = (((s_intent_bits & ACT_ST_CHG) != 0u) ? 1u : 0u);
    }

    if (dsg != NULL)
    {
        *dsg = (((s_intent_bits & ACT_ST_DSG) != 0u) ? 1u : 0u);
    }
}

uint8_t app_protect_mos_diag(uint8_t *state_bits, uint8_t *last_rc)
{
    if (s_snap_valid == 0u)
    {
        return 0u;                      /* ★ 从未成功读过快照 ⇒ 一个数都不给（不给"看着对"的 0）*/
    }

    *state_bits = actuator_get_state();
    *last_rc    = s_last_act_rc;

    return 1u;
}
