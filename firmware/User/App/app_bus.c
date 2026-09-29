/**
 ****************************************************************************************************
 * @file        app_bus.c
 * @brief       传感 I²C 总线互斥（静态互斥量 + 钩子登记），实现见 `app_bus.h` 的口径说明
 * @note        ★ 全工程唯一持有这把锁的地方。锁的**使用**在 `softi2c.c` 的三个事务入口，
 *              本文件只回答"怎么拿、拿不到怎么办"。
 ****************************************************************************************************
 */

#include "./App/app_bus.h"
#include "./App/app_config.h"
#include "./BSP/SOFTI2C/softi2c.h"
#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"      /* ★ taskENTER_CRITICAL/EXIT（g_bus_afe 的有界临界区锁） */

/* ⛔ 动态创建（xSemaphoreCreateMutex）在本工程会直接链接失败：C-42 已把动态分配降到链接级 */
static StaticSemaphore_t s_bus_mutex_buf;
static SemaphoreHandle_t s_bus_mutex = NULL;

/* ★ **并发与超时的计数器（永久件，⛔ 不随任何取证块删除）**
 *   为什么这里是"永久"而不是插桩的一部分：C-96 ⑥ 建的锁本身**不可观测** ——
 *   "锁 work 了"与"锁从头到尾没人抢过"在两总线 `fail` 计数里长得**一模一样**（都是 0）。
 *   `contend` 回答的正是"到底抢过没有"，它是 V8 那条判据的**存在性探针**（同 §3.1 ② 对待
 *   `IWDG->PR` 的口径）；而 D10 的 INA226 一旦上总线，这把锁每天都会被抢 —— 同一个计数器
 *   届时继续回答"这条总线有没有出现过拿不到锁"，所以它属于观测层，不属于取证层。
 *   ⚠️ 判读口径：`contend > 0` 只证明"两个调用者同时在要这条总线"，⛔ 不证明"不加锁就会坏"；
 *   `timeout > 0` 才是失败（那笔事务**没发生**，`softi2c.c` 的 `bus_take()` 已给它记一次总线 `fail`）。
 *   ⚠️ 单测里"抢过"的判定用 `uxSemaphoreGetCount()`（= `uxMessagesWaiting`，`queue.c:1894`
 *   那条路径就是靠它判可用）：互斥量 `uxLength=1` ⇒ **0 = 已被持有**。 */
static volatile uint32_t s_bus_contend;
static volatile uint32_t s_bus_timeout;

/**
 * @brief       事务入口的拿锁钩子（`softi2c.c` 的 `bus_take()` 调它）
 * @retval      0 = 拿到锁或无需锁; 1 = 超时（调用方将返回 `I2C_ERR_LOCK` 并给本总线记一次失败）
 * @note        ⚠️ 超时上限 = `APP_BUS_LOCK_TIMEOUT_MS`，⛔ 不得改成"无穷等"：
 *              持有者可能是优先级最低的 Ui 侧路径，无穷等等于让采集/报警等一个界面刷新。
 */
/* ★★★ 2026-09-29（D8 前裁决的执行）：**`g_bus_afe` 也必须串行化** ——
 *   因为**今天就已经有两个任务**碰这条总线：`TaskProtect` 的 `afe_set_channel()`（开合 MOS，
 *   是**安全动作**）＋ 容器的 `app_acq_afe_step()`（每 100ms 读单体/组压）。
 *   ⛔ **不能用互斥量**：`AGENTS.md` 铁律 **S6** 明写「保护路径不得被锁阻塞」。
 *   ⇒ 用**有界临界区**：它**不等待任何人、也永不失败**，只把本次事务（~0.5ms，由 `delay_us`
 *     的 SysTick 忙等保证有界）与别的任务隔开。
 *   ★ **语义澄清（本轮裁决）**：S6 禁止的是「**等锁**」，⛔ 不是「关中断」—— 它要防的是
 *     "保护路径在同一条总线上排队等 24C02/等别人"。临界区恰好**不产生排队**。
 *   ★ 为什么必须两边都包：单核上高优先级任务可以在**位中间**抢占低优先级任务
 *     ⇒ 只有**发起事务的双方都关中断**，才能保证一次事务不被切开。 */
static uint8_t app_bus_afe_lock(softi2c_bus_t *bus)
{
    (void)bus;
    taskENTER_CRITICAL();
    return 0;                       /* ★ 永不失败、永不等待 ⇒ 保护路径的写不会因"拿不到锁"而丢 */
}

static void app_bus_afe_unlock(softi2c_bus_t *bus)
{
    (void)bus;
    taskEXIT_CRITICAL();
}

static uint8_t app_bus_sens_lock(softi2c_bus_t *bus)
{
    (void)bus;                          /* 钩子签名要求带总线，本实现只服务 g_bus_sens 一把锁 */

    if (s_bus_mutex == NULL)
    {
        return 0;                       /* `app_bus_init()` 未跑（启动序列）⇒ 单线程，放行 */
    }

    /* ★ 调度器未起时不能碰队列原语（`xQueueSemaphoreTake` 会走 `configASSERT`）；
     *   而此刻也确实不需要锁 —— 只有一个调用者在跑。测试放在超时之前，故不消耗等待。 */
    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    {
        return 0;
    }

    /* 存在性探针：进内核等待**之前**先看一眼有没有人持着（读一个 volatile 字段，不是系统调用）*/
    if (uxSemaphoreGetCount(s_bus_mutex) == 0u)
    {
        s_bus_contend++;
    }

    if (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(APP_BUS_LOCK_TIMEOUT_MS)) == pdPASS)
    {
        return 0u;
    }

    s_bus_timeout++;                    /* ★ 与 `contend` 分开数：两者混成一个数就分不出"抢过"与"抢输" */
    return 1u;
}

/**
 * @brief       还锁钩子（与 `app_bus_sens_lock()` 成对，成对性由 `softi2c.c` 的 bus_take/bus_give 保证）
 */
static void app_bus_sens_unlock(softi2c_bus_t *bus)
{
    (void)bus;

    if (s_bus_mutex == NULL)
    {
        return;
    }

    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    {
        return;                         /* 与 lock() 同口径：那条路径根本没 take，give 会让计数漂走 */
    }

    (void)xSemaphoreGive(s_bus_mutex);
}

uint8_t app_bus_init(void)
{
    if (s_bus_mutex == NULL)
    {
        s_bus_mutex = xSemaphoreCreateMutexStatic(&s_bus_mutex_buf);
    }

    if (s_bus_mutex == NULL)
    {
        return 1;
    }

    /* ★ 只注册传感这一条。`g_bus_afe` 上锁 = 让保护路径排队等 24C02，S6/S1~S3 明确禁止。 */
    softi2c_set_lock(&g_bus_sens, app_bus_sens_lock, app_bus_sens_unlock);

    /* ★★ 2026-09-29：**AFE 那条也装**（临界区版，见上面的语义澄清）——
     *   原设计让它恒为 NULL，但那是在「只有一个任务碰它」的前提下成立的；
     *   而 `TaskProtect` 的 `afe_set_channel()` 早就在用这条总线了。 */
    softi2c_set_lock(&g_bus_afe, app_bus_afe_lock, app_bus_afe_unlock);

    return 0;
}

/* ==================== 取证出口（⛔ 不参与任何判据）====================
 * 口径同 `app_tasks.c` 的那五个 getter：位图/计数只回答"有没有发生过"，不回答"该不该动作"。
 * 消费点 = 调试器直读（`Build.md` §8.4 V8）与 D9 的串口命令入口。 */
uint32_t app_bus_contention_count(void)
{
    return s_bus_contend;
}

uint32_t app_bus_lock_timeout_count(void)
{
    return s_bus_timeout;
}
