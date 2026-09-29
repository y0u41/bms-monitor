/**
 ****************************************************************************************************
 * @file        app_main.h
 * @brief       应用主入口
 ****************************************************************************************************
 */

#ifndef __APP_MAIN_H
#define __APP_MAIN_H

#include "./SYSTEM/sys/sys.h"

void app_main(void);

/**
 * @brief       遗留业务切片的"每轮入口"（★ D5 迁移容器 `task_legacy()` 专用）
 * @note        这就是原 `app_main()` 里那个 `while(1)` 的**循环体**，逻辑一条未改；
 *              延时改由容器用 `vTaskDelay(APP_PERIOD_LEGACY_MS)` 提供（S12）。
 * @note        ⏳ **临时接口**：容器按 `Build.md` 的通道清单逐步搬空，D15 清空时本函数
 *              与 `task_legacy()`、`APP_PERIOD_LEGACY_MS` 一起删除。
 *              ⛔ 新业务不得往这里加，一律建到对应任务里。
 */
void app_main_service(void);

/** ★ 启动自检汇总出口（2026-09-29；编号见 `app_main.c` 的 `APP_SELF_*`） */
uint8_t     app_selfcheck_rc(uint8_t idx);

/** ★ v0.66：串口命令入口（极小：`?` 回放上电段原文 / `s` 状态行）；由 `task_log()` 周期调用 */
void        app_main_diag_poll(void);
void        app_selfcheck_report(uint8_t *out, uint8_t n);

#endif
