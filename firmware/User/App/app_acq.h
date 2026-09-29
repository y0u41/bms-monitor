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
 *                 D7 的单体电压、D10 的电流**复用同一个原语** `filt_lpf_step()`。
 *                 ✅ **2026-09-29 裁决已定（`Tech_Design.md` §4.4 末 / `附录C` C-120）**：
 *                 **`bms_raw_t` 语义冻结为"原始值（未滤波）"、字段零改动**；**滤波后的 BMS 量
 *                 不进快照**，就留在**本模块**的静态量里、经**只读取值口**暴露（沿用本文件已有的
 *                 `app_acq_env_*()` 三口的同一种形态）——
 *                 ★ 之所以这样就够：`app_soc` 是**在 `TaskAcq` 里被调用**的 ⇒ 滤波值**不过任务边界**，
 *                 ⛔ 不必为它付"扩冻结结构 + 改 §6.1 协议 + G4 全链对账"的代价。
 *                 ⛔ 仍然成立的两条禁令：**滤波值一旦进了 `app_protect` 就是吃掉三级动作的时间预算**
 *                 （PRD:193）；**原始值无效时必须清本模块的滤波器累加器**，⛔ 不得把"不可知"平滑成"已知"。
 *                 ⚠️ 若将来出现**第二个跨任务消费者**（例如界面要吃滤波值），说明该前提破了
 *                 ⇒ 届时要**重开裁决**，⛔ 不是悄悄往快照里加字段。
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

/**
 * @brief       ★★ **D7 交接：AFE 通道**（单体电压 + 组压 → 快照的 raw 盒）
 * @param       now_ms  当前毫秒
 * @note        ★ 本函数**只在 `afe_read_cells()` 与 `afe_read_pack()` 都返回 `AFE_OK` 时才写快照**
 *              ⇒ AFE 不在位/未 BOOT/CRC 口径错时**一个字都不动**（快照保持原样，通道自然变陈旧、
 *              `BMS_LINK_AFE` 亮起）—— 这就是"不可知即不安全"在本通道的落点（§4.4）。
 *              ★ 因此它今天（AFE 未接）对在板行为**零影响**；到货那天 AFE 一应答就自动接管。
 * @note        ⛔ 本函数不判告警、不算 SOC、不碰 Flash、不碰执行器（§4.2 的禁止列）。
 * @note        ⚠️ **D7 当天必须同批删掉假源里这三处**（⛔ 不许只删值不删时间戳，
 *              否则假源每拍刷新 `tick_ms_cell` ⇒ 通道永远"新鲜"、`BMS_LINK_AFE` 永不亮，
 *              那就是 `附录C` **C-97 F3** 登记的那笔债）：
 *                ① `app_data.c` 里 `r->cell_mv[i] = mv;`（与同段的 `cell_valid` 设置）
 *                ② `r->pack_10mv = …;` 与 `r->pack_10mv_aux = …;`（INA226 那一路归 D10）
 *                ③ `r->tick_ms_cell = now_ms;`（★ 时间戳与值**同批**删）
 */
void app_acq_afe_step(uint32_t now_ms);

/**
 * @brief       ★★ **D10 交接：INA226 通道**（电流 + 冗余组压 → 快照的 raw 盒）
 * @note        ★ 与 `app_acq_afe_step()` 同一条纪律：**只在 `ina226_read()` 返回 `INA226_OK` 时才写**
 *              ⇒ INA226 不在位时一个字都不动（通道自然陈旧 ⇒ `BMS_LINK_INA226` 亮起）。
 * @note        ⚠️ `pack_10mv_aux` 没有独立时间戳 ⇒ 它与电流共用 `tick_ms_curr`（**同一颗器件的同一次读**，
 *              这是对的；⛔ 若将来两路读数分了家，必须给 aux 单独一个时间戳）。
 * @note        ⚠️ **D10 当天必须同批删掉假源里这四处**（值 ＋ 时间戳一起删，`C-97 F3`）：
 *                `r->pack_10mv_aux = …` ／ `r->curr_10ma = …` ／ `r->curr_valid = 1u` ／ `r->tick_ms_curr = now_ms`。
 */
void app_acq_ina_step(uint32_t now_ms);

/**
 * @brief       ★★ **D11 交接：NTC 通道**（两路电芯表面温度 → 快照）
 * @note        同一条纪律：**只在 ADC 读到合法码且 `ntc_code_to_temp_x10()` 成功时才写**；
 *              未接 NTC 时阈值落在量程外 ⇒ 回 `NTC_ERR_RANGE` ⇒ **一个字都不写**（通道自然陈旧）。
 *              ⛔ 不要把超量程的码当“很冷/很热的读数”写进去 —— 那是把不可知伪装成已知。
 * @note        两路写入下标：`APP_TEMP_IDX_CELL_NTC`(0) 与 `APP_TEMP_IDX_CELL_NTC2`(2)；
 *              环境(1) 的属主是 `app_acq` 的 SHT30 那一路（D3）。
 * @note        ⚠️ 本函数**不**推进 `tick_ms_temp[]` 以外的任何时间戳；也⛔ 不判告警。
 */
void app_acq_ntc_step(uint32_t now_ms);

/**
 * @brief       ★★ NTC 通道的**稳定性门限**（ADC 码）—— 两次相邻读数之差的上限
 * @note        为什么非要有它：本轮实测发现“**悬空引脚**”的码会落在合法量程内
 *              （本板未接 A 采样板时 `s_ntc_ok` 在上涨）⇒ **单次读数分不出“真 NTC”与“悬空”**。
 *              而真 NTC 在 100ms 间隔下码值几乎不动（几个码），悬空引脚则上百码地乱跳 ⇒
 *              **稳定性就是第二形态**。℃ 对应码：近 25℃ 时 **16 码 ≈ 0.4℃**，而电芯 100ms 内不可能变 0.4℃。
 * @note        ⚠️ 首次读数（无前一次可比）**不发布**：一个无历史的读数无法自证稳定。
 */
#define APP_ACQ_NTC_STABLE_CODES   16u

/**
 * @brief       ★★ 连续多少次稳定才**发布**（`APP_ACQ_NTC_STABLE_CODES` 内的连续次数）
 * @note        为什么还要这一层：本轮实测“只要求两次相近”时，**悬空引脚**仍有 ≈0.8 次/秒
 *              能凑出两个相近的码 ⇒ 会偶尔往快照里写一个**假温度**。要求**连续 3 次**后，
 *              偶然事件要连着发生三次才能骗过去 ⇒ 概率再降一个量级。
 * @note        ⚠️ 这不是掩盖问题而是提高门槛：**真板子上该失效模式本来就不存在**
 *              （分压电阻在位时 NTC 拔掉 = 10K 上拉到 Vref ⇒ 码 ≈ 4095 ⇒ 超量程被拒）。
 */
#define APP_ACQ_NTC_STABLE_N       3u

/** ★ 取证出口（⛔ 不参与判据）：NTC 交接点被调次数 / 成功次数 */
void app_acq_ntc_stat(uint16_t *try_n, uint16_t *ok_n);

/** ★ 取证出口（⛔ 不参与判据）：INA226 交接点被调次数 / 成功次数 */
void app_acq_ina_stat(uint16_t *try_n, uint16_t *ok_n);

/** ★ 取证出口（⛔ 不参与判据）：AFE 交接点被调次数 / 成功次数 */
void app_acq_afe_stat(uint16_t *try_n, uint16_t *ok_n);

/* ==================== 只读取值口（给遗留报警通路与界面/协议） ==================== */
int16_t  app_acq_env_temp_x10(void);        /* 环境温度, 0.1℃ */
uint16_t app_acq_env_humi_x10(void);        /* 环境湿度, 0.1%RH */
uint8_t  app_acq_env_valid(void);           /* 1 = 至少有一帧好数据且尚未连续失败到判死 */

#endif
