/**
 ****************************************************************************************************
 * @file        app_cfg.h
 * @brief       门限参数 Flash 持久化 + 版本迁移（`Tech_Design.md` §4.2 的 `app_cfg`，D14 的 DoD② 依赖它）
 * @note        1, ★ **本文件的唯一职责**：把 `app_config.h` §12.7 那一批**编译期默认值**变成
 *                  "**默认值 = 出厂值，Flash 参数区 = 现场值**"的两层结构。⛔ 本模块**不解析通信报文**
 *                  （§4.2 的"禁止"列）—— Modbus 侧只调 `app_cfg_set()` / `app_cfg_get()`。
 *              2, ★★ **页内布局 / 上电三态 / A-B 交替写协议 / 运行期层次重校验 / 80 格分组 / 版本迁移 /
 *                  掉电安全 —— 全部在 `Tech_Design.md` §8「门限参数区协议」**（2026-09-29 新增，
 *                  `附录C` **C-121**）。本文件只落码，⛔ 不复述那张表（`AGENTS.md` §5.1）。
 *              3, ★★ **关于"范围与跨项校验"（本表就是它们的唯一来源）**：§8 协议 (4) 写明
 *                  "编译期断言管默认值自洽，运行期校验管现场改出来的值自洽" ⇒ 本文件的
 *                  `APP_CFG_ITEMS` 表**同时**承载：默认值（引用 `app_config.h` 的宏名，⛔ 不抄数值）、
 *                  单项上下界、以及 `app_cfg_check_cross()` 里的**跨项不等式**。
 *                  ⛔ 任何别处再写一套范围/不等式 = 第二个落点。
 *              4, ⛔ 本模块**不碰日志区**（另一个写者）与**累计量区**（另一个写者）；`stmflash_write()`
 *                  是"整页擦 + 整页写回"⇒ **一页只能有一个写者**，本模块独占 `APP_PARAM_A_ADDR` 与
 *                  `APP_PARAM_B_ADDR` 两页。
 *              5, ★ **CRC 复用不新写**：参数区用 **CRC-16/CCITT-FALSE（poly 0x1021, init 0xFFFF）**，
 *                  与事件日志同算法 ⇒ 实现**只有一份**（`app_log.c` 里那份，经 `app_log_crc16_hw()` 暴露）。
 *                  ⛔ 不得在 `app_cfg.c` 里再抄一份（"同一算法两个落点"就是 §5.1 要防的那类漂移）。
 *              6, ⛔ 中断里不得调用（S8：Flash 擦写）。
 *
 * @note        ★ 逐项清单口径（§8 协议 (5) 的机械自查）：**`app_config.h` 里每一个"数值门限宏"
 *              必须在 `APP_CFG_ITEMS` 表里出现且仅出现一次**。显式白名单（**非门限、故意不入表**）：
 *              `APP_BMS_ASSERT_ASC/DESC/HYST`（断言辅助）、`APP_BMS_CHEM`、
 *              `APP_BMS_TEMP_ACTIVE_COUNT` / `APP_BMS_TEMP_COUNT_MAX` / `APP_CELL_COUNT*`（结构规模）、
 *              `APP_BMS_*_VALID_*`（合理性上下界本身，不是可配门限）、`APP_FILT_*`（滤波参数，属采集算法）。
 *              ⚠️ 离线自查必须**带这份白名单**，否则"合法不出现在表里"的宏会让那条自查永远为假。
 ****************************************************************************************************
 */

#ifndef __APP_CFG_H
#define __APP_CFG_H

#include "./SYSTEM/sys/sys.h"
#include "./App/app_config.h"

/* ==================== 返回码 ==================== */
#define APP_CFG_OK              0u
#define APP_CFG_ERR_RANGE       1u      /* 单项越界 ⇒ Modbus 回异常码 0x03 */
#define APP_CFG_ERR_CROSS       2u      /* 跨项不等式不成立（S7 层次）⇒ 同上 */
#define APP_CFG_ERR_BUSY        3u      /* 已有一次提交在处理中（⛔ 不许并发两次提交） */
#define APP_CFG_ERR_NOT_SAFE    4u      /* 不在 SAFE_OFF ⇒ 提交继续排队（不是失败，是"还没到时候"） */
#define APP_CFG_ERR_FLASH       5u      /* 擦写/回读校验失败 */
#define APP_CFG_ERR_PARAM       6u      /* 入参非法 = 代码 bug */

/* ==================== 上电三态（`Tech_Design.md` §8 协议 (2)） ==================== */
#define APP_CFG_BOOT_FRESH      0u      /* 两页都空白 = 首次上电 ⇒ 用编译期默认值，并立刻写入 A 区 */
#define APP_CFG_BOOT_LOADED     1u      /* 至少一区好 ⇒ 已按"提交序号最大"装载 */
#define APP_CFG_BOOT_LATCHED    2u      /* ★ 被写过但两区都坏 ⇒ **拒绝启动 → FAULT_LATCHED**（§5.2） */

/**
 * @brief       ★★ 参数表（**逐项清单的唯一来源**；`X(枚举名, 默认值, 最小值, 最大值, 描述)`）
 * @note        ⛔ 默认值一律**引用 `app_config.h` 的宏名**，⛔ **不抄数值**（抄了就有两个落点）。
 *              ★ 顺序 = Flash 载荷顺序 = Modbus `0x0100~0x014F` 的分组顺序（§8 协议 (5)）。
 *              ⚠️ 增删一项会同时改变：载荷长度、`APP_CFG_VER` 是否要迁移、80 格格号 ——
 *                 ⇒ 这三处是**计数耦合**，必须同批改（本文件 + §8 的分组表 + `app_modbus.c` 的映射）。
 */
#define APP_CFG_ITEMS(X)                                                                              \
    /* ---- 单体（§8 分组 0x0103~0x010B，9 格） ---- */                                                \
    X(CELL_OV_L1_MV,      APP_BMS_CELL_OV_L1_MV,      APP_BMS_CELL_MV_VALID_MIN, APP_BMS_CELL_MV_VALID_MAX, "cell OV L1")   \
    X(CELL_OV_L2_MV,      APP_BMS_CELL_OV_L2_MV,      APP_BMS_CELL_MV_VALID_MIN, APP_BMS_CELL_MV_VALID_MAX, "cell OV L2")   \
    X(CELL_OV_L3_MV,      APP_BMS_CELL_OV_L3_MV,      APP_BMS_CELL_MV_VALID_MIN, APP_BMS_CELL_MV_VALID_MAX, "cell OV L3")   \
    X(CELL_UV_L1_MV,      APP_BMS_CELL_UV_L1_MV,      APP_BMS_CELL_MV_VALID_MIN, APP_BMS_CELL_MV_VALID_MAX, "cell UV L1")   \
    X(CELL_UV_L2_MV,      APP_BMS_CELL_UV_L2_MV,      APP_BMS_CELL_MV_VALID_MIN, APP_BMS_CELL_MV_VALID_MAX, "cell UV L2")   \
    X(CELL_UV_L3_MV,      APP_BMS_CELL_UV_L3_MV,      APP_BMS_CELL_MV_VALID_MIN, APP_BMS_CELL_MV_VALID_MAX, "cell UV L3")   \
    X(CELL_HYST_MV,       APP_BMS_CELL_HYST_MV,       0u,                          500u,                   "cell hyst")     \
    X(CELL_DIF_MV,        APP_BMS_CELL_DIF_MV,        0u,                          APP_BMS_CELL_MV_VALID_MAX, "cell diff")  \
    X(CELL_DIF_HOLD_N,    APP_BMS_CELL_DIF_HOLD_N,    1u,                          100u,                   "diff hold N")    \
    /* ---- 温度（§8 分组 0x010C~0x0126，27 格） ---- */                                              \
    X(T_CHG_OT_L1_X10,    APP_BMS_T_CHG_OT_L1_X10,    -400,                        1000,                   "T chg OT L1")    \
    X(T_CHG_OT_L2_X10,    APP_BMS_T_CHG_OT_L2_X10,    -400,                        1000,                   "T chg OT L2")    \
    X(T_CHG_OT_L3_X10,    APP_BMS_T_CHG_OT_L3_X10,    -400,                        1000,                   "T chg OT L3")    \
    X(T_CHG_UT_L1_X10,    APP_BMS_T_CHG_UT_L1_X10,    -400,                        1000,                   "T chg UT L1")    \
    X(T_CHG_UT_L2_X10,    APP_BMS_T_CHG_UT_L2_X10,    -400,                        1000,                   "T chg UT L2")    \
    X(T_CHG_UT_L3_X10,    APP_BMS_T_CHG_UT_L3_X10,    -400,                        1000,                   "T chg UT L3")    \
    X(T_DIS_OT_L1_X10,    APP_BMS_T_DIS_OT_L1_X10,    -400,                        1000,                   "T dis OT L1")    \
    X(T_DIS_OT_L2_X10,    APP_BMS_T_DIS_OT_L2_X10,    -400,                        1000,                   "T dis OT L2")    \
    X(T_DIS_OT_L3_X10,    APP_BMS_T_DIS_OT_L3_X10,    -400,                        1000,                   "T dis OT L3")    \
    X(T_DIS_UT_L1_X10,    APP_BMS_T_DIS_UT_L1_X10,    -400,                        1000,                   "T dis UT L1")    \
    X(T_DIS_UT_L2_X10,    APP_BMS_T_DIS_UT_L2_X10,    -400,                        1000,                   "T dis UT L2")    \
    X(T_DIS_UT_L3_X10,    APP_BMS_T_DIS_UT_L3_X10,    -400,                        1000,                   "T dis UT L3")    \
    X(T_HYST_X10,         APP_BMS_T_HYST_X10,         0u,                          200u,                   "T hyst")         \
    X(T_DIF_L1_X10,       APP_BMS_T_DIF_L1_X10,       0u,                          1000u,                  "T dif L1")       \
    X(T_DIF_L2_X10,       APP_BMS_T_DIF_L2_X10,       0u,                          1000u,                  "T dif L2")       \
    X(T_DIF_L3_X10,       APP_BMS_T_DIF_L3_X10,       0u,                          1000u,                  "T dif L3")       \
    X(T_DIF_HYST_X10,     APP_BMS_T_DIF_HYST_X10,     0u,                          200u,                   "T dif hyst")     \
    X(TR_RATE_L1_X10,     APP_BMS_TR_RATE_L1_X10,     0u,                          1000u,                  "rise L1")        \
    X(TR_RATE_L2_X10,     APP_BMS_TR_RATE_L2_X10,     0u,                          1000u,                  "rise L2")        \
    X(TR_RATE_L3_X10,     APP_BMS_TR_RATE_L3_X10,     0u,                          1000u,                  "rise L3")        \
    X(TR_RATE_HYST_X10,   APP_BMS_TR_RATE_HYST_X10,   0u,                          200u,                   "rise hyst")      \
    X(TR_HOLD_L1_N,       APP_BMS_TR_HOLD_L1_N,       1u,                          100u,                   "rise hold L1")   \
    X(TR_HOLD_L2_N,       APP_BMS_TR_HOLD_L2_N,       1u,                          100u,                   "rise hold L2")   \
    X(TR_HOLD_L3_N,       APP_BMS_TR_HOLD_L3_N,       1u,                          100u,                   "rise hold L3")   \
    X(T_NOCHG_X10,        APP_BMS_T_NOCHG_X10,        -400,                        200,                    "0C no-chg")      \
    X(T_NOCHG_REC_X10,    APP_BMS_T_NOCHG_REC_X10,    -400,                        200,                    "0C recover")     \
    X(T_NOCHG_REC_MS,     APP_BMS_T_NOCHG_REC_MS,     0u,                          600000u,                "0C rec time")    \
    /* ---- 电流（§8 分组 0x0127~0x0133，13 格） ---- */                                              \
    X(I_CHG_L1_10MA,      APP_BMS_I_CHG_L1_10MA,      0u,                          3000u,                  "I chg L1")       \
    X(I_CHG_L2_10MA,      APP_BMS_I_CHG_L2_10MA,      0u,                          3000u,                  "I chg L2")       \
    X(I_CHG_L3_10MA,      APP_BMS_I_CHG_L3_10MA,      0u,                          3000u,                  "I chg L3")       \
    X(I_DIS_L1_10MA,      APP_BMS_I_DIS_L1_10MA,      0u,                          3000u,                  "I dis L1")       \
    X(I_DIS_L2_10MA,      APP_BMS_I_DIS_L2_10MA,      0u,                          3000u,                  "I dis L2")       \
    X(I_DIS_L3_10MA,      APP_BMS_I_DIS_L3_10MA,      0u,                          3000u,                  "I dis L3")       \
    X(I_HOLD_L1_MS,       APP_BMS_I_HOLD_L1_MS,       0u,                          60000u,                 "I hold L1")      \
    X(I_HOLD_L2_MS,       APP_BMS_I_HOLD_L2_MS,       0u,                          60000u,                 "I hold L2")      \
    X(I_HOLD_L3_MS,       APP_BMS_I_HOLD_L3_MS,       0u,                          60000u,                 "I hold L3")      \
    X(I_JUMP_10MA,        APP_BMS_I_JUMP_10MA,        0u,                          3000u,                  "I jump")         \
    X(I_JUMP_MS,          APP_BMS_I_JUMP_MS,          0u,                          60000u,                 "I jump win")     \
    X(I_HYST_CHG_10MA,    APP_BMS_I_HYST_CHG_10MA,    0u,                          1000u,                  "I hyst chg")     \
    X(I_HYST_DIS_10MA,    APP_BMS_I_HYST_DIS_10MA,    0u,                          1000u,                  "I hyst dis")     \
    /* ---- 硬件层（§8 分组 0x0134~0x0139，6 格）★ 这 6 个会被写进 AFE 寄存器（D8） ---- */            \
    X(CELL_OV_HW_MV,      APP_BMS_CELL_OV_HW_MV,      APP_BMS_CELL_MV_VALID_MIN, APP_BMS_CELL_MV_VALID_MAX, "HW cell OV")   \
    X(CELL_UV_HW_MV,      APP_BMS_CELL_UV_HW_MV,      APP_BMS_CELL_MV_VALID_MIN, APP_BMS_CELL_MV_VALID_MAX, "HW cell UV")   \
    X(I_OCC_HW_10MA,      APP_BMS_I_OCC_HW_10MA,      0u,                          3000u,                  "HW OCC")         \
    X(I_OCD_HW_10MA,      APP_BMS_I_OCD_HW_10MA,      0u,                          3000u,                  "HW OCD")         \
    X(I_SCD_HW_10MA,      APP_BMS_I_SCD_HW_10MA,      0u,                          3000u,                  "HW SCD")         \
    X(HW_OCC_OCD_MS,      APP_BMS_HW_OCC_OCD_MS,      0u,                          60000u,                 "HW delay")       \
    /* ---- 告警时序（§8 分组 0x013A~0x013B，2 格） ---- */                                            \
    X(CONFIRM_MS,         APP_BMS_CONFIRM_MS,         0u,                          60000u,                 "confirm win")    \
    X(DEBOUNCE_L3_MS,     APP_BMS_DEBOUNCE_L3_MS,     0u,                          5000u,                  "L3 debounce")    \
    /* ---- 本地设置（§8 分组 0x013C~0x0141，6 格） ---- */                                            \
    X(MB_ADDR,            APP_MB_ADDR_DEFAULT,        APP_MB_ADDR_MIN,              APP_MB_ADDR_MAX,        "Modbus addr")    \
    X(MB_BAUD_CODE,       APP_MB_BAUD_CODE_DEFAULT,   0u,                           APP_MB_BAUD_CODE_MAX,   "baud code")      \
    X(LATCH_HOLD_MS,      APP_CTR_LATCH_HOLD_MS,      1000u,                       60000u,                 "latch hold")     \
    X(PRECHARGE_MS,       APP_CTR_PRECHARGE_MS,       10u,                         10000u,                 "precharge")      \
    X(MIN_INTERVAL_MS,    APP_CTR_MIN_INTERVAL_MS,    100u,                        60000u,                 "min interval")   \
    X(PRECHARGE_RETRY,    APP_CTR_PRECHARGE_RETRY,    1u,                          10u,                    "prechg retry")   \
    /* ---- 采样与判活（§8 分组 0x0142~0x0143，2 格） ---- */                                          \
    X(DATA_STALE_MS,      APP_DATA_STALE_MS,          500u,                        60000u,                 "data stale")     \
    X(PRECHARGE_DV_MV,    APP_CTR_PRECHARGE_DV_MV,    100u,                        5000u,                  "prechg V_SW")

/** 参数索引（顺序 == Flash 载荷顺序 == Modbus 分组顺序） */
typedef enum
{
#define APP_CFG_X_ENUM(n, d, mn, mx, s)     APP_CFG_IDX_##n,
    APP_CFG_ITEMS(APP_CFG_X_ENUM)
#undef APP_CFG_X_ENUM
    APP_CFG_COUNT
} app_cfg_idx_t;

/** 载荷长度（半字数）—— 与 `APP_CFG_COUNT` 同源，⛔ 不另写数字 */
#define APP_CFG_PAYLOAD_HW      ((uint16_t)APP_CFG_COUNT)
/** ★ 结构版本：**每次改动 `APP_CFG_ITEMS` 的顺序/增删都必须 +1**（否则旧数据会被按新布局读） */
#define APP_CFG_VER             1u
/** 参数区魔数（半字口径：'P','A' → 0x5041） */
#define APP_CFG_MAGIC           0x5041u
/** 页头半字数（魔数/版本/序号(2)/CRC/长度 = 6 个半字 = 12 B） */
#define APP_CFG_HDR_HW          6u
/** 第几格开始是保留区（§8 分组表：0x0144~0x014F 共 12 格） */
#define APP_CFG_MODBUS_FIRST    0x0100u
#define APP_CFG_MODBUS_RESERVED 0x0144u

/**
 * @brief       上电装载：跑 §8 协议 (2) 的三态判据
 * @retval      `APP_CFG_BOOT_FRESH` / `APP_CFG_BOOT_LOADED` / `APP_CFG_BOOT_LATCHED`
 * @note        ⛔ 必须在 `app_log_init()` **之前**调用（本模块只用 `stmflash_read()`，不写日志）；
 *              返回 `APP_CFG_BOOT_LATCHED` 时调用方必须进 `FAULT_LATCHED`（§5.2），
 *              并**不得**用默认值继续跑（`app_main()` 的落点见 `Build.md` §8.37）。
 * @note        ⚠️ **`APP_CFG_BOOT_FRESH` 分支不在启动序列里写 Flash**（2026-09-29 就地更正本行原措辞：
 *              原写"会立刻写 A 区" —— 实现时改成**只置待提交标志**，真正的擦写一律走
 *              `app_cfg_poll()`，于是**启动序列里一次 Flash 写都没有**）。理由有两条：
 *              ① 首次上电的关键路径越短越好（§8 的擦写窗口 20~80ms 不该挂在开机路径上）；
 *              ② 让"所有 Flash 擦写都经过同一个出口"这条不变式成立（§2.3：擦写只在 `TaskLog` 里做）。
 */
uint8_t     app_cfg_load(void);

/**
 * @brief       取一项的**当前生效值**（RAM 副本；未装载时返回编译期默认值）
 */
uint16_t    app_cfg_get(uint16_t idx);

/**
 * @brief       改一项的 RAM 值（**不落盘**；§8 协议 (3) 第 1 步）
 * @retval      `APP_CFG_OK` / `APP_CFG_ERR_PARAM` / `APP_CFG_ERR_RANGE` / `APP_CFG_ERR_CROSS`
 * @note        ★★ **这里就是 §8 协议 (4) 的落点**：单项范围 + **跨项不等式**两道都过才写入 RAM。
 *              ⛔ 不许"先写进去、运行时再判断" —— 那等于把 S7 交给运气。
 * @note        `idx >= APP_CFG_COUNT` ⇒ `APP_CFG_ERR_PARAM`（调用方是 Modbus 时回异常码 0x02）。
 */
uint8_t     app_cfg_set(uint16_t idx, uint16_t val);

/**
 * @brief       请求把当前 RAM 参数集提交到 Flash（置**待提交标志**就返回）
 * @retval      `APP_CFG_OK` / `APP_CFG_ERR_BUSY`（已有一次在排队/进行中）
 * @note        ⛔ 本函数**不写 Flash**（§8 协议 (3) 第 2~3 步）：真正的擦写只发生在
 *              `app_cfg_poll()` 里、且只在 `SAFE_OFF` 时。理由 = §4.4 规则 2（Flash 空窗）
 *              ＋ §2.3（擦写必须在 `TaskLog` 里做，与日志写**串行**）。
 */
uint8_t     app_cfg_commit_request(void);

/**
 * @brief       把 RAM 参数集**恢复为编译期默认值**（同样只置待提交标志，⛔ 不写 Flash）
 */
uint8_t     app_cfg_restore_defaults(void);

/**
 * @brief       提交执行体：**由 `TaskLog` 每拍调用**（§2.3）
 * @param       in_safe_off  调用方给的"现在是不是 SAFE_OFF"（★ 由 `app_protect_state()` 判定，
 *                           ⛔ 本模块**不自己去读状态**：状态属主是 `app_protect`，读一次就多一个消费者）
 * @retval      `APP_CFG_OK`（无事可做或已成功）/ `APP_CFG_ERR_NOT_SAFE`（还在排队）/
 *              `APP_CFG_ERR_FLASH`（擦写或回读校验失败 ⇒ **已回滚到原活动区**，现场不变）
 * @note        ⛔ 内部**不阻塞等待**、⛔ 不调 `HAL_Delay`；写期间的 `flash_busy` 由调用方置
 *              （它与日志写共用那个位，`app_data_flash_busy()` 的写者仍是 TaskLog 一处）。
 */
uint8_t     app_cfg_poll(uint8_t in_safe_off);

/**
 * @brief       只读状态出口（给 Modbus `0x0301` / `0x0100~0x0102` 用）
 * @param[out]  boot_state  三态之一；`commit_seq` 当前活动区的提交序号；`bad_mask` 哪一区坏（bit0=A,bit1=B）
 * @note        ⛔ 只读，不改任何状态。
 */
void        app_cfg_status(uint8_t *boot_state, uint32_t *commit_seq, uint8_t *bad_mask);

/**
 * @brief       ★ 无需外部设备的自检：CRC 已知答案 + 表长与页头断言 + 跨项校验在默认值上成立
 * @retval      0 = 全过；1 = CRC 已知答案不符；2 = 默认值**自己**就过不了跨项校验（表写错了）
 * @note        ★ 为什么值得写：**默认值必须自己满足全部跨项不等式** —— 那正是 `app_config.h`
 *              §12.7 那 40+ 条编译期断言在做的事，这里用**运行期同一套代码**再核一遍。
 *              两者不一致 ⇒ 说明我把编译期断言的规则在运行期实现错了（**两条通路互为对照**）。
 * @note        ★ 判定结果落在 `app_cfg_selftest_result()` 里（RAM 可读），⛔ 不只有串口一个出口
 *              —— 本机没有 COM 通路（这条口径来自验收轮抓到的"自检判定无出口"）。
 */
uint8_t     app_cfg_selftest(void);

/** ★ 只读：是否有一次提交正在排队（Modbus `0x0304` 的读回要用；⛔ 不暴露"上膛"等命令语义） */
uint8_t     app_cfg_commit_pending(void);

/** 取证出口（⛔ 不参与判据）：最近一次 `app_cfg_selftest()` 的返回值 */
uint8_t     app_cfg_selftest_result(void);

/**
 * @brief       ★ 上电装载的**取证出口**（5 个字节，⛔ 不参与任何判据）
 * @param[out]  d  d[0]=A 区分类  d[1]=B 区分类  d[2]=走的分支  d[3]=跨项校验结果  d[4]=最后返回的态
 * @note        ★ 为什么必须有：本机**没有 COM 通路**（`485 :`/`CFG :` 那些 printf 看不见），
 *              而"上电装载走了哪一支"是这类缺陷唯一的现场事实。⛔ 不许只喂 printf（验收轮抓过一次）。
 *              分类码：0=空白 1=可用 2=版本是别的 3=真的坏了；分支码：1=FRESH 2=LOADED 3=三态③ 4=跨项校验。
 */
void        app_cfg_diag(uint8_t *d);

/**
 * @brief       ★ 提交取证环（⛔ 不参与判据）：最近 4 次提交各 4 字节
 * @param[out]  out16  out16[i*4+0]=目标区(0=A,1=B)  +1=提交前的活动区  +2=新序号低 8 位  +3=结构版本
 * @param[out]  n      累计提交次数
 */
void        app_cfg_commit_trace(uint8_t *out16, uint8_t *n);

#endif
