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
 * @note        ⚠️ **2026-09-28（D6 软件侧前置）更正本行原口径**："本文件今天不驱动任何执行器"
 *              已经不成立 —— 执行器契约 `actuator.h` 的**唯一调用者**就是本文件，
 *              并由门禁 **G7-⑦** 机械守住（S9 的机械化：⛔ 不靠头注里的纪律，纪律不会自己守住）。
 *              ★ 但**这台机器仍然恒停在 `BMS_STATE_SAFE_OFF`**，而且那是**正确状态**：
 *              意图通道（D9）、`V_SW` 采样（D6 硬件）、AFE 通道位（D7/D8）、三级判据（D14）
 *              四条输入都还没有真值 ⇒ "任何让状态离开 SAFE_OFF 好看一眼"的改动等于绕过 S3/S4/S13
 *              （`附录C` C-103②）。本轮交付的是**判据的骨架**，行为半属 D6/D8/D12/D13。
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
 * @note        做四件事：① 告警帧单写者（`app_data_link_check()`）；② 消费界面送来的解锁请求；
 *                 ③ 状态机一拍（§5.2 转移表 —— ★ 唯一会驱动执行器的一处，见 .c 头注）；
 *                 ④ 每 `APP_WDG_CHECK_MS` 一次**条件喂狗**。⛔ 不得 `printf`（见 .c 头注）。
 */
void        app_protect_step(uint32_t now_ms);

/**
 * @brief       ★ **开机把三路执行器使能钉在"断开"并自检引脚模式**（D6 / §4.5 契约的 `actuator_init()` 出口）
 * @note        ⛔ 必须在 `vTaskStartScheduler()` **之前**、且与 `app_protect_wdg_start()` 同批由
 *              `app_main()` 调用：执行器先进安全态，调度器才可能让任何任务去碰它。
 *              顺序在 `app_protect_wdg_start()` **之后**（狗要能咬住"配执行器时卡住"这一段）。
 */
void        app_protect_actuator_start(void);

/**
 * @brief       报告主机的**意图**（§6.1 线圈 0x0000 允许放电 / 0x0001 允许充电）
 * @param       chg / dsg  非 0 = 该方向被授权
 * @note        ★ "写 1 后仍需过保护状态机，**不是直接闭合**"（§6.1 原话）—— 本函数只更新一个
 *              意图字节，真正的动作全部发生在 `app_protect_step()` 里（S9 / FR-COM-04）。
 *              ⚠️ 今天**还没有调用者**：Modbus 从机框架排 D9（`附录C` C-100）。
 *              撤销意图（写 0）会让状态机断开已导通的方向 —— 意图是授权的**唯一**来源。
 */
void        app_protect_set_intent(uint8_t chg, uint8_t dsg);

/**
 * @brief       ★ 读回当前**意图**（`app_protect_set_intent()` 的读侧）
 * @param[out]  chg / dsg  1 = 该方向被授权；允许传 NULL（只读一个方向）
 * @note        ★ 为什么它必须存在：Modbus 线圈 `0x0000/0x0001` 的 **FC01 读**要回真值，
 *              而"让协议层自己存一份影子"会在状态机某天自己清意图时静默分叉
 *              （两条路径给出两个答案，且没有任何报错）。★ 口径同本文件头部那句
 *              "契约里'有开始没有结束'的成对动作，评审时默认它是缺陷" —— 这一对是完整的。
 * @note        ⛔ 只读；单字节量 ⇒ 不需要锁。
 */
void        app_protect_get_intent(uint8_t *chg, uint8_t *dsg);

/**
 * @brief       执行器诊断出口：引脚现状位图 + 最近一次动作的返回码
 * @param[out]  state_bits  `actuator.h` 的 `ACT_ST_CHG/DSG/PRE`（= §6.1 的 0x0020 位序）
 * @param[out]  last_rc     `ACT_OK` / `ACT_ERR_*`
 * @retval      0 = **不可知**（状态机从未成功读到过可信快照 ⇒ 两个出参都不写）；1 = 有效
 * @note        ★ 为什么需要它：`last_rc` 目前**只在内存里**，复位即失 ⇒ 它现在还算不上证据。
 *              把它变成证据的位置 = D9 批次①的页 3 状态行与 `0x0020`（任务 #69），
 *              ⛔ 不得在本轮声称"执行器失败可事后复盘"。
 *              ⚠️ 与 `app_protect_state()` 的分工：那边回答"状态机认为自己在哪一态"，
 *              这边回答"引脚现在是什么电平"。两者不一致本身就是缺陷（见 `actuator_precharge_start()`
 *              的互锁），所以两个都得有出口。
 */
uint8_t     app_protect_mos_diag(uint8_t *state_bits, uint8_t *last_rc);

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

/**
 * @brief       ★ **开机读回上一次的卡死现场**（C-110，备份域携带）并补投一条 `EV_TASK_STALLED`
 * @param[out]  mask    停滞位图（bit i = `app_tasks.h` 的 `app_task_id_t` 第 i 个任务）
 * @param[out]  age_ms  最坏那一路当时的年龄（ms）
 * @param[out]  dl_ms   它自己的判活窗口（ms）
 * @retval      0 = 无携带（三个出参不写）；1 = 已补投，出参有效
 * @note        ★ 为什么需要它：判活检出后投的那条 `EV_TASK_STALLED` 只进 RAM 队列，落盘者是
 *              prio 1 的 `task_log()`；**prio ≥ 2 的任务卡死会把 TaskLog 饿死** ⇒ 记录永不进
 *              Flash，而狗咬完 RAM 就没了 ⇒ "是谁卡的"这件事在现场不可恢复（实测：1097 条
 *              TASK_STALLED 全为 bit4、0 条带 bit1/2/3，而 WDG_RESET 有 203 条）。
 * @note        ⛔ **调用位置**：必须在 `vTaskStartScheduler()` **之前**、且在 `app_log_boot_event()`
 *              **之后**（日志里就固定读成 `EV_WDG_RESET → EV_BOOT → EV_TASK_STALLED` 三联），
 *              并且**早于任何复位备份域的动作**。落点 = `app_main.c`。
 * @note        本函数无论有无携带都会把携带槽位清零（只报一次，并顺手擦掉上一版构建的残留）。
 */
uint8_t     app_protect_stall_carry_report(uint8_t *mask, uint16_t *age_ms, uint16_t *dl_ms);

#endif
