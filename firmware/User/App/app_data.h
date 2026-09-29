/**
 ****************************************************************************************************
 * @file        app_data.h
 * @brief       BMS 数据模型 + 多任务共享快照的发布机制
 * @note        本文件是 `Tech_Design.md` §4.4 的**代码落点**。设计里那三张结构体是冻结的
 *              （字段名/单位/无效值口径都不许改，Modbus 映射与 LCD 都按它们取数），
 *              D1 的验收项"数据结构与 §4.4 一致"到 2026-09-26 才真正落盘 —— 之前只存在于文档。
 *
 * @note        为什么要"双缓冲 + 序号"而不是互斥量（§4.4 审查补充，AGENTS §2.3 的落地）：
 *                TaskProtect 优先级最高，一旦它去抢 TaskAcq 持有的锁，保护路径就要等采集任务
 *                —— 正是 S12"保护路径不得被阻塞"要禁止的形态。单向数据用双缓冲，
 *                双向/命令类才用队列或互斥量。
 *              代价：读到的快照最多滞后 1 个采集周期（100ms），**永不阻塞、永不见撕裂**。
 *
 * @note        单位约定（AGENTS §2.2，★ 内部量纲 = Modbus 量纲，不允许两套量纲并存）：
 *                单体电压 mV（无效 = APP_MV_INVALID，**不是 0**）｜ 组压 10mV ｜ 电流 10mA
 *                （**充正放负**）｜ 温度 0.1℃ ｜ SOC/SOH 0.1% ｜ 容量 mAh ｜ 时间 ms。
 ****************************************************************************************************
 */

#ifndef __APP_DATA_H
#define __APP_DATA_H

#include "./SYSTEM/sys/sys.h"
#include "./App/app_config.h"

/* ==================== 状态机状态（Tech_Design.md §5.2） ==================== */
/* ★ 权威写入方是 D5 的 app_protect.c；本模块只负责把它随快照一起发布。
 *   UI 与 Modbus 读到这里的状态即可，⛔ 不得自己根据门限推断（§4.2 模块职责）。 */
typedef enum
{
    BMS_STATE_SAFE_OFF = 0,      /* 所有使能 = 0，主 MOS 全断（上电默认态，S3） */
    BMS_STATE_PRECHARGE,         /* 预充 MOS 导通、主 MOS 断开 */
    BMS_STATE_ON,                /* 主 MOS 导通 */
    BMS_STATE_FAULT_LATCHED      /* 全断 + 锁定，只能长按 KEY0 解除（S11） */
} bms_state_t;

/* ==================== 化学体系（决定 OCV-SOC 表与截止电压） ==================== */
/* chem_type_t 与 APP_BMS_CHEM 定义在 app_config.h §12.8 —— 本文件包含它，不重复定义。 */

/* ==================== 告警等级（0x001E 寄存器） ==================== */
#define BMS_LEVEL_NORMAL        0
#define BMS_LEVEL_L1            1       /* 提示 + 降额 50% */
#define BMS_LEVEL_L2            2       /* 降额 20% */
#define BMS_LEVEL_L3            3       /* 切断 */

/* ==================== 采样链路健康位图（0x002F） ==================== */
/* ★ 每一位都对应"一个可能让保护判据失效的观测缺口"，置位即"不可知"，
 *   而不可知一律向保守方向降级（§4.4 数据新鲜度规则）。 */
#define BMS_LINK_AFE            0x01    /* AFE 组压/单体通道不新鲜或通信失败 */
#define BMS_LINK_INA226         0x02    /* 电流/冗余组压通道 */
#define BMS_LINK_NTC            0x04    /* 电芯表面 NTC（失效 → 禁止充电） */
#define BMS_LINK_SHT30          0x08    /* 环境温度（失效 → 仅告警） */
#define BMS_LINK_VSW            0x10    /* 开关两端压差（失效 → 禁止闭合） */
#define BMS_LINK_FLASH_BUSY     0x20    /* ★ Flash 正在擦写：软件层保护空窗（S25） */

/* ==================== 硬件保护层的可观测状态（审查 C-98） ====================
 * ★ 这一位是"硬件层现在到底能不能兜底"的唯一界面出口，⛔ 不得把"没有观测"渲染成"待机正常"。
 *   三态而不是布尔：布尔的 `!=0 ? ACTED : idle` 在 AFE 从未接入时会永久显示 idle，
 *   读起来像"保护层已武装"，而实际是**无任何观测**（违反 §4 自检项"不可知即不安全"）。 */
#define BMS_HW_UNKN             0u      /* 上电初值 / AFE 链路未建立 ⇒ 硬件层状态**不可知** */
#define BMS_HW_IDLE             1u      /* ★ 只有这一档可称正常：链路新鲜且解码确认未动作（D7/D8 落地） */
#define BMS_HW_ACTED            2u      /* 硬件层解码命中保护位 = 已动作（§5.1 / S26；解码落点在契约侧） */

/* ==================== 温度测点索引（★ 与 §4.4 结构体注释 + §6.1 的 0x0013~0x0014「温度 1~2 表面/环境」同口径） ====================
 * ⛔ 任何模块不得再散写 [0] / [1] 字面量。
 * ⚠️ **D11 必须重新裁决本表**：`附录C` **C-48** 已把 **PA6 提为"NTC #2 必用"**（`PRD.md` FR-SOC-05 要"温差 / 最高最低温度 / 温升速率"，
 *   单路算不出温差，且环境温度不能替代电芯测点）⇒ 实际测点 = **电芯 #1 + 电芯 #2 + 环境 = 3 个**，
 *   而 §6.1 的温度块只有 2 个寄存器。⇒ 改的时候必须**一次改齐四处**（§6.1 扩格 = 协议改动 ／ 本节与 §4.4 注释 ／
 *   `APP_BMS_TEMP_ACTIVE_COUNT` 2→3 ／ 界面温度行与温差判据取数）；数组上限 `APP_BMS_TEMP_COUNT_MAX = 8` ⇒ **不改结构体**。
 *   ★ **审查 C-98 已把"每测点一条时间戳"提前落地**（`tick_ms_temp[]` 按本域下标）⇒ D11 扩测点时**新鲜度这一处不用再改**，
 *     原 F2 记的"第五处（结构体）"随之消失。
 * ⛔ 在裁决之前**不得把 `temp_valid` 的 bit2 置位**（未写入的通道标可信 = 把 0 当 0.0℃ 交给判据）。 */
#define APP_TEMP_IDX_CELL_NTC   0       /* 电芯表面 NTC #1（PA5 = ADC1_IN5，D11 落地）｜ ★ 今天**无写者**：
                                         *   D2 的假温度已删（C-98）、时间戳无人推进 ⇒ 界面显示 "--.-"、
                                         *   `BMS_LINK_NTC` 常亮 —— 这才是"还没有 NTC 硬件"的正确表达 */
#define APP_TEMP_IDX_ENV        1       /* 环境温度 = SHT30（D3 落地，写者 app_acq） */
#define APP_TEMP_IDX_CELL_NTC2  2       /* 电芯表面 NTC #2（PA6 = ADC1_IN6，D11 落地）
                                         *   ★ 为什么不放在 [1]：[1] 已经是环境（已交付的 `0x0014`），
                                         *   改它的含义 = 对已发布协议静默改义（`Tech_Design.md` §4.4 v3.50 裁决）。 */

/* ★★ 两个**集合**（§4.4 v3.50 裁决）—— 它们回答的是两个不同的问题，⛔ 不能合并：
 *   ① `APP_TEMP_CELL_MASK`  = 「**保护判据**该看哪几路」 ⇒ 只有**电芯测点**（环境 ⛔ 不进，§3.2 明写它不能替代电芯测点）；
 *   ② `APP_TEMP_OWNER_MASK` = 「**新鲜度**该等哪几路」 ⇒ 只等**有属主**的测点（写者存在即可）。
 *   ★ 两者容易被当成一回事：把「前 N 个活跃点」当成判据集合，会让**环境温度参与电芯温差**（切断级判据）；
 *   把「有属主」当成「有数据」，又会把“设备未接”消成一个正常时间戳。 */
#define APP_TEMP_CELL_MASK      ((uint16_t)((1u << APP_TEMP_IDX_CELL_NTC) | (1u << APP_TEMP_IDX_CELL_NTC2)))
#define APP_TEMP_OWNER_MASK     ((uint16_t)((1u << APP_TEMP_IDX_CELL_NTC) | (1u << APP_TEMP_IDX_ENV) | (1u << APP_TEMP_IDX_CELL_NTC2)))

/* ==================== 三张核心结构体（字段与 §4.4 逐字一致） ==================== */

/* 采集层输出：只由 TaskAcq 写，其余模块只读
 * ★★ 语义冻结（2026-09-29 裁决，`Tech_Design.md` §4.4 末「原始值 / 滤波值」段 / `附录C` C-120）：
 *    **本结构只承载"原始值（未滤波）"** —— 它是 `app_protect` 的唯一取数口，而 PRD:193 明令
 *    保护判据不得吃滑动平均的群延迟。⛔ 任何滤波量都不得加进这里；滤波值留在 `app_acq` 的
 *    模块内静态量里、经只读取值口暴露（`app_soc` 就在 TaskAcq 内被调用 ⇒ 不过任务边界）。
 *    ⚠️ 唯一例外 = `temp_x10[APP_TEMP_IDX_ENV]`（它存的是滤波值：那一格的消费者里没有保护判据）。 */
typedef struct
{
    uint16_t cell_mv[APP_CELL_COUNT_MAX];   /* ★ 无效 = APP_MV_INVALID(0xFFFF)，不是 0 */
    uint16_t cell_valid;                    /* 位图，1 = 该串可信（16 位正好覆盖 16 串） */
    uint16_t pack_10mv;                     /* ★ 组电压(AFE)：单位 10mV，与 Modbus 一致 */
    uint16_t pack_10mv_aux;                 /* ★ INA226 冗余通道：同单位 */
    int16_t  curr_10ma;                     /* 充正放负 */
    uint8_t  curr_valid;
    int16_t  temp_x10[APP_BMS_TEMP_COUNT_MAX];  /* [0]=电芯表面 [1]=环境 */
    uint16_t temp_valid;                    /* 位图；测点 >16 时改 u32 */
    uint16_t v_sw_10mv;                     /* ★ 预充判据 */
    uint8_t  v_sw_valid;
    uint32_t tick_ms;                       /* 本帧时间戳 */
    uint32_t tick_ms_cell, tick_ms_curr;
    /* ★ 温度新鲜度**按测点各一条**（审查 C-98，原 F2）：一条时间戳两个属主 ⇒ 没在更新的那个
     *   测点会永远"看起来新鲜"，而 `BMS_LINK_NTC` 正是"NTC 失效 → 禁止充电"的判据。
     *   下标与 `temp_x10[]` 同域（`APP_TEMP_IDX_xxx`），D11 扩到 3 个测点时**不必再改结构体**。 */
    uint32_t tick_ms_temp[APP_BMS_TEMP_COUNT_MAX];
} bms_raw_t;

/* 派生量：由 app_soc / 统计逻辑写 */
typedef struct
{
    uint16_t soc_x10, soh_x10;
    uint32_t chg_mah, dis_mah, cycles;
    uint16_t cell_max_mv, cell_min_mv, delta_mv;
    int16_t  temp_max_x10, temp_min_x10, temp_delta_x10;
    int16_t  temp_rise_x10_per_min;
    chem_type_t chem;
} bms_derived_t;

/* 告警层输出：判据在 TaskProtect 内同步执行并写快照，app_protect 读 */
typedef struct
{
    uint8_t  level;                         /* 0=正常 1=轻 2=中 3=重 */
    uint16_t fault_bits;                    /* 见 Tech_Design.md §6.2（0x001F 寄存器） */
    uint8_t  link_health;                   /* 见上面 BMS_LINK_xxx */
    uint8_t  derate_chg_pct, derate_dis_pct;    /* 100/50/20/0 */
    uint8_t  derate_state;                  /* 0未请求 1已请求未应答 2已生效 3已升级断开 */
    uint8_t  request_open;                  /* 位：充/放 */
    uint8_t  request_latch;                 /* ★ 只有"越限型"才置位（§5.2 锁定分两类，C-38） */
    uint8_t  hw_layer_state;                /* ★ BMS_HW_UNKN/IDLE/ACTED，⛔ 不可知不得显示成待机（§5.1 / S26） */
    uint8_t  hw_fault_bits;                 /* ★ 硬件层解码结果：过压/欠压/充电过流/放电过流/短路 */
    uint8_t  latch_reason;                  /* ★ 锁定原因码（区分"越限型"与"环境型"，§5.2） */
    char     reason[APP_REASON_LEN];        /* 可读原因串（ASCII），格式如 "cell3 OV 3.62V" */
} bms_alarm_t;

/* ==================== 快照：三张表一起发布 ==================== */
/* §4.4 的结论是"派生量与 raw 合成一帧一起发布"，所以发布侧只有两个盒：
 *   盒 1 = raw + derived（TaskAcq 写，同一代、一次交换 ⇒ 读者看不到"半新半旧"的组合）
 *   盒 2 = alarm       （TaskProtect 写，10ms 一帧，比采集快）
 * 本类型是**读侧的聚合视图**：app_data_read() 把两个盒拼成一份完整快照交给界面与协议。
 * ⛔ reason[] 必须随结构体整体发布，否则 LCD/日志会打出半截原因串。 */
typedef struct
{
    bms_raw_t     raw;
    bms_derived_t der;
    bms_alarm_t   alarm;
} bms_snapshot_t;

/* ==================== 数据新鲜度（核心机制，§4.4 原文） ==================== */
static inline uint8_t acq_is_fresh(uint32_t tick, uint32_t now)
{
    if (tick == 0)
    {
        return 0;                                       /* 从未采集 */
    }

    /* 无符号回绕安全：1ms/tick 的 32 位计数 49.7 天回绕一次，
     * ⛔ 不许写成 now > tick + APP_DATA_STALE_MS */
    return ((uint32_t)(now - tick) < (uint32_t)APP_DATA_STALE_MS) ? 1u : 0u;
}

/* ==================== 发布 / 读取接口 ==================== */
void             app_data_init(void);       /* 上电把两份缓冲全部置为"从未采集"(全通道无效) */

/* ---- 写者：raw+derived 由 TaskAcq(100ms) 写，alarm 由 TaskProtect(10ms) 写 ----
 * 三个 write() 返回的都是**上一帧的完整副本**，写者只改自己负责的字段，
 * 填完必须调用对应的 commit —— 没 commit 之前读者看到的仍是上一帧（不会看到半帧）。
 * ⛔ 每个盒只允许一个写者任务（app_data.c 头部解释了为什么共用缓冲会撕裂）。 */
bms_raw_t       *app_data_raw_write(void);
bms_derived_t   *app_data_der_write(void);
void             app_data_raw_commit(void);         /* raw+derived 同一代发布，一次交换 */
bms_alarm_t     *app_data_alarm_write(void);
void             app_data_alarm_commit(void);

/* 读者（UI / Modbus / MQTT）：拷一份到调用者的局部变量里，永不阻塞写者。
 * 返回 0 = 快照完整可信；1 = 从未发布过（字段还是"全无效"初值）；
 * 2 = 连续重试仍被写者打断 ⇒ **调用方必须按"数据不可信"处理**，不得用这份内容做判据。 */
uint8_t          app_data_read(bms_snapshot_t *out);

/* 快照序号：界面/协议据此判断"这轮要不要重绘" */
uint16_t         app_data_seq(void);

/* ==================== 链路新鲜度（§4.4 的判据处） ==================== */
/**
 * @brief       按"每通道时间戳 + APP_DATA_STALE_MS"重算 `link_health`，并在位变化时投链路事件
 * @note        ★ 为什么它必须是**公开函数**而不是假数据源尾部的一段代码（附录C · C-97 F3）：
 *              假数据源在 D7/D10 全部通道交接完就要整体删掉；判据跟着它一起死的话，
 *              `link_health` 会退化成"没人写 = 永远 0"，也就是**所有链路永远健康** ——
 *              这正是 §4 自检项"不可知即不安全"的反面，而且不会有任何报错。
 *              ⚠️ 唯一写者口径（C-97 F1）：本函数只允许由**告警帧的那个单写者**调用
 *              （今天 = `app_data_fake_step()` 尾部；D5 起 = TaskProtect），⛔ 不得在别的任务里再调。
 */
void             app_data_link_check(uint32_t now_ms);

/**
 * @brief       报入"Flash 正在擦写"这一**输入**（由 `app_log_poll()` 每拍调用）
 * @note        ⛔ 本函数不写告警帧、不 commit —— 它只存一个 bool，由 `app_data_link_check()`
 *              在下一次发布时读走。做成"直接改 link_health"的话，全工程就多了第二个告警帧写者
 *              （F1 那条债），而且会把单写者假设悄悄破坏掉。
 */
void             app_data_flash_busy(uint8_t busy);

/* ==================== D2 假数据源 ==================== */
/* ⏳ 交接点：TaskAcq（D3 起逐通道接入）真正写快照后，本函数连同 app_main.c 里
 *   那次调用一起删除。留着它不会掩盖真实故障 —— 采集一发布就把假值覆盖掉。 */
void             app_data_fake_step(uint32_t now_ms);

#endif
