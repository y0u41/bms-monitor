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

#endif
