/**
 ****************************************************************************************************
 * @file        softi2c.h
 * @brief       软件 I2C 主机（位操作），**单实现 · 双实例**（C-39 收口）
 * @note        1, 实例划分（引脚口径唯一来源 = `Tech_Design.md` §2）：
 *                  `g_bus_afe`  = PD11(SCL)/PD12(SDA) —— AFE **独占**，S6 的硬件半边；
 *                  `g_bus_sens` = PB6(SCL)/PB7(SDA)  —— INA226 + SHT30 + 24C02 三条非安全器件；
 *              2, ★ 为什么必须是"两个实例"而不是"两套实现"（审查 C-39）：
 *                  S6 的完整含义是"AFE 那条总线不能被别人卡住"，而**同一个失效模式可以从软件侧发生** ——
 *                  若驱动是**单实例·共享失败计数**，SHT30 的偶发 NACK 就会把 AFE 的计数抬过
 *                  `APP_AFE_FAIL_MAX` 门限 → 无意义跳闸。所以：**实现只有一份**（时序/恢复/计数逻辑不许复制），
 *                  **状态按总线分开**（`fail` 各自独立、9 时钟恢复各自独立）。
 *                  ⛔ 反过来也禁止：把三个传感从机搬进 `g_bus_afe`（那是把 S6 从物理层直接违反掉）。
 *              3, ⛔ **禁止在中断里调用**（S8：阻塞式 I2C 等待）。所有等待都有超时计数，不会死循环。
 *              4, 电平：AFE 总线的 SCL/SDA 上拉**只能接 3.3V**（REGOUT/ALERT 绝对最大 3.6V，接 5V 即超规格）。
 *                  A 板必须外接 4.7K 量级上拉；内部 ~40K 上拉只作台架临时兜底，边沿不够陡。
 *              5, ★ 本模块**不认识 CRC**：字节内容由调用者拼好（含寄存器地址与逐字节 CRC），
 *                  这样总线层与协议层解耦，换 AFE 型号不必动这里。
 * @note        移植来源：本项目自写（位操作时序参考 `Drivers/BSP/SHT30/sht30.c` 的旧私有实现，独立成模块）。
 *              改动点（v2，D3 / C-39）：由"编译期宏绑死 PD11/PD12"改为**传总线指针**，
 *                  新增 `g_bus_sens`(PB6/PB7) 实例，`sht30.c` 的私有位带删除后改调本模块。
 *              改动点（v3，D5 / C-96 ⑥）：总线结构体新增 **`lock`/`unlock` 事务仲裁钩子**，三个事务入口
 *                  成对加解；新增返回码 `I2C_ERR_LOCK`；`softi2c_init()`/`softi2c_release_bus()` **不**加锁
 *                  （前者只在启动序列跑一次，后者会被事务内部调用 = 会自锁）。
 *              验证方式：① D5 `afe_self_test()` 实机验证 AFE 总线；② D3 台架 —— 逻辑分析仪抓 PB6/PB7 波形，
 *                  确认 SHT30 读通 + 同一总线 24C02 仍可读写 + 拔 SHT30 不阻塞 INA226；
 *                  ★ D5 追加：两任务并发打 SHT30 与 24C02 各 1000 次 ⇒ 两条总线 `fail` 增量 = 0（`Build.md` §8.4 V8）；
 *                  ③ 编译期 G1 0E/0W + `Tools/gate_check.sh`。⚠️ 截至本次落盘**未上机**。
 ****************************************************************************************************
 */

#ifndef __SOFTI2C_H
#define __SOFTI2C_H

#include "./SYSTEM/sys/sys.h"

/* ===================== 实例（引脚定义见 `Tech_Design.md` §2，此处为代码落点） ===================== */
#define SOFTI2C_AFE_GPIO_PORT       GPIOD
#define SOFTI2C_AFE_SCL_PIN         GPIO_PIN_11
#define SOFTI2C_AFE_SDA_PIN         GPIO_PIN_12

#define SOFTI2C_SENS_GPIO_PORT      GPIOB
#define SOFTI2C_SENS_SCL_PIN        GPIO_PIN_6
#define SOFTI2C_SENS_SDA_PIN        GPIO_PIN_7

/* ===================== 返回码 ===================== */
#define I2C_OK                    0
#define I2C_ERR_NACK              1   /* 地址/数据无应答：器件不在位、未启动、地址错 */
#define I2C_ERR_BUS               2   /* SDA 被拉住不放或时钟拉伸超时 —— 已自动尝试总线恢复 */
#define I2C_ERR_PARAM             3
#define I2C_ERR_LOCK              4   /* ★ 没拿到总线锁（D5 / C-96 ⑥）：本次事务**未发生**，⛔ 不得当"器件不在位"解读 */

/* ===================== 速率 ===================== */
/* SCL 半周期延时。bq769x0 的 I2C 标准模式 100kHz（§8.3），取 3us → 约 100kHz。
 * 两条总线共用同一节拍：传感总线上的三颗器件都跑得动 100kHz，没有分开的理由。 */
#define SOFTI2C_HALF_PERIOD_US    3

/**
 * @brief       一条总线的全部状态：**引脚 + 独立失败计数 + 事务仲裁钩子**
 * @note        除 `fail` 之外没有可变状态 —— 时序逻辑全在静态函数里，靠参数取引脚，
 *              所以加第三个实例不需要改实现（但⛔不要为了这个可能性提前加字段）。
 */
typedef struct softi2c_bus_s
{
    GPIO_TypeDef *port;     /* SCL/SDA 同口：F1 的中断按 GPIOx 分组，异口会白耗一次读 */
    uint16_t      scl_pin;
    uint16_t      sda_pin;
    uint32_t      fail;     /* 本总线累计失败（NACK/总线/拿不到锁），供链路健康判定（S2） */

    /* ★ **事务仲裁钩子**（D5 / `附录C` C-96 ⑥）：上调度器后"D3 靠单线程超循环天然串行"这个
    *   保证就消失了 —— 两个任务各发一半帧会互相咬出 NACK，而 NACK 记的是**同一条总线的账**，
    *   于是"别人的撕裂"被读成"这颗器件坏了"。
    *   ⇒ 锁**加在驱动的三个事务入口**（`write` / `read` / `read_only`），⛔ 不要求调用者自觉：
    *     新增一个从机驱动（D10 的 INA226）不需要记得加锁，也就不会漏。
    *   ⚠️ **只有 `g_bus_sens` 注册钩子**：`g_bus_afe` 必须永远 `lock == NULL`（S6 / S1~S3 ——
    *     保护路径不得被任何锁阻塞）。这个不对称是**故意的**，⛔ 不要"为了统一"给 AFE 也加。
    *   ⚠️ 钩子为空 = 单线程期（含调度器启动前的初始化）直接放行，不是漏配。 */
    uint8_t (*lock)(struct softi2c_bus_s *bus);     /* 0 = 拿到锁 */
    void    (*unlock)(struct softi2c_bus_s *bus);
} softi2c_bus_t;

extern softi2c_bus_t g_bus_afe;     /* PD11/PD12 —— AFE 独占（S6） */
extern softi2c_bus_t g_bus_sens;    /* PB6/PB7  —— INA226 + SHT30 + 24C02 */

/**
 * @brief       初始化一条总线：配开漏输出、释放电平、跑一遍总线恢复
 * @note        ★ 每条总线**只在启动序列里初始化一次**（多从机共用同一条线，重复 init 会把
 *              别人的事务切一半）。运行期的恢复走 `softi2c_release_bus()`。
 */
void     softi2c_init(softi2c_bus_t *bus);

/**
 * @brief       总线恢复：最多 9 个 SCL 脉冲 + STOP
 * @note        从机在等第 9 个时钟释放 SDA 时会解锁；断电不需要。对应验收项 C-12。
 */
void     softi2c_release_bus(softi2c_bus_t *bus);

/**
 * @brief       纯写事务：S addr+W [字节...] P
 * @param       bus    目标总线（决定用哪两个脚，以及失败记在谁的账上）
 * @param       addr7  7 位从机地址（不含 R/W）
 * @param       buf    待发送字节（len=0 时允许 NULL，用于只发地址做探测）
 * @retval      I2C_OK / I2C_ERR_NACK / I2C_ERR_BUS
 */
uint8_t  softi2c_write(softi2c_bus_t *bus, uint8_t addr7, const uint8_t *buf, uint8_t len);

/**
 * @brief       写寄存器地址后**重复起始**再读：S addr+W reg... Sr addr+R data... P
 * @param       bus    目标总线
 * @param       addr7  7 位从机地址
 * @param       wr     写阶段字节（至少 1 个 = 寄存器地址）
 * @param       rd     读阶段缓冲
 * @param       rlen   读字节数；除最后一个外全部回 ACK
 */
uint8_t  softi2c_read(softi2c_bus_t *bus, uint8_t addr7, const uint8_t *wr, uint8_t wlen, uint8_t *rd, uint8_t rlen);

/**
 * @brief       **纯读**事务：S addr+R data... P（没有写阶段，也就不需要重复起始）
 * @param       bus    目标总线
 * @param       addr7  7 位从机地址
 * @param       rd     读阶段缓冲
 * @param       rlen   读字节数；除最后一个外全部回 ACK
 * @note        为什么单独给一个原语而不是让 `softi2c_read()` 允许 `wlen=0`：
 *              命令型器件（SHT30 读温湿度）的"发命令"和"取数据"是**两个事务**，
 *              把 wlen=0 塞进写-读混合事务里会让调用者误以为可以省掉 START 的差别。
 *              ⛔ 这不是第三套位带 —— 时序代码仍然只有本文件那一份（C-39）。
 */
uint8_t  softi2c_read_only(softi2c_bus_t *bus, uint8_t addr7, uint8_t *rd, uint8_t rlen);

/**
 * @brief       取某条总线的累计失败次数（⛔ 不许跨总线相加，C-39）
 */
uint32_t softi2c_fail_count(const softi2c_bus_t *bus);

void     softi2c_clear_fail(softi2c_bus_t *bus);

/**
 * @brief       登记事务仲裁钩子（★ 只给非安全总线用；`g_bus_afe` ⛔ 永不调用本函数）
 * @param       lock    返回 0 = 拿到锁；非 0 = 超时/失败 ⇒ 事务**不发生**，返回 `I2C_ERR_LOCK`
 * @param       unlock  与 `lock` 成对；两者必须同时给（缺一个就是死锁或漏解锁，故一律拒收）
 * @note        ⛔ 禁止在中断里持有本锁（S8：ISR 不做 I2C）—— 钩子内部用的是会阻塞的信号量。
 */
void     softi2c_set_lock(softi2c_bus_t *bus,
                          uint8_t (*lock)(softi2c_bus_t *bus),
                          void (*unlock)(softi2c_bus_t *bus));

#endif
