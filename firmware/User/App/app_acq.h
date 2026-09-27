/**
 ****************************************************************************************************
 * @file        app_acq.h
 * @brief       采集层（`Tech_Design.md` §4.2 的 `app_acq`）—— **只写原始量**
 * @note        职责边界（§1.2 单向数据流：BSP → app_acq → 快照 → app_soc/app_alarm/app_protect）：
 *              ① 本模块是 SHT30 的**唯一读者**：两阶段读法（S12）意味着"谁调 `sht30_measure()`
 *                 谁推进流水"，两处同时调会让一方永远拿到 BUSY —— 所以环境温湿度只能有一个属主，
 *                 别人（含 `app_main.c` 的遗留报警通路）⛔ 不得再直接驱动 SHT30，只读本模块的取值口；
 *              ② ⛔ 不判告警、不算 SOC、不写 Flash、不碰执行器 GPIO（§4.2 的"禁止"列）；
 *              ③ 写快照只走 `app_data` 的 raw 盒（`app_data_raw_write()` → 填 → `_commit()`），
 *                 ⛔ 不写 alarm 盒 —— 那个盒的写者是 TaskProtect（每盒单写者，见 `app_data.c` 头部）；
 *              ④ ★ **滤波归本模块**（FR-ACQ-06）：环境温湿度已接定点一阶低通，
 *                 D7 的单体电压、D10 的电流**复用同一个原语**。
 *                 ⚠️ 但 `PRD.md`:193 要求"保护用原始值 / 滤波值只喂 SOC 与显示"，
 *                 而 `bms_raw_t` 每通道**只有一个值字段** ⇒ 两者现在装不下并存。
 *                 这条冲突**已登记、未在 D4 擅自解决**（选项与判据见 `app_config.h` §12.11），
 *                 D7 接 AFE 之前必须裁决 —— ⛔ 不要靠"给电压也滤一层"糊过去，
 *                 滤波值一旦进了 `app_protect` 就是把三级动作的时间预算吃掉（同一条 193 行的理由）。
 *
 * @note        ★ D3 只接入**有真实硬件源**的那一条通道（环境温度 = SHT30）。
 *                 其余通道保持"由 D2 的假数据源填写"，各通道在 `Build.md` §2 里的落点：
 *                   单体电压 / 组压（AFE，`g_bus_afe`）      → D7
 *                   电流 + 冗余组压（INA226，`g_bus_sens`）  → D10
 *                   电芯表面 NTC（PA5/PA6 两路 ADC）         → D11
 *                   V_SW 开关两端压差（PC0）                 → D11
 *                 ⛔ 不要为了"看起来完整"给没接的通道写占位实现 —— 无效值口径（`APP_MV_INVALID` /
 *                 `temp_valid` 位 0 / `acq_is_fresh()`）就是"没有数据"的正确表达。
 ****************************************************************************************************
 */

#ifndef __APP_ACQ_H
#define __APP_ACQ_H

#include "./SYSTEM/sys/sys.h"

/**
 * @brief       采集层初始化：探测环境传感器在位
 * @note        ★ 不初始化 I2C 总线（总线是共享资源，由启动序列 `softi2c_init(&g_bus_sens)` 负责一次）
 */
void app_acq_init(void);

/**
 * @brief       采集节拍（在主循环/TaskAcq 里按固定周期反复调用）
 * @param       now_ms  当前毫秒（`HAL_GetTick()`）
 * @note        内部按 `APP_ACQ_ENV_PERIOD_MS` 限流；首拍只发命令，次拍才有数据（两阶段读法）
 */
void app_acq_step(uint32_t now_ms);

/* ==================== 只读取值口（给遗留报警通路与界面/协议） ==================== */
int16_t  app_acq_env_temp_x10(void);        /* 环境温度, 0.1℃ */
uint16_t app_acq_env_humi_x10(void);        /* 环境湿度, 0.1%RH */
uint8_t  app_acq_env_valid(void);           /* 1 = 至少有一帧好数据且尚未连续失败到判死 */

#endif
