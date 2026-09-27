/**
 ******************************************************************************
 * @file    app_tasks.c
 * @brief   FreeRTOS 任务定义、静态内存分配、心跳与条件喂狗判据
 * @note    对应设计：Tech_Design.md §4.3 / AGENTS.md §3 G7
 *
 * ★ 关键约束（违反即回滚）：
 *   1) AGENTS G7：全工程禁止 malloc / pvPortMalloc
 *      → FreeRTOSConfig.h 里 configSUPPORT_DYNAMIC_ALLOCATION = 0
 *      → 本文件所有任务的栈与 TCB 都是静态数组，一律用 xTaskCreateStatic
 *   2) AGENTS S12：任务体内禁止 HAL_Delay / delay_ms 长时间阻塞，用 vTaskDelay
 *   3) 任务优先级：Protect(4) > Acq(3) > Comm(2) > Log(1) = Ui(1)
 *      （Tech_Design.md §4.3；参数集中在 app_config.h —— 单一配置源）
 *
 * ⚠️ **本文件的 include 清单现在是"带删除日期的例外"**（`app_tasks.h` 头注写的
 *      "本模块不 #include 业务头"到 D15 之前不成立）：
 *      D5 的迁移容器 `task_legacy()` 必须调 `app_main_service()`，容器就必须看得见业务头。
 *      ⇒ 迁移期允许，⛔ 不得长期保留：容器每搬空一个通道就往对应任务里搬，
 *      D15 容器清空时本文件的 `app_main.h` 一并删除，届时恢复"只创建任务、不碰业务"的边界。
 ******************************************************************************
 */
#include "app_tasks.h"
#include "app_config.h"
#include "./App/app_main.h"       /* ⏳ 迁移容器专用（D15 随 task_legacy 一起删除） */
#include "./App/app_protect.h"    /* TaskProtect：告警帧写者 + 唯一喂狗点 */
#include "./App/app_log.h"        /* TaskLog：事件落盘的唯一消费者 */
#include <string.h>

/* ==========================================================================
 * 一、静态内存池
 *    栈单位 = StackType_t = 4 字节，与 Tech_Design.md §4.3 的"字"约定一致
 * ==========================================================================*/
static StackType_t  s_stk_protect[APP_STK_PROTECT];
static StaticTask_t s_tcb_protect;

static StackType_t  s_stk_acq[APP_STK_ACQ];
static StaticTask_t s_tcb_acq;

static StackType_t  s_stk_comm[APP_STK_COMM];
static StaticTask_t s_tcb_comm;

static StackType_t  s_stk_log[APP_STK_LOG];
static StaticTask_t s_tcb_log;

static StackType_t  s_stk_ui[APP_STK_UI];
static StaticTask_t s_tcb_ui;

/* 空闲任务：FreeRTOS 必需，且静态分配模式下必须由应用提供内存 */
static StackType_t  s_stk_idle[APP_STK_IDLE];
static StaticTask_t s_tcb_idle;

TaskHandle_t g_task_protect = NULL;
TaskHandle_t g_task_acq     = NULL;
TaskHandle_t g_task_comm    = NULL;
TaskHandle_t g_task_log     = NULL;
TaskHandle_t g_task_ui      = NULL;

/* ==========================================================================
 * 二、判活（条件喂狗的判据，验收项 WDG-2 / WDG-3）
 *    ★ 口径（附录C · C-34）：**逐任务 deadline**，不是"自上次检查以来都推进过"。
 *      TaskProtect 每 10ms 就会问一次，而其余任务的周期是 20~100ms —— 用"都要推进过"
 *      做判据会在一半以上的检查里把活着的任务判成死，结果是**设备正常运行也 2s
 *      一次复位**，现象与硬件故障同形（C-34 原缺陷）。
 *      现在的判据只跟"该任务自己的周期"挂钩，与调用频率无关：
 *          now - 该任务最后一次推进 > APP_WDG_ALIVE_K × 自身周期  ⇒  判死
 *    ⚠️ tick 是 32 位无符号（1ms/tick，49.7 天回绕），一律用 `now - then` 的
 *      回绕差值比较，**不许**写成 `now > then + limit`（回绕那天会误判）。
 *    ⚠️ 本函数用 `xTaskGetTickCount()`，而快照时间戳用 `HAL_GetTick()` —— **两钟不许混**
 *      （机理与后果见 `app_protect.h` 的 `app_protect_step()` 参数说明）。
 * ==========================================================================*/
static volatile uint32_t s_beat_tick[APP_TASK_COUNT];   /* 各任务最近一次推进的 tick */

/* 取证用（⛔ 不参与判据）：历次判活时刻见过的最大年龄 + 最近一次的位图 */
static volatile uint32_t s_worst_age[APP_TASK_COUNT];
static volatile uint8_t  s_stalled_last;

/* 下标顺序必须与 app_task_id_t 完全一致（同一份宏，不另写字面量） */
static const uint32_t s_period_ms[APP_TASK_COUNT] =
{
    APP_PERIOD_PROTECT_MS,
    APP_PERIOD_ACQ_MS,
    APP_PERIOD_COMM_MS,
    APP_PERIOD_LOG_MS,
    /* ⏳ 迁移期：`APP_TASK_UI` 这个身份当前由 `task_legacy()` 占用，它的节拍是
     *   `APP_PERIOD_LEGACY_MS`(20ms)，⛔ 不是 `APP_PERIOD_UI_MS`(50ms)。填错这一个数的
     *   后果是判活余量算成 200ms ⇒ 容器真卡死时要到第 10 拍才报，等于把 WDG-3 的
     *   检测窗口拉长 2.5 倍。容器拆完（≤D15）时本行改回 `APP_PERIOD_UI_MS`。 */
    APP_PERIOD_LEGACY_MS
};

void app_tasks_heartbeat(uint8_t idx)
{
    if (idx < (uint8_t)APP_TASK_COUNT)
    {
        s_beat_tick[idx] = xTaskGetTickCount();
    }
}

uint8_t app_tasks_stalled_mask(void)
{
    uint32_t now = xTaskGetTickCount();
    uint8_t  mask = 0u;
    uint8_t  i;

    for (i = 0; i < (uint8_t)APP_TASK_COUNT; i++)
    {
        uint32_t age = (uint32_t)(now - s_beat_tick[i]);   /* 1ms/tick ⇒ 差值即 ms */

        /* 峰值只涨不落：容器是"一阵一阵"超的，判活时刻的年龄才是现场证据 */
        if (age > s_worst_age[i])
        {
            s_worst_age[i] = age;
        }

        if (age > pdMS_TO_TICKS(s_period_ms[i] * APP_WDG_ALIVE_K))
        {
            mask |= (uint8_t)(1u << i);     /* 位图直接给出"是哪个任务卡死"，供事件日志用 */
        }
    }
    s_stalled_last = mask;
    return mask;
}

/* ==================== 取证出口（⛔ 不是判据）====================
 * 位图只说"谁超了"，不说"超了多少" —— 而连续多条 TASK-BAD 需要区分的正是
 * "刚好超一点（余量不足）"与"超一个数量级（某段不泵钩子的长等待）"，
 * 这两者的修法完全不同，且 ⛔ 不许靠猜。
 * ⏳ 随页 3 的调试行一起删除（D9 串口命令入口建好后由串口承担）。 */
uint8_t app_tasks_stalled_last(void)
{
    return s_stalled_last;
}

uint32_t app_tasks_worst_age_ms(uint8_t idx)
{
    if (idx >= (uint8_t)APP_TASK_COUNT)
    {
        return 0u;
    }

    return s_worst_age[idx];
}

uint32_t app_tasks_deadline_ms(uint8_t idx)
{
    if (idx >= (uint8_t)APP_TASK_COUNT)
    {
        return 0u;
    }

    return s_period_ms[idx] * APP_WDG_ALIVE_K;
}

/* ==========================================================================
 * 三、FreeRTOS 要求的应用回调（静态分配模式下必须由应用实现）
 * ==========================================================================*/
void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t  **ppxIdleTaskStackBuffer,
                                   uint32_t      *pulIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer   = &s_tcb_idle;
    *ppxIdleTaskStackBuffer = s_stk_idle;
    *pulIdleTaskStackSize   = APP_STK_IDLE;
}

/* ★ BMS 补丁（附录C · C-41）：栈溢出钩子（configCHECK_FOR_STACK_OVERFLOW = 2）
 *
 *   ⚠️ 本函数由 stack_macros.h 的检查宏在**任务切换路径的临界区内**调用，
 *      等同中断上下文 → AGENTS S8 禁止 printf / Flash 擦写 / 阻塞等待。
 *      （S25 也独立禁止在这里写 Flash：擦写会全局停顿 20~80ms，
 *        看门狗窗口只有 2s，且此刻内核正处在临界区。）
 *
 *   处置：把任务名抄进 volatile 全局（供调试器在复位前挂上去看），
 *   随后关中断停住，让 IWDG 复位 —— 宁可"带信息地死"，
 *   也不能"继续跑并踩坏相邻内存"（在 BMS 里被踩坏的很可能就是保护判据变量）。
 *   ⛔ RAM 内容会随复位丢失，所以"是哪个任务溢出"只能靠调试器或
 *      复位后用 RCC->CSR 的 IWDGRSTF 标志知道"发生过看门狗复位"（D5 记日志）。 */
static volatile char     s_overflow_name[16];
static volatile uint32_t s_overflow_count;

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    uint32_t i;

    (void)xTask;                        /* 句柄此刻不可信，只用名字 */
    s_overflow_count++;

    for (i = 0; i < (sizeof(s_overflow_name) - 1u); i++)
    {
        if (pcTaskName[i] == '\0')
        {
            break;
        }
        s_overflow_name[i] = pcTaskName[i];
    }
    s_overflow_name[i] = '\0';

    taskDISABLE_INTERRUPTS();

    for (;;)
    {
    }
}

/* ==========================================================================
 * 四、五个任务的主体
 *    ★ D5（§8.4）之后这里**不再是骨架**：Protect / Log / 迁移容器三个已经在真跑业务，
 *      Acq / Comm 仍是空转 —— 空转是**有意的**，理由写在各自的函数里，不是"忘了写"。
 *      本段的"函数签名 + 优先级 + 周期"自此冻结。
 *    ★ 每个任务的第一句都是 `app_tasks_heartbeat()`：**先记"我起来了"再干活**，
 *      这样"卡在活里"能在一个 deadline 内被检出；反过来先干活则最坏多等一整拍。
 * ==========================================================================*/

/* ---------- TaskProtect（10ms，prio 4）：告警帧唯一写者 + 全工程唯一喂狗点 ---------- */
static void task_protect(void *pv)
{
    (void)pv;
    for (;;)
    {
        app_tasks_heartbeat(APP_TASK_PROTECT);

        /* 三件事：① `app_data_link_check()`（告警帧写者身份，C-97 F1）；② 每
         *   `APP_WDG_CHECK_MS` 一次判活 + **条件**喂狗；③ 消费界面的长按解锁请求。
         * ⚠️ 入参必须是 `HAL_GetTick()` 口径（两钟不许混，见 `app_protect.h`）。
         * ⛔ 本任务内不得 printf / 写 Flash / 碰 I²C / 等锁 —— 它一阻塞，保护链整条停摆。 */
        app_protect_step(HAL_GetTick());

        /* TODO(D6): 四态状态机离开 SAFE_OFF（预充 + `actuator.h` 使能输出，S9 的唯一出口）
         * TODO(D8): AFE ALERT 上升沿 → 先解码再处置（S26）；ISR 只置标志，位图经
         *           `afe.h` 的跨实现契约取，⛔ 寄存器名不得出现在本层（S15） */

        vTaskDelay(pdMS_TO_TICKS(APP_PERIOD_PROTECT_MS));
    }
}

/* ---------- TaskAcq（100ms，prio 3）---------- */
static void task_acq(void *pv)
{
    (void)pv;
    for (;;)
    {
        app_tasks_heartbeat(APP_TASK_ACQ);

        /* ⏳ 迁移期**故意空转**：`app_acq_step()` 仍留在容器里由 `app_periodic()` 调用。
         *   两个原因，任一条成立都不能提前搬：
         *   ① raw 盒只能有一个写者（C-97 F1）。现在 `app_periodic()` 里
         *      `app_data_fake_step()` 与 `app_acq_step()` 是**同一轮里的两个 commit**，
         *      把它们拆到两个任务 = 一个盒两个写者 = 撕裂帧；
         *   ② 顺序不可颠倒：真实帧必须写在假帧之后（假源是全量覆盖，后写者赢）。
         *   ⇒ D7~D11 逐通道交接时，"假源 + 采集"作为**一对**同时搬进本任务，
         *     假源整体删除那天（D11 末）本任务才成为 raw 盒的唯一写者。 */

        vTaskDelay(pdMS_TO_TICKS(APP_PERIOD_ACQ_MS));
    }
}

/* ---------- TaskComm（10ms，prio 2）---------- */
static void task_comm(void *pv)
{
    (void)pv;
    for (;;)
    {
        app_tasks_heartbeat(APP_TASK_COMM);

        /* TODO(D9): Modbus RTU 收帧/回帧（app_modbus.c）—— IDLE 中断 + DMA，
         *           DE/RE 用 TC 中断释放；10ms 轮询，ISR 内不调 xxxFromISR。
         * ⛔ D5 的 M7（"假数据先进 task_comm"）本轮**不做**：`附录C` C-100 要求
         *   任何对外应答先证明"是这台机器在应答"，而 RS485/3 路 UART 的收发主体
         *   还没接（D9）—— 现在建一个回假值的从机，测出来的"通"是假的。
         * TODO(加分): WiFi/MQTT/OneNET 上报（迁移期仍在容器里，见 `app_main.c`） */

        vTaskDelay(pdMS_TO_TICKS(APP_PERIOD_COMM_MS));
    }
}

/* ---------- TaskLog（50ms，prio 1）：事件队列的唯一消费者 ---------- */
static void task_log(void *pv)
{
    (void)pv;
    for (;;)
    {
        app_tasks_heartbeat(APP_TASK_LOG);

        /* ★ 一次页擦除 20~40ms 且 **CPU 全局停顿**（S25：与任务优先级无关）。
         *   所以 `app_log_poll()` 全工程只能有本任务这一个调用者 ——
         *   两个同优先级调用者会被时间片轮转在擦除中途切走，"整页写"的假设就没了。
         *   原容器每轮那次调用已删（`app_main.c` 头注 ⑤）。
         * ⚠️ 入参同为 `HAL_GetTick()`：日志时间戳与快照时间戳必须同钟，否则上位机
         *   看到的"事件时刻"和"数据时刻"差一个调度器启动偏移。 */
        app_log_poll(HAL_GetTick());

        vTaskDelay(pdMS_TO_TICKS(APP_PERIOD_LOG_MS));
    }
}

/* ---------- 迁移容器（20ms，prio 1）：⏳ D5 引入 / D15 删除 ---------- */
/**
 * @note        ★ 这就是"原 `app_main()` 的 `while(1)`"，逻辑一条未搬（§8.4 M2 的偏差说明见
 *              `app_main_service()` 的头注）。它占用 `APP_TASK_UI` 这个心跳身份。
 * @note        ⛔ 新业务不得往容器里加，一律建到对应任务（`app_main.h` 同一条）。
 * @note        ⚠️ 心跳只有一个属主 = 本任务。联网等待期间 `app_poll()`（注册给 ESP8266
 *              驱动的泵回调）也会刷同一个槽，但那是**同一任务的调用栈里**发生的，
 *              不算第二个属主；代价 = 那一段对本任务放弃判活（`app_main.c` 头注 M5 ①）。
 */
static void task_legacy(void *pv)
{
    (void)pv;
    for (;;)
    {
        app_tasks_heartbeat(APP_TASK_UI);     /* 迁移期占用 Ui 身份（判活周期见 s_period_ms 注） */
        app_main_service();                   /* ⏳ 遗留超循环的循环体，逐日搬空 */
        vTaskDelay(pdMS_TO_TICKS(APP_PERIOD_LEGACY_MS));
    }
}

/* ==========================================================================
 * 五、任务创建
 * ==========================================================================*/
uint8_t app_tasks_create(void)
{
    g_task_protect = xTaskCreateStatic(task_protect, "Protect", APP_STK_PROTECT,
                                       NULL, APP_PRIO_PROTECT, s_stk_protect, &s_tcb_protect);
    g_task_acq     = xTaskCreateStatic(task_acq,     "Acq",     APP_STK_ACQ,
                                       NULL, APP_PRIO_ACQ,     s_stk_acq,     &s_tcb_acq);
    g_task_comm    = xTaskCreateStatic(task_comm,    "Comm",    APP_STK_COMM,
                                       NULL, APP_PRIO_COMM,    s_stk_comm,    &s_tcb_comm);
    g_task_log     = xTaskCreateStatic(task_log,     "Log",     APP_STK_LOG,
                                       NULL, APP_PRIO_LOG,     s_stk_log,     &s_tcb_log);
    /* ⏳ 迁移期：`g_task_ui` 这个句柄槽给容器用，名字给 "Legacy"（V1 数心跳时按名字认，
     *   别按"有没有 Ui"认 —— 找不到 Ui 是正常的）。 */
    g_task_ui      = xTaskCreateStatic(task_legacy,  "Legacy",  APP_STK_UI,
                                       NULL, APP_PRIO_UI,      s_stk_ui,      &s_tcb_ui);

    if ((g_task_protect == NULL) || (g_task_acq  == NULL) || (g_task_comm == NULL)
     || (g_task_log     == NULL) || (g_task_ui   == NULL))
    {
        return 1;   /* 任一失败即视为失败（静态分配下只可能是参数写错） */
    }

    memset((void *)s_beat_tick, 0, sizeof(s_beat_tick));
    return 0;
}
