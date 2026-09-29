/**
 ****************************************************************************************************
 * @file        afe.h
 * @brief       AFE（模拟前端）对应用层的**唯一**接口 —— 型号/寄存器/引脚对上层不可见（S15）
 * @note        1, 契约来源：`Tech_Design.md` §4.5。User/App 目录下的 .c 只能 include 本文件，
 *                  不得出现寄存器地址、寄存器名或 AFE 型号相关宏。
 *              2, 本版实现 = `afe_hal_bq76920.c`（bq76920，直寄存器模型 + CRC-8 + BOOT 脉冲）。
 *              3, ★ **电流与温度不走本接口**：电流用 INA226、电芯温度用 MCU ADC 的 NTC（见 §2 引脚表），
 *                  AFE 的 CC 通道与 TS2 本版不接线 —— 与其放一个恒返回"不支持"的空壳函数误导上层，
 *                  不如不提供。
 *              4, ⛔ 本接口全部**阻塞式**（软件 I2C），只允许在 `TaskAcq` / `TaskProtect` 任务里调用，
 *                  **禁止在中断里调用**（S8）。
 ****************************************************************************************************
 */

#ifndef __AFE_H
#define __AFE_H

#include "./SYSTEM/sys/sys.h"
#include "./App/app_config.h"

/* ===================== 返回码 ===================== */
#define AFE_OK                  0
#define AFE_ERR_NACK            1   /* 器件无应答：未 BOOT / 线束未接 / 地址不对（C-64 的第一现场） */
#define AFE_ERR_CRC             2   /* CRC 校验失败：后缀参数填错（`03` 出厂固化开启，不可关） */
#define AFE_ERR_BUS             3   /* 总线锁死或时钟拉伸超时（已尝试 9 脉冲恢复） */
#define AFE_ERR_DATA            4   /* 通信正常但数据不合理：换算/线序/芯片损坏 */
#define AFE_ERR_IO              5   /* 入参非法 */

/**
 * @brief   硬件层状态（已由驱动**解码**，应用层拿到的是"哪一项保护动作了"）
 * @note    ★ S26：ALERT 高 ≠ 硬件层动作（`CC_READY` 等也会让 ALERT 跳变），
 *          所以本结构把"保护位"与"非保护位"分开，应用层只认前者。
 */
typedef struct
{
    uint8_t ov;         /* 1 = 单体过压比较器动作 */
    uint8_t uv;         /* 1 = 单体欠压比较器动作 */
    uint8_t ocd;        /* 1 = 过流(放电)比较器动作 */
    uint8_t scd;        /* 1 = 短路比较器动作 */
    uint8_t cc_ready;   /* 1 = 库仑计就绪标志（**不得**据此锁定） */
} afe_hw_t;
/* ALERT 引脚(PD13)的电平由中断侧管理，不在这里读：EXTI 配置归 `app_nvic.c`，
 * 两处初始化同一个脚必然打架。 */

/* ===================== 生命周期 ===================== */

/**
 * @brief       上电初始化：**先发 BOOT 脉冲再碰 I2C**（C-64），随后读标定值(GAIN/OFFSET)并缓存
 * @retval      AFE_OK / AFE_ERR_*
 */
uint8_t afe_init(void);

/**
 * @brief       上电自检（★ 每次改动接线后必须重跑，坏芯片的表现与驱动 bug 完全一样）
 * @note        做四件事：① BOOT 后地址探测 —— 区分"没 ACK(未启动/线束)"与"有 ACK 但数据错"；
 *              ② 逐串合理性（1.5~4.2V 之外即判无效）；③ Σ单体 vs 组压寄存器交叉校验；
 *              ④ CRC 生效性（同时验 CRC 开与关两种模式，防止"后缀填错导致通信静默不成立"）。
 * @retval      AFE_OK / AFE_ERR_NACK / AFE_ERR_CRC / AFE_ERR_DATA
 */
uint8_t afe_self_test(void);

/* ===================== 采集 ===================== */

/**
 * @brief       读单体电压
 * @param       mv     输出 `count` 个 mV；**无效值 = `APP_MV_INVALID`(0xFFFF)，不是 0**
 * @param       count  ≤ `APP_CELL_COUNT_MAX`，一律用 `APP_CELL_COUNT` 传，禁止字面量 4（G6）
 */
uint8_t afe_read_cells(uint16_t *mv, uint8_t count);

/**
 * @brief       读组电压（AFE 侧 = Σ单体，非独立测量）
 * @param       pack_10mv  输出，单位 10mV（= Modbus 量纲，§4.4）
 */
uint8_t afe_read_pack(uint16_t *pack_10mv);

/* ===================== 均衡（P2 功能，本版只保证接口在） ===================== */
uint8_t afe_get_balance(uint8_t *mask);
uint8_t afe_set_balance(uint8_t mask);      /* bit0 = 第 1 串 */

/* ===================== 硬件层（只读解码 + 栅极使能） ===================== */

/**
 * @brief       取解码后的硬件层保护位；★ 驱动内部完成"读 + 写 1 清位"，调用者不碰寄存器
 * @note        由 `TaskProtect` 在 ALERT 上升沿通知后调用（§5.1）；返回 AFE_OK 且各保护位为 0
 *              表示"ALERT 响了但不是保护动作"（如 CC_READY）—— 此时**禁止**进锁定态。
 */
uint8_t afe_hw_fault(afe_hw_t *st);

/**
 * @brief       请求 AFE 的充/放电通道导通状态（与 MCU 的 `EN_x` 是**与**关系，S1）
 * @note        ★ 只允许 `app_protect.c` 经 `actuator` 层调用（S9）；上电默认 0/0 = 常态断开（S3）。
 *              ⛔ 本函数**不得**被用来关闭 AFE 内置比较器（S4）。
 */
uint8_t afe_set_channel(uint8_t chg_on, uint8_t dsg_on);

/* ===================== 链路健康（S2 失效安全） ===================== */

/**
 * @brief       1 = 通信链路可信；0 = 已连续失败达 `APP_AFE_FAIL_MAX` 次
 * @note        为 0 时保护状态机必须进安全态，**不是**维持现状（S2-①）。
 */
uint8_t afe_link_ok(void);

uint16_t afe_fail_count(void);      /* 累计 I2C 失败次数，诊断/日志用 */
void     afe_clear_fail(void);


/* ==================== ★★ D8 前置：硬件保护门限（纯编码，⛔ 不碰总线） ==================== */

/** 硬件保护门限的**落码形式**（解码后的工程量） */
typedef struct
{
    uint16_t ov_mv;         /* 单体过压门限（mV） */
    uint16_t uv_mv;         /* 单体欠压门限（mV） */
    uint16_t ocd_mv;        /* 放电过流门限（跨 Rsns 的毫伏） */
    uint16_t scd_mv;        /* 短路门限（跨 Rsns 的毫伏） */
    uint16_t ocd_ms;        /* 放电过流延时（ms） */
    uint16_t ov_s;          /* 过压延时（s） */
    uint16_t uv_s;          /* 欠压延时（s） */
    uint16_t scd_us;        /* 短路延时（µs） */
} afe_prot_t;

/** 四个寄存器的原始字节（❗ 仅编码结果，⛔ 本结构不保证已写入器件） */
typedef struct
{
    uint8_t protect1;       /* 0x06 */
    uint8_t protect2;       /* 0x07 */
    uint8_t protect3;       /* 0x08 */
    uint8_t ov_trip;        /* 0x09 */
    uint8_t uv_trip;        /* 0x0A */
} afe_prot_regs_t;

/**
 * @brief       把参数区的 5 项硬件门限读出来并换算成工程量
 * @note        门限**取值来自参数区**（`CELL_OV_HW_MV` / `CELL_UV_HW_MV` /
 *              `I_OCD_HW_10MA` / `I_SCD_HW_10MA` / `HW_OCC_OCD_MS`）⇒ ⛔ 本模块不自带任何数值默认。
 * @retval      `AFE_OK` / `AFE_ERR_IO`（参数区未加载）
 */
uint8_t     afe_prot_from_params(afe_prot_t *p);

/**
 * @brief       工程量 → 寄存器字节（纯函数，⛔ 不碰总线）
 * @note        公式与表均取自**本仓手册** `datasheet/BQ769x0_数据手册_SLUSBK2I.txt`：
 *                · OV/UV：`OV_TRIP_FULL = (OV_mV − ADCOFFSET_mV) × 1000 / ADCGAIN_µV`（p22 名 9 步骤），
 *                  再**右移 4 位取中间 8 位**；
 *                · 延时：OV_D1:0 = 1/2/4/8 s（表 8-11）、UV_D1:0 = 1/4/8/16 s、OCD_D2:0 = 8/20/40/80/160/320/640/1280 ms（表 8-10）、
 *                  SCD_D1:0 = 70/100/200/400 µs（表 8-9）；
 *                · 门限码：OCD_T3:0 按表 8-10 的毫伏梯（RSNS=0）取**不超过目标值的最大码**；
 *                  SCD_THRESH = `44 + 22.2 × code` mV。
 * @note        ★ 两个**方向性**必须记住：过压门限取码时**向下取**（硬件提前跳），
 *              而欠压门限**向上取**（硬件晚一点跳）—— 两者都是**保守**方向：⛔ 不让是设定值被软件误读。
 */
uint8_t     afe_prot_pack(const afe_prot_t *p, uint16_t gain_uv, int16_t offset_mv, afe_prot_regs_t *r);

/** @brief 寄存器字节 → 工程量（**回读校验用**；⚠️ 换算还原到码中心，因此与原值比较必须用容差而非相等 */
void        afe_prot_unpack(const afe_prot_regs_t *r, uint16_t gain_uv, int16_t offset_mv, afe_prot_t *q);

/**
 * @brief       生产路径取刻度（即器件实测的 ADCGAIN / ADCOFFSET）
 * @retval      `AFE_OK` / `AFE_ERR_IO`（**尚未从器件取到** ⇒ ⛔ 不许继续编码）
 * @note        ★★ 为什么把刻度变成**显式入参**：本轮实测发现，原来的 `pack()` 内部直接读
 *              `s_gain_uv`，而 **AFE 不在位时它恒为 0** ⇒ 换算出的码是 0 ⇒ 自检必然失败，
 *              而且生产路径会**拿一个 0 刻度算出的门限写进器件**。
 *              ★ 口径：**读硬件派生参数的自检不是离线自检** —— 自检的输入必须完全来自被测代码自身。
 */
uint8_t     afe_prot_scale(uint16_t *gain_uv, int16_t *offset_mv);

/* ==================== ★★ D16 前置：逐串电压标定（k/b）的应用侧 ==================== */

/**
 * @brief       ★★ 喂入某串的标定系数（`Tech_Design.md` §8「标定表区协议」(5) 的分层纪律）
 * @param       idx    串下标（0 起）
 * @param       k_ppm  比例系数，**ppm**（`1000000` = 不修正）
 * @param       b_uv   偏置，**µV**
 * @retval      `AFE_OK` ／ `AFE_ERR_IO`（下标越界 或 k 落出 [900000, 1100000] 的合理性区间）
 * @note        ★★ 为什么由**应用层喂**而不是 BSP 自己读 Flash：§8 (5) 写死"**BSP 层不得读 Flash**"
 *              ⇒ `app_calib` 负责持久化，`afe` 只负责用它。⛔ 未标定的串**不要调用**本函数 ⇒ 走全局系数。
 * @note        ★ 修正作用在**全局换算之后、量程判定之前**：⛔ 修正不得把一个越界值"救回来"。
 */
uint8_t     afe_set_cell_calib(uint8_t idx, uint32_t k_ppm, int32_t b_uv);

/** @brief 清掉所有逐串标定（回到只用全局系数）—— 自检与"标定表无效"时用 */
void        afe_clear_cell_calib(void);

/** @brief 取证出口（⛔ 不参与判据）：已喂入标定系数的串位图（bit i = 第 i 串有标定） */
uint16_t    afe_cell_calib_mask(void);

/** @brief ★ 离线自检（⛔ 不需要器件）：恒等性 ／ 修正算法 ／ 越界不救回 */
uint16_t    afe_cell_calib_selftest(void);
uint16_t    afe_cell_calib_selftest_result(void);

/**
 * @brief       ★★ **D8：把参数区的硬件保护门限下发到 AFE，并回读校验**
 * @param[out]  first_bad_reg  回读不一致时写回"第一个不符的寄存器地址"（0x06~0x0A）；可传 NULL
 * @retval      `AFE_OK` ／ `AFE_ERR_IO`（未取到刻度或参数区不可用）／ `AFE_ERR_NACK`（器件不在位）／
 *              `AFE_ERR_DATA`（回读不符 —— ★ 这一条必须**当作故障处理**，⛔ 不许当成"写没生效"轻描淡写）
 * @note        ★ 手册 §8.3 原话：**"It is recommended that the host controller reload these values during its
 *              standard power-up sequence."** ⇒ 本工程在上电时（且**仅在 AFE 在位时**）调它。
 * @note        ★ 5 个寄存器（0x06~0x0A）**连续** ⇒ 用**一次块写**下发，避免中间态不一致（见 `afe_hal.h`）。
 * @note        ⛔ 未取到刻度（`afe_prot_scale()` 不 OK）时**直接拒绝**：⛔ 不允许用 0 刻度算出门限写进器件。
 */
uint8_t     afe_prot_apply(uint8_t *first_bad_reg);

/** 取证出口（⛔ 不参与判据）：最近一次 `afe_prot_apply()` 的结果码（0=成功；0xFF=从未调用） */
uint8_t     afe_prot_apply_result(void);

/** @brief ★ 离线往返自检（⛔ 不需器件）：编码→解码 必须一致；⛔ 非法输入必须被拒 */
uint16_t    afe_prot_selftest(void);
uint16_t    afe_prot_selftest_result(void);

#endif
