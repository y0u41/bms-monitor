/**
 ****************************************************************************************************
 * @file        app_protect.h
 * @brief       BMS 保护/状态属主：★ **告警帧的唯一写者** + **全工程唯一喂狗点**
 * @note        本文件是 `Tech_Design.md` §4.3（TaskProtect 职责）与 §5.2（四态状态机）的代码落点，
 *              D5（`Build.md` §8.4 M4/M6）建的骨架。
 *
 * @note        ★ 为什么 D5 就要建这个文件（它今天几乎不"保护"任何东西）：
 *              ① **告警帧必须只有一个写者**（`附录C` C-97 F1）。今天它唯一的写路径是
 *                 `app_data_link_check()`，而那个函数挂在**假数据源**尾部 —— 假源在通道交接完
 *                 就要整体删掉（D7~D11），删它的那天没人再 commit 告警帧，`link_health` 会
 *                 退化成"永远 0 = 所有链路永远健康"，而且不会有任何编译错误。
 *                 ⇒ M6 的第一动作就是把这个写者身份从假源手里接过来。
 *              ② **喂狗点必须收口成一处**（§8.4 M4）。搬进本文件而不是留在 `app_main.c`，
 *                 理由是判活的消费者就是最高优先级任务本身（S13：狗是最后兜底，
 *                 允许"多处无条件喂"= 允许单任务卡死永远检不出来）。
 *
 * @note        ⛔ **本文件今天不驱动任何执行器**（D6 才接 `actuator.h`），所以状态机恒停在
 *              `BMS_STATE_SAFE_OFF` —— 这是 **S3「开关常态断开」的代码表达**，不是没写完。
 *              任何"让状态离开 SAFE_OFF 好看一眼"的改动都等于绕过 S3。
 ****************************************************************************************************
 */

#ifndef __APP_PROTECT_H
#define __APP_PROTECT_H

#include "./SYSTEM/sys/sys.h"
#include "./App/app_data.h"      /* bms_state_t（§4.4 冻结结构体所在） */

/**
 * @brief       启动独立看门狗（IWDG）：配分频/重载、启动、立即喂一次
 * @note        ★ 从 `app_main.c` 搬来（D5 / §8.4 M4）：`IWDG->KR` 全工程只允许出现在本文件，
 *              由 `Tools/gate_check.sh` 的 G7-⑤ 机械检查。
 * @note        必须在 `app_tasks_create()` **之前**调用：狗要在调度器起来前就有效。
 *              ⛔ 本函数内部那次"立即喂"不是喂狗点，它只是消除启动瞬间的满窗。
 */
void        app_protect_wdg_start(void);

/**
 * @brief       TaskProtect 的周期体（10ms）
 * @param       now_ms  ★ 必须是 **`HAL_GetTick()` 口径**（与写者盖在快照上的时间戳同钟）。
 *                       ⛔ 不得传 `xTaskGetTickCount()`：两个计数器的起点差
 *                       = 调度器启动时刻（本项目实测约几百 ms），混用会让 `now - tick` 回绕成
 *                       一个巨大值 ⇒ **所有链路同时判成陈旧**。这是"改一行就全错"的那类坑。
 * @note        做三件事：① 告警帧单写者（`app_data_link_check()`）；② 每 `APP_WDG_CHECK_MS`
 *                 一次**条件喂狗**；③ 消费界面送来的解锁请求。⛔ 不得 `printf`（见 .c 头注）。
 */
void        app_protect_step(uint32_t now_ms);

/**
 * @brief       当前主回路状态（§5.2 四态）
 * @note        ★ 取数方式定稿（`Build.md` §2 D5 判据⑥c）：界面与协议经**本 getter** 取状态，
 *              ⛔ 不去改冻结的 `bms_alarm_t`（那会触发 G4 的协议/文档同步链），
 *              也⛔ 不许任何读者自己根据门限推断状态（§4.2 模块职责）。
 */
bms_state_t app_protect_state(void);

/**
 * @brief       报告"用户长按 KEY0 满 `APP_CTR_LATCH_HOLD_MS`"（S11 锁定的唯一解除入口）
 * @note        本函数**只置一个标志就返回**：它由 `app_ui.c` 的按键状态机在 UI 任务上下文里调用，
 *              真正的处置发生在 `app_protect_step()`（单写者原则，不在调用者线程里改状态）。
 *              单词标志天然幂等 ⇒ 跨任务不需要锁。
 */
void        app_protect_request_unlock(void);

#endif
