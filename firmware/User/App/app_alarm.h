/**
 ****************************************************************************************************
 * @file        app_alarm.h
 * @brief       BMS **三级告警判定**（`Tech_Design.md` §5.3 ／ `附录C` **C-123** ／ `PRD.md` §2.3 判据）
 * @note        1, ★★ **本文件只管 BMS 三级**。**环境两级**已按 `C-38` 搬到 `app_env_alarm.{c,h}`
 *                  （那里的 `alarm_*` 名字与语义一字未改）⇒ ⛔ 两套门限不得再混进一个模块：
 *                  混在一起时 `*_HYST_*` 到底指"环境舒适区"还是"电芯温度"靠读代码分不清，
 *                  而混淆的后果是**误切断或拒动**。
 *              2, ★ **门限数值的唯一来源 = `PRD.md` §2.3**，其宏形式在 `app_config.h` §12.7
 *                  （`APP_BMS_*`，含硬件层值与 30 余条编译期层次断言）⇒ 本模块**只引用宏**，
 *                  ⛔ 任何 `.c` 里不得出现第三次数值（`AGENTS.md` §5.1）。
 *              3, ★★ **判据本体是纯函数**（`app_alarm_eval`）：只读 `r`/`d`、只写 `a`，
 *                  **⛔ 不写快照、⛔ 不碰执行器 GPIO、⛔ 不调 `delay`／不算 SOC／不写 Flash**（§4.2 的"禁止"列）。
 *                  ★ `now_ms` **由调用者给**（⛔ 内部不调 `HAL_GetTick()`）⇒ 同一个函数可以在
 *                  **离线注入**下被逐条对表（`app_alarm_selftest()` 就是为此存在的）。
 *              4, ★ **调用者 = `TaskProtect`**（§4.4 发布机制表："判据在 `TaskProtect`（10ms）内同步执行，
 *                  顺带写快照"）⇒ 报警帧的**写者仍然只有 TaskProtect 一处**。
 *              5, ⛔ **本模块不得直接读外设**：AFE 的量只能从 `bms_raw_t` 来，硬件层故障只能从
 *                  `hw_fault_bits`／`hw_layer_state` 传入（S15 在本模块的落点）。
 *              6, ⛔ 中断里不得调用（不是可重入时序安全的）。
 ****************************************************************************************************
 */

#ifndef __APP_ALARM_H
#define __APP_ALARM_H

#include "./SYSTEM/sys/sys.h"
#include "./App/app_data.h"
#include "./App/app_config.h"

/**
 * @brief       判据标识（★ 顺序 = 原因串并列时的**第三级判据**：故障位序号最小者优先，
 *              所以本枚举顺序必须与 `Tech_Design.md` §6.2 的位号顺序一致）
 * @note        ⛔ 增删一项会同时影响：计时器数组大小、原因串并列选择、以及自检的期望表（计数耦合）。
 */
typedef enum
{
    APP_ALM_Q_CELL_OV = 0,      /* 单体过压（取最高单体），位 0 */
    APP_ALM_Q_CELL_UV,          /* 单体欠压（取最低单体），位 1 */
    APP_ALM_Q_PACK_OV,          /* 组过压，位 2 */
    APP_ALM_Q_PACK_UV,          /* 组欠压，位 3 */
    APP_ALM_Q_I_CHG,            /* 充电过流（含持续时间），位 4 */
    APP_ALM_Q_I_DIS,            /* 放电过流（含持续时间），位 5 */
    APP_ALM_Q_HW,               /* 硬件层已动作（AFE 内部），位 6（软件另置 4/5 中对应者） */
    APP_ALM_Q_T_CHG_OT,         /* 充电过温，位 8 */
    APP_ALM_Q_T_CHG_UT,         /* 充电过冷，位 9 */
    APP_ALM_Q_T_DIS_OT,         /* 放电过温，位 10 */
    APP_ALM_Q_T_DIS_UT,         /* 放电过冷，位 11 */
    APP_ALM_Q_T_DIF,            /* 温差过大，位 12 */
    APP_ALM_Q_T_RISE,           /* 温升速率异常，位 13 */
    APP_ALM_Q_NOCHG,            /* ★ 0℃ 禁充（**独立判据**，B 类：断充电但⛔ 不锁定） */
    APP_ALM_Q_COUNT
} app_alm_q_t;

/**
 * @brief       ★★ 三级判定（纯函数）
 * @param       r        采集层原始量（★ 保护判据**只许用原始值**：滤波值只喂 SOC 积分，§4.4 裁决 C-120）
 * @param       d        派生量（`cell_max_mv` / `cell_min_mv` / `delta_mv` / `temp_max_x10` …）
 * @param[in,out] a      告警帧：**读入** `hw_fault_bits`/`hw_layer_state`（硬件层事实由外部给），
 *                       **写出** level / fault_bits / request_open / request_latch / derate_* / reason[]
 * @param       now_ms   当前毫秒；★ 由调用者给 ⇒ 可注入（判据可离线对表）
 * @note        ★ 等级归属（§5.3 表）：一级 ⇒ 降额 50%；二级 ⇒ 降额 20% + 发起降额请求并计时；
 *              **二级超时未应答 ⇒ 升级为断开对应方向**；三级 ⇒ 断对应方向 + 锁定。
 *              ⚠️ **0℃ 禁充是 B 类**：断充电但 `request_latch` 保持 0（条件回升自动恢复）。
 *              ⚠️ **融合只升不降**（`FR-ALM-05` 可实现子集；"压差大"那一条无数值、不实现）。
 */
void        app_alarm_eval(const bms_raw_t *r, const bms_derived_t *d, bms_alarm_t *a, uint32_t now_ms);

/** @brief 上电清一次内部状态（计时器／迟滞／降额超时）。⛔ 不碰 `bms_alarm_t`（那是 TaskProtect 的帧） */
void        app_alarm_init(void);

/**
 * @brief       ★★ 离线注入自检：用**合成输入序列**逐条对表（不需要任何外部硬件）
 * @retval      0 = 全过；非 0 = **第几条用例失败**（1 起数）
 * @note        ★ 为什么必须有它：判据要的量此刻全都不存在（AFE/INA226/NTC 未接），
 *              但判据本体是**纯函数** ⇒ 可以在**本板本固件**上把合成 raw/derived 灌进去，
 *              逐条核对"等级 / 动作位 / 锁定 / 原因串"。★ 它测的是**这份编译产物**，不是副本。
 *              ⛔ 它的结论**只覆盖判据逻辑**：⛔ 不覆盖真实采样、⛔ 不覆盖执行器动作、
 *              ⛔ 不能替代 `F2` 的台架故障注入（`AGENTS.md` §3.1 ⑤ 的口径）。
 * @note        ★ 判定结果同时落在 `app_alarm_selftest_result()`（RAM 可读）——
 *              ⛔ 不得只有串口一个出口（本机没有 COM 通路，这是验收轮抓过的缺陷类）。
 */
uint16_t    app_alarm_selftest(void);

/** 取证出口（⛔ 不参与判据）：最近一次 `app_alarm_selftest()` 的返回值 */
uint16_t    app_alarm_selftest_result(void);

/** 取证出口（⛔ 不参与判据）：各判据当前等级（`n` 个字节，按 `app_alm_q_t` 顺序） */
void        app_alarm_levels(uint8_t *out, uint8_t n);

#endif
