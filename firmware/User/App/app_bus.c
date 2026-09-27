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

    return (xSemaphoreTake(s_bus_mutex, pdMS_TO_TICKS(APP_BUS_LOCK_TIMEOUT_MS)) == pdPASS) ? 0u : 1u;
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
