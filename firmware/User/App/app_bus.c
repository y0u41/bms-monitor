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
