/**
 ******************************************************************************
 * @file    app_tasks.h
 * @brief   FreeRTOS 任务定义、静态内存分配与启动
 * @note    对应设计：Tech_Design.md §4.3（任务划分与看门狗策略）
 *                     AGENTS.md  §1.2 S12/S13、§3 G7（全工程禁用动态分配）
 *
 * 本模块的职责边界（Tech_Design.md §4.2）：
 *   只做"任务创建 + 静态内存 + 心跳"，不做任何业务判断。
 *   ⏳ 迁移期（D5~D15）例外：容器 `task_legacy()` 要调 `app_main_service()`，
 *      所以本模块的 .c 暂时 #include 业务头；容器清空后恢复"单向依赖、不碰业务"。
 *      登记在 `app_tasks.c` 头注，删除日期 = D15。
 ******************************************************************************
 */
#ifndef __APP_TASKS_H
#define __APP_TASKS_H

#include "FreeRTOS.h"
#include "task.h"

/* 任务在心跳数组中的下标（顺序必须与 APP_TASK_COUNT 一致） */
typedef enum
{
    APP_TASK_PROTECT = 0,
    APP_TASK_ACQ,
    APP_TASK_COMM,
    APP_TASK_LOG,
    APP_TASK_UI,        /* ⏳ 迁移期（D5~D15）这个身份由 `task_legacy()` 占用 */
    APP_TASK_COUNT
} app_task_id_t;

/* 任务句柄，供调试与看门狗统计使用 */
extern TaskHandle_t g_task_protect;
extern TaskHandle_t g_task_acq;
extern TaskHandle_t g_task_comm;
extern TaskHandle_t g_task_log;
extern TaskHandle_t g_task_ui;      /* ⏳ 迁移期内存的是 `task_legacy()`（名字 "Legacy"）*/

uint8_t app_tasks_create(void);     /* 创建全部 5 个任务；返回 0 = 成功 */
void    app_tasks_heartbeat(uint8_t idx);   /* 任务体内每轮调用一次（记 tick） */

/** @brief 判活位图：bit i = 第 i 个任务已落后超过 `APP_WDG_ALIVE_K × 自身周期`（C-34）
 *         位图本身就是"是哪个任务卡死"的现场证据，直接进 `EV_TASK_STALLED` 的 payload。
 *         ⛔ 只有 `TaskProtect` 调它；界面要读结果请用下面两条 getter。 */
uint8_t app_tasks_stalled_mask(void);

/** @brief 取证出口（⛔ 不是判据）：最近一次判活算出的位图（单位：位，同 `stalled_mask`）。
 *         存在的理由 = 判活只往日志里投"当时谁超了"，而界面需要一个**一直看得见**的读数；
 *         直接调 `app_tasks_stalled_mask()` 会让 UI 任务也算一遍（顺带污染峰值统计）。 */
uint8_t app_tasks_stalled_last(void);

/** @brief 取证出口（⛔ 不是判据）：**历次判活时刻**见过的最大"距上一次心跳"，单位 ms。
 *         ★ 为什么是峰值而不是瞬时值：容器是**一阵一阵**超的（超的瞬间才投日志），
 *         界面每 200ms 采样一次基本只会采到"一切正常"—— 实测就是这样白拍一张照片。
 *         峰值回答的才是排障要的问题：差 5ms（余量不足）与差 3000ms（某段不泵钩子的
 *         长等待）是两个完全不同的修法。
 *         ⚠️ 1ms/tick（`configTICK_RATE_HZ = 1000`）⇒ tick 数即 ms，与判活同一个钟。
 *         ⚠️ 起点 = 本次上电（`s_beat_tick` 在创建任务时清零），跨上电比较无意义。
 *         ⏳ 随页 3 的调试行一起删除（D9 串口命令入口建好后由串口承担）。 */
uint32_t app_tasks_worst_age_ms(uint8_t idx);

/** @brief 第 idx 个任务的判活窗口 = `APP_WDG_ALIVE_K × 自身周期`，单位 ms（配上一条用） */
uint32_t app_tasks_deadline_ms(uint8_t idx);

#endif /* __APP_TASKS_H */
