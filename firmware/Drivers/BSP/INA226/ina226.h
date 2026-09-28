/**
 ****************************************************************************************************
 * @file        ina226.h
 * @brief       INA226（36V / 16 位 / I2C 输出 电流·总线电压 监视器）驱动，挂 `g_bus_sens` = PB6/PB7
 * @note        1, ★ 本文件 = `Build.md` **D10 第 ④ 条**的落点（"此刻工程里还没有这个文件"，
 *                  `附录C` C-96 登记的尾巴）。**本轮只交付驱动本体 + 上电自检**，
 *                  ⛔ **不接进采集通路**：`app_data.c` 的假数据源此刻仍在对 `curr_10ma` /
 *                  `pack_10mv_aux` / `tick_ms_curr` 写入，而**逐通道交接**（同批删掉"值 + 它的
 *                  时间戳"两处写入，C-97 F3）必须与 **INA226 实物到位的同一天**做 ——
 *                  只交接一半的话，页 1 会从"显示假电流"变成"显示不可信"，而**两个现象都
 *                  看不出来是交接没做完**（这正是 F3 要挡的那一类）。
 *              2, 总线口径（`Tech_Design.md` §2 / §3.3）：地址 **0x40**，与 SHT30(0x44) /
 *                  24C02(0x50) 共 `g_bus_sens`。⛔ **不得改调 `g_bus_afe`** —— S6：AFE 独占
 *                  那条总线，非安全器件的 NACK 不许抬 AFE 的失败计数（否则 = 无意义跳闸）。
 *                  事务仲裁锁由 `softi2c.c` 的三个事务入口统一加（C-96 ⑥），
 *                  ⛔ **本驱动不自带任何锁**，也不需要调用者记得加锁。
 *              3, ★ **`g_bus_sens` 的失败计数今天没有任何消费者**（全工程只有 `g_bus_afe`
 *                  的计数进 `afe_link_ok()`，见 `afe.c:316`）⇒ 上电自检遇到"INA226 还没插"
 *                  只让 `g_bus_sens.fail` 加一点数，**不会让任何判据变红**。写在头注里是因为
 *                  下一个人很容易把"fail 涨了"读成"链路故障"。
 *              4, ⛔ 中断里不得调用（S8：阻塞式 I2C 等待）。本驱动内部**没有任何 `delay_ms`**
 *                  —— 器件工作在连续转换模式，读寄存器不需要等转换完成（首次转换的时序见
 *                  `ina226_selftest()` 的注）。
 * @note        移植来源：**本项目自写**（⛔ 两个工程树里都没有 INA226 驱动可移植；
 *                  `firmware/Drivers/BSP/` 与旧工程 `atk_f103/Drivers/BSP/` 均无）。
 *              改动点：不适用（新建）。
 *              验证方式：**本轮只能到"编译级 + 门禁级 + 契约级"**（INA226 实物未到）：
 *                  ① `ina226_selftest()` 上电实跑 = 探测 → 写 CONFIG/CAL/MASK 并**逐项回读比对**
 *                     → 打印 MFG/DIE 与一次总线电压；
 *                  ② D10 台架：**电流方向符号**（充正放负，`Build.md` D10 判据①）＋ 与 AFE
 *                     组压互校 ≤1%（判据②）；
 *                  ③ ★ **本驱动自带第二形态**：同一次读里"分流器电压反算"与"电流寄存器(CAL)
 *                     反算"必须一致（`APP_INA226_XCHECK_10MA`）—— 它不需要任何外部基准就能判
 *                     "CAL 与分流器电压这两条通路自洽"，是判据①②之前的第一道门
 *                     （口径同 `AGENTS.md` §2.3 C-110：**一条判据只有单形态时不叫证据**）。
 *
 * @note        ★★ **寄存器口径取证（`AGENTS.md` §3.1 ①：绝对地址必须当场查、不许凭记忆）**
 *                  地址表取到两份**互相独立**的出处并逐条对上，位域取到其中一份逐位给出：
 *                  · TI 数据手册 **SBOS547**（INA226，§6.5 Programming / §7 Register Map）
 *                    https://www.ti.com/document-viewer/lit/html/SBOS547
 *                  · slsDetectorPackage `INA226.h`（CERN PSI）
 *                    https://gitea.psi.ch/detectors/slsDetectorPackage/raw/commit/8f3f9caf2bb67cf622b07bb381da2e189a384abf/slsDetectorServers/slsDetectorServer/INA226.h
 *                  · go-ina226（Arduino INA226 库的 Go 移植）常量表：地址同值 + **逐位**给 CONFIG
 *                    https://pkg.go.dev/github.com/Fede85/go-ina226
 *                  ⚠️ **两条没有取到手册原文的常量**（MFG_ID / DIE_ID 的**期望值**）⇒ 按 §3.1 ⑤
 *                     "我写的判据也是待核证据"，它们**只作打印参照，⛔ 不参与自检结论**
 *                     （见下面 §器件身份）。D10 首次上机以**实读值**登记为基线。
 ****************************************************************************************************
 */

#ifndef __INA226_H
#define __INA226_H

#include "./SYSTEM/sys/sys.h"

/* ===================== 器件地址（7 位，不含 R/W） =====================
 * A1 = A0 = GND（模块出厂默认）→ 0x40。`Tech_Design.md` §2/§3.3 已按 0x40 冻结。
 * ⚠️ 若模块把 A0/A1 焊到 VDD，地址会变成 0x41~0x4F —— 那种情况下 `probe()` 恒 NACK，
 *    与"器件没插"同形 ⇒ D10 上机时若 probe 失败，**先量 A0/A1 电平**再怀疑驱动。 */
#define INA226_I2C_ADDR             0x40

/* ===================== 寄存器地址（★ 出处见文件头） ===================== */
#define INA226_REG_CONFIG           0x00    /* R/W 配置：平均次数 / 两个转换时间 / 工作模式 / RST */
#define INA226_REG_SHUNT_VOLTAGE    0x01    /* R   分流器电压，**有符号** 16 位，LSB = 2.5µV */
#define INA226_REG_BUS_VOLTAGE      0x02    /* R   总线电压，**无符号** 16 位，LSB = 1.25mV */
#define INA226_REG_POWER            0x03    /* R   功率（本版不用：功率 = 电流×总线压，自己算更可控） */
#define INA226_REG_CURRENT          0x04    /* R   电流，有符号 16 位，1 LSB = Current_LSB（本版 1mA） */
#define INA226_REG_CALIBRATION      0x05    /* R/W 定标：CAL = 0.00512 / (Current_LSB × R_shunt) */
#define INA226_REG_MASK_ENABLE      0x06    /* R/W ALERT 屏蔽/使能（本版不接 ALERT ⇒ 必须写 0） */
#define INA226_REG_ALERT_LIMIT      0x07    /* R/W ALERT 门限（本版不写、不读） */
#define INA226_REG_MANUFACTURER_ID  0xFE    /* R   器件身份（只读） */
#define INA226_REG_DIE_ID           0xFF    /* R   裸片版本（只读） */

/* ===================== 物理量 LSB（★ 出处：SBOS547 §7.5 寄存器表） ===================== */
#define INA226_SHUNT_LSB_NV         2500u   /* 分流器电压：2.5µV/LSB = 2500nV/LSB（有符号） */
#define INA226_BUS_LSB_UV           1250u   /* 总线电压：1.25mV/LSB = 1250µV/LSB（无符号） */
#define INA226_SHUNT_FS_UV          81920u  /* 分流器电压满量程 ±81.92mV（固定增益，无 PGA 档位） */
#define INA226_BUS_FS_UV            40960000u   /* 总线电压满量程 40.96V（36V 器件的工作上限更低） */

/* ===================== CONFIG 位域（★ 出处：go-ina226 逐位给出，见文件头） =====================
 *   bit15      RST（写 1 触发软复位，自清；本驱动 ⛔ 不使用，理由见 `ina226_init()`）
 *   bit14~12   固定 100b（保留，⛔ 不要按"能写"对待）
 *   bit11~9    AVG     平均次数编码
 *   bit8~6     VBUSCT  总线电压转换时间编码
 *   bit5~3     VSHCT   分流器电压转换时间编码
 *   bit2~0     MODE    工作模式
 * ⚠️ 位域**只用来"把编码拼进寄存器"**；驱动不需要"读位域"来做任何判断（唯一的读判据是
 *    写后回读整字相等 ⇒ 回读一致性本身不依赖上面这几行定义正确）。 */
#define INA226_CFG_RST_BIT          0x8000u
#define INA226_CFG_AVG_SHIFT        9u
#define INA226_CFG_VBUSCT_SHIFT     6u
#define INA226_CFG_VSHCT_SHIFT      3u

/* 工作模式码（bit2~0） */
#define INA226_MODE_POWER_DOWN      0x00u
#define INA226_MODE_SHUNT_TRIG      0x01u
#define INA226_MODE_BUS_TRIG        0x02u
#define INA226_MODE_SHUNT_BUS_TRIG  0x03u
#define INA226_MODE_ADC_OFF         0x04u
#define INA226_MODE_SHUNT_CONT      0x05u
#define INA226_MODE_BUS_CONT        0x06u
#define INA226_MODE_SHUNT_BUS_CONT  0x07u   /* ★ 本版选它：分流器与总线连续转换 */

/* ===================== 返回码 ===================== */
#define INA226_OK                   0
#define INA226_ERR_NACK             1   /* 地址/数据无应答：器件不在位 / 地址不是 0x40 */
#define INA226_ERR_BUS              2   /* SDA 被拉住或时钟拉伸超时（`softi2c` 已自动尝试 9 时钟恢复） */
#define INA226_ERR_LOCK             3   /* ★ 没拿到 `g_bus_sens` 的锁：本次事务**未发生**，
                                         *   ⛔ 不得读成"器件不在位"（C-96 ⑥ 的原文口径） */
#define INA226_ERR_PARAM            4   /* 调用方传参错（空指针 / 寄存器地址越界）= 代码 bug */
#define INA226_ERR_VERIFY           5   /* 写后回读不一致（CONFIG / CALIBRATION / MASK_ENABLE） */
#define INA226_ERR_XCHECK           6   /* ★ 第二形态不一致：分流器电压与电流寄存器反算出的电流
                                         *   差超过 `APP_INA226_XCHECK_10MA` ⇒ 本次读数**不可信**，
                                         *   调用方必须按"不可知即不安全"丢弃它（§4.4） */

/**
 * @brief       探测器件是否在位（只发地址；不初始化总线、不清失败计数）
 * @retval      INA226_OK / INA226_ERR_NACK / INA226_ERR_BUS / INA226_ERR_LOCK
 * @note        ★ 调用前必须已经 `softi2c_init(&g_bus_sens)`（启动序列里做一次，⛔ 驱动不得
 *              反复改总线方向：同线上还有 SHT30 与 24C02，切一半别人的事务就是这个后果）。
 */
uint8_t  ina226_probe(void);

/**
 * @brief       写 CONFIG（平均次数/转换时间/连续模式）+ CALIBRATION + MASK_ENABLE=0，逐项回读比对
 * @retval      INA226_OK / INA226_ERR_VERIFY / INA226_ERR_NACK / INA226_ERR_BUS / INA226_ERR_LOCK
 * @note        ★ **不做软复位（RST）**：RST 会把配置打回复位默认值，于是"写进去的"与"读回来的"
 *              之间多出一段必须靠等待/轮询才能越过的窗口，而那段等待在冷上电时正是 C-107 记的
 *              坑（等的是一个还没走起来的计时源）。直接写目标值再回读，既短又只有一种失效形态。
 *              ⚠️ 本函数幂等，可以反复调用（每次都会重写这三个寄存器）。
 */
uint8_t  ina226_init(void);

/**
 * @brief       读一次电流与总线电压（**内部跑两种形态并比对**）
 * @param       curr_10ma  输出：电流，单位 **10mA**，★ 充正放负（符号由分流器接线决定，
 *                         见 `apps` 侧口径：`Tech_Design.md` §4.4 / D10 判据①）
 *                         取的是**分流器电压反算值**（不依赖 CAL 写对）
 * @param       vbus_10mv  输出：总线电压，单位 **10mV**（与 §4.4 / Modbus 0x0011 同量纲）
 *                         两者都允许传 NULL（"只想拿一个"），但至少给一个
 * @retval      INA226_OK        两种形态一致，两个输出都可信
 *              INA226_ERR_XCHECK 两种形态不一致：**输出已按分流器那条填好**，但调用方
 *                                必须把这次读数按不可信处理（⛔ 不得只挑一个用）
 *              INA226_ERR_*      通信类失败，输出内容无意义
 * @note        ⛔ 本函数内部**不写任何全局判据**、不 commit、不碰快照 —— 发布是调用方的事
 *              （§4.2 单向数据流：BSP → app_acq → …）。
 */
uint8_t  ina226_read(int16_t *curr_10ma, uint16_t *vbus_10mv);

/**
 * @brief       上电自检：探测 → 配置并回读 → 读一次 → 记录两种形态
 * @retval      第一个失败步骤的返回码；全过 = INA226_OK
 * @note        ⚠️ **首次转换的时序**：配置写完时器件刚从复位默认态进入连续转换，第一次
 *              "总线电压"读到的是**配置生效前的那一次转换**（或 0）。本函数**不等**它
 *              （约 `APP_INA226_REFRESH_US` ≈ 8.8ms）—— 自检的判据是"寄存器写得进、读得回、
 *              两条形态自洽"，不依赖读数是不是最新一帧；要一帧新鲜值，等一次 ACQ 节拍即可。
 *              ⛔ 不得为了"让打印好看"在这里加 `delay_ms`：那会把一个可观测的时序事实
 *              变成一句"我猜它已经转换完了"。
 */
uint8_t  ina226_selftest(void);

/**
 * @brief       读器件身份（**诊断用**，⛔ 不参与任何判据，见文件头那条 ⚠️）
 * @param       mfg_id / die_id  允许传 NULL
 * @retval      读成功 = INA226_OK；否则 INA226_ERR_*
 * @note        为什么单列一个出口而不是塞进 `ina226_selftest()` 的返回值：这两个数的**期望值
 *              本轮没有取到手册原文**，把它们写进判据就等于把一条未经核实的常量当门禁
 *              （§3.1 ⑤）。打印出来、由 D10 首次上机登记基线，才是"判据可核"的形态。
 */
uint8_t  ina226_id_read(uint16_t *mfg_id, uint16_t *die_id);

/**
 * @brief       **取证出口（⛔ 不参与任何判据）**：取最近一次 `ina226_read()` 的两个形态与残差
 * @param       shunt_10ma / cal_10ma / vbus_10mv  允许传 NULL
 * @note        口径同 `app_bus.c` 的 `contend`/`lock_to`：它回答"上一次读的两条通路各是多少"，
 *              不回答"该不该动作"。★ 它是本驱动"每条判据要有第二形态"的**存在性探针**
 *              —— 只打印一个电流值的话，"两种形态一致"与"我根本没比"完全同形
 *              （C-110 那条结构纪律在本工程第四次命中，这次在判据设计阶段挡住）。
 */
void     ina226_last_read(int16_t *shunt_10ma, int16_t *cal_10ma, uint16_t *vbus_10mv);

/**
 * @brief       取累计的"两形态不一致"次数（诊断用；⛔ 不参与任何判据）
 */
uint32_t ina226_xcheck_fail_count(void);

#endif
