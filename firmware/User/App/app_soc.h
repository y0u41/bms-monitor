/**
 ****************************************************************************************************
 * @file        app_soc.h
 * @brief       派生层：**`bms_derived_t` 的唯一写者**（`Tech_Design.md` §4.2/§4.4 ／ `PRD.md` §2.2）
 * @note        1, ★★ **本工程唯一允许用浮点的业务模块**（§4.2 模块表原文）。★ 但本版实际用的是
 *                  **纯整数**：安时积分与极值都是整数运算，浮点只在将来做 OCV 拟合时才需要
 *                  ⇒ 口径："允许"不等于"必须"（整数更可复现、更好对表）。
 *              2, ★ **写者唯一**：`bms_derived_t` 只由本模块写。⛔ `app_data_fake_step()` 里原来
 *                  那一堆派生假值（soc/soh/ah/cycles/极值/差值/温升）**已随之删除** ——
 *                  它保留的只是"假 **raw** 源"，与 §4.4 的"每盒单写者"一致。
 *              3, ★ **本模块不写 raw 盒**（`app_acq` 的职责）、⛔ 不碰告警/执行器/通信/Flash。
 *              4, ⚠️ **本版的两条硬缺口（⛔ 不得算"已实现"，已登记进 `PRD.md` §6）**：
 *                  **a)** **SOC 的初值与锚定**：`FR-SOC-02` 要"安时积分 ＋ OCV 静置锚定 ＋ 满充锚定
 *                       ＋ 放空锚定 ＋ 温度容量修正" —— 其中 OCV-SOC 表值与"满充/放空"的判据
 *                       要等 **D16 标定**（真电池）⇒ 本版**只落积分器 + 锚定接口**，
 *                       未锚定期间 SOC 一律回 **`APP_SOC_UNKNOWN`（0xFFFF）**，⛔ 不冒充一个数值。
 *                  **b)** **SOH / 循环次数**：容量法要一次完整循环的实测容量（D16）⇒ 本版 `soh_x10`
 *                       沿用"未测容量前按 1000"（与 `app_data.c` 的 `der_mark_invalid()` 同口径），
 *                       `cycles` 恒 0。
 *              5, ★ **FR-SOC-03 的闸门**：电流通道无效 ⇒ **停止积分**且 SOC **不可信**
 *                  （"不可知即不安全"：宁可输出"未知"，也不输出一个正在漂的假数）。
 *              6, ⛔ 中断里不得调用。
 ****************************************************************************************************
 */

#ifndef __APP_SOC_H
#define __APP_SOC_H

#include "./SYSTEM/sys/sys.h"
#include "./App/app_data.h"

/** ★ SOC 不可信/未知的出口值（与 `APP_MV_INVALID` 同一条"无效值纪律"）。⛔ 不是 0 */
#define APP_SOC_UNKNOWN     0xFFFFu

/** 锚定来源（`FR-SOC-02` 的四条通路；本版只落接口，判据与表值等 D16） */
#define APP_SOC_ANCHOR_OCV      0u      /* 静置后的 OCV 查表 */
#define APP_SOC_ANCHOR_FULL     1u      /* 满充 */
#define APP_SOC_ANCHOR_EMPTY    2u      /* 放空 */
#define APP_SOC_ANCHOR_USER     3u      /* 人工/上位机置位 */

/**
 * @brief       ★★ 派生量计算（**纯函数**，供离线注入对表用）
 * @param       r        采集层原始量（只读；★ 用**原始值**，滤波值只喂积分，§4.4 裁决 C-120）
 * @param[in,out] d      派生量（**本函数是它的唯一写者**）；只更新本模块拥有的字段
 * @param       now_ms   当前毫秒（由调用者给 ⇒ 可注入）
 * @note        算的：`FR-SOC-05` 全部（最高/最低单体、压差、最高/最低温度、温差、温升速率）
 *              ＋`SOC`（安时积分，带 `FR-SOC-03` 闸门）。
 */
void        app_soc_update(const bms_raw_t *r, bms_derived_t *d, uint32_t now_ms);

/**
 * @brief       周期体（**接线入口**）：自己从快照盒取 back 缓冲、算完写回
 * @param       now_ms 当前毫秒
 * @note        ⛔ 本函数**不 commit**（发布是 `TaskAcq`/容器那一轮的既有 commit 负责，
 *              ⛔ 不允许出现第二个发布点 —— 那正是 C-97 F1 的失效形态）。
 */
void        app_soc_step(uint32_t now_ms);

/** @brief 上电清一次内部状态（积分累加器/温升历史/锚定标志） */
void        app_soc_init(void);

/** @brief 当前 SOC（0.1%）；**未锚定或电流通道无效时 = `APP_SOC_UNKNOWN`** */
uint16_t    app_soc_get_x10(void);

/** @brief 当前 SOC 是否可信（1 = 可信）。★ 与 `app_soc_get_x10() == APP_SOC_UNKNOWN` 必须一致 */
uint8_t     app_soc_trust(void);

/**
 * @brief       置一个 SOC 锚点（`FR-SOC-02` 的四条通路都走这里）
 * @param       kind  `APP_SOC_ANCHOR_*`；`soc_x10` 0.1%（>1000 会被夹到 1000）
 * @retval      0 = 已置位；非 0 = 入参非法（kind 越界）
 */
uint8_t     app_soc_set_anchor(uint8_t kind, uint16_t soc_x10);

/**
 * @brief       ★★ 离线注入自检（与三级引擎同一套手法：合成 raw 灌进**纯函数**，逐条对表）
 * @retval      0 = 全过；非 0 = 第几条用例失败（1 起数）
 * @note        ⛔ 只覆盖**派生量与积分器的逻辑**：不覆盖真实采样、不覆盖标定精度（±5% 要真电池）。
 */
/**
 * @brief       ★★ **OCV → SOC 查表**（纯函数；表来自标定表区）
 * @param       ocv_mv  静置后的开路电压（mV）
 * @retval      SOC（0.1%）／ `APP_SOC_UNKNOWN`
 * @note        ★ **插值规则（本项目裁决，`Tech_Design.md` §8 协议 (6) 缺口 a 的处理口径）**：
 *              相邻两点间**线性插值**；★ **表外一律返回"未知"，⛔ 不外推** ——
 *              外推会给出一个**看起来像数值**的 SOC，而它没有任何测量依据。
 * @note        ⛔ 本函数**不判"是否处于静置态"**：那是 `PRD.md` `FR-SOC-06` 的判据，
 *              其**电流门限与持续时长都还没有数值**（`§8 协议 (6)` 缺口 b）⇒ 由调用方决定何时调它。
 */
uint16_t    app_soc_ocv_to_soc(uint16_t ocv_mv);

/** @brief ★ 查表并锚定；表外/表不可用 ⇒ 返回 0（**不锚定**，SOC 仍为未知） */
uint8_t     app_soc_anchor_ocv(uint16_t ocv_mv);

/** @brief ★ 离线自检（⛔ 不需要器件；★ 单出口 ＋ 无条件还原，见 `附录C` C-134 口径 a） */
uint16_t    app_soc_ocv_selftest(void);
uint16_t    app_soc_ocv_selftest_result(void);

uint16_t    app_soc_selftest(void);

/** 取证出口（⛔ 不参与判据）：最近一次自检返回值 */
uint16_t    app_soc_selftest_result(void);

/** 取证出口（⛔ 不参与判据）：累计充/放电荷（10mA·ms）低 32 位，用于核对积分器 */
void        app_soc_charge_raw(uint32_t *chg, uint32_t *dis);

#endif
