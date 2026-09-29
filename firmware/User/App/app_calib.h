/**
 ****************************************************************************************************
 * @file        app_calib.h
 * @brief       标定表区（`APP_CALIB_ADDR`）持久化 —— 规格 = `Tech_Design.md` §8「标定表区协议」（`附录C` C-127）
 * @note        1, ★★ **它是独立模块，⛔ 不并进 `app_cfg.c`**：两者的**生命周期不同** ——
 *                  参数区 = 操作员改门限 + `0x0304` 提交；标定表 = **标定会话产出、极少写**。
 *                  混在一起的后果是"谁该在什么时候写"失去单一答案。
 *              2, ★★ **「缺失 ≠ 故障」**（与参数区三态**刻意不同**）：本区两份副本都不可用（空白或 CRC 坏）
 *                  ⇒ **正常回退**到芯片全局 `gain/offset`（`afe_hal_read_scale()`）＋ 标记"未逐串标定"，
 *                  ⛔ **绝不拒绝启动**。理由：出厂/标定前用全局系数是**正确**行为（`afe.c` 已有
 *                  `[365, 396] µV/LSB` 的合理性检查）。★ 参数区那条"两区都坏就拒绝启动"之所以成立，
 *                  是因为**那些值决定跳闸门限**；**"不可知即不安全"的推论方向由"这个量用来做什么"决定。**
 *              3, ⛔ **BSP 层不得读本区**（本模块也不读写外设）：值由**应用层**在上电时读出来、
 *                  再喂给 AFE 层（`afe_set_cell_calib()`，见 §8 协议 (5)）。
 *              4, ★ **写者与时机**：只在 **`SAFE_OFF`** 且无故障时、由 **`TaskLog`** 调 `app_calib_poll()`
 *                  落盘（§4.4 规则 2 ＋ §2.3）。⛔ 擦写期间片内取指 stall ⇒ 绝不能发生在测量/保护路径上。
 *              5, ⛔ 中断里不得调用。
 *
 * @note        ⚠️ **本版状态 = 存储层已落、数据未接**：填表的人是 **D16 的标定流程**
 *              （逐串 k/b 怎么测出来，见 §8 协议 (6) 缺口 c）。⇒ 今天 `app_calib_valid()` 恒为 0，
 *              系统走全局系数 —— 这是**预期**，⛔ 不是"标定没做"的故障。
 ****************************************************************************************************
 */

#ifndef __APP_CALIB_H
#define __APP_CALIB_H

#include "./SYSTEM/sys/sys.h"
#include "./App/app_config.h"

#define APP_CALIB_VER       1u          /* ★ 改动载荷顺序/长度必须 +1（否则旧数据会被按新布局读） */
#define APP_CALIB_MAGIC     0x4341u     /* 'C','A'（半字口径） */
#define APP_CALIB_CELL_MAX  16u         /* 与 `APP_CELL_COUNT_MAX` 同口径；⛔ 改这里要同批改 §8 协议 (2) */
#define APP_CALIB_OCV_MAX   32u

/** 页内两份副本的起始偏移（半字）—— 分区表只给 1 页 2 KB ⇒ 同页内双副本，⛔ 不新增分区 */
#define APP_CALIB_COPY_HW   ((APP_FLASH_PAGE_SIZE / 2u) / 2u)   /* 512 半字 = 1 KB */

/** 只读信息（给界面/日志/取证用） */
typedef struct
{
    uint8_t  valid;         /* 1 = 有一份可信的标定表（本版预期为 0） */
    uint8_t  cells;         /* 表里记录的串数 */
    uint8_t  ocv_n;         /* OCV 点数 */
    uint8_t  copy_used;     /* 用的是哪一份：0 / 1；无有效表时为 0xFF */
    uint32_t seq;           /* 提交序号 */
    uint32_t calib_unix;    /* 标定时刻（Unix 秒；0 = 未知） */
    int16_t  t_calib_x10;   /* 标定时的电芯温度（0.1℃） */
} app_calib_info_t;

/**
 * @brief       上电装载：读两份副本、按"序号更大者"选一份
 * @note        ★ **不返回致命状态、⛔ 不拒绝启动**（口径 2）。两份都不可用时 `valid = 0`，
 *              调用方据此回退全局系数。
 * @note        ⛔ 只读 Flash，⛔ 不在启动序列里写（首次写入由标定会话触发）。
 */
void        app_calib_init(void);

/** @brief 只读：是否有一份可信的逐串标定表 */
uint8_t     app_calib_valid(void);

/** @brief 只读取证信息 */
void        app_calib_info(app_calib_info_t *o);

/**
 * @brief       取某串的 k/b（★ 未标定/越界 ⇒ 返回 0，调用方**必须**据此走全局系数）
 * @param       idx   串下标（0 起）
 * @param[out]  k_ppm 比例（ppm；1000000 = 不修正）
 * @param[out]  b_uv  偏置（µV）
 * @retval      1 = 该串有标定；0 = 没有（⛔ 不许把 0 当成"k=0"用）
 */
uint8_t     app_calib_get_cell(uint8_t idx, uint32_t *k_ppm, int32_t *b_uv);

/** @brief 改某串的 k/b（只改 RAM；由 `app_calib_commit_request()` 才落盘） */
uint8_t     app_calib_set_cell(uint8_t idx, uint32_t k_ppm, int32_t b_uv);

/** @brief 设置 OCV 点（★ 必须按 soc 升序；只改 RAM） */
uint8_t     app_calib_set_ocv(uint8_t n, const uint16_t *soc_x10, const uint16_t *ocv_mv);

/**
 * @brief       读出 OCV-SOC 表（给 `app_soc` 的锚定用）
 * @param[out]  n        点数（0 = 本表没有 OCV 数据）
 * @param[out]  soc_x10  按 soc 升序的测点（容量 ≥ `APP_CALIB_OCV_MAX`）；可传 NULL
 * @param[out]  ocv_mv   对应的 OCV（mV）；可传 NULL
 * @note        ⛔ 本接口只搬数据、**不做插值**（插值是消费者 `app_soc` 的事，见 §8 (2) 的"插值规则"缺口）。
 */
void        app_calib_get_ocv(uint8_t *n, uint16_t *soc_x10, uint16_t *ocv_mv);

/** @brief 清空 OCV 表（自检还原 / 标定表作废时用） */
void        app_calib_clear_ocv(void);

/** @brief 置"待提交"标志（⛔ 不写 Flash；真正的擦写在 `app_calib_poll()` 里） */
void        app_calib_commit_request(void);

/**
 * @brief       落盘执行体（**由 `TaskLog` 每拍调用**）
 * @param       in_safe_off  调用方给的"现在是不是 SAFE_OFF"
 * @retval      0 = 无事可做或已成功；1 = 还在排队（不在 SAFE_OFF）；2 = 擦写/回读校验失败（原副本保持不动）
 */
uint8_t     app_calib_poll(uint8_t in_safe_off);

/**
 * @brief       ★★ 离线注入自检（⛔ 不碰 Flash：编解码与判定都是**纯缓冲**操作）
 * @retval      0 = 全过；非 0 = 第几条用例失败（1 起数）
 * @note        判据：编→解往返一致 ／ ★ CRC 覆盖**判据字段**（改序号能被发现）／
 *              `cell_count != APP_CELL_COUNT` ⇒ 整表拒用 ／ 空白 ⇒ 不可用（回退全局）。
 */
uint16_t    app_calib_selftest(void);

/** 取证出口（⛔ 不参与判据） */
uint16_t    app_calib_selftest_result(void);
void        app_calib_diag(uint8_t *out, uint8_t n);
void        app_calib_pending(uint8_t *pending, uint32_t *seq);

#endif
