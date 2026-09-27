/**
 ****************************************************************************************************
 * @file        softi2c.c
 * @brief       软件 I2C 主机实现（**单实现 · 双实例**，C-39）
 * @note        实例：`g_bus_afe` = PD11(SCL)/PD12(SDA)（AFE 独占）；`g_bus_sens` = PB6/PB7
 *              （INA226 + SHT30 + 24C02）。时序代码只有一份，**状态按总线分开**。
 * @note        移植来源：本项目自写（参考 Drivers/BSP/SHT30/sht30.c 旧版位操作时序，但独立实现）。
 *              改动点：① v1 增加**时钟拉伸等待**与 **SDA 卡死检测**（sht30 版没有，AFE 总线必须抗干扰）；
 *                      ② v1 增加**重复起始**多字节读（VC1~VC5 高低字节须同事务连读才原子）；
 *                      ③ v1 增加 **9 脉冲 + STOP 总线恢复**与失败计数（C-12 / S2 链路健康）；
 *                      ④ ★ v2（D3 / C-39）：引脚与失败计数从"编译期宏 + 文件级 static"改为
 *                         `softi2c_bus_t` 实例参数 —— 单实现·双实例，SHT30 的偶发 NACK 再也
 *                         抬不动 AFE 的失败计数（⛔ 反之也不许把传感从机调进 `g_bus_afe`，S6）。
 *                      ⑤ ★ v3（D5 / C-96 ⑥）：三个事务入口按 `bus->lock/unlock` 钩子成对加解锁，
 *                         新增返回码 `I2C_ERR_LOCK`（事务**未发生**，⛔ 不得当"器件不在位"解读）。
 *                         `softi2c_init()` 与 `softi2c_release_bus()` **不**加锁 —— 前者只在启动
 *                         序列跑一次，后者会被事务内部调用（加了就是自锁）。
 *              验证方式：D6 上电后 afe_self_test() 实机验证 AFE 总线 + D3 台架逻辑分析仪抓 PB6/PB7
 *              （含 CRC 覆盖范围核实、24C02 共存读写、拔 SHT30 不阻塞 INA226）。
 *              ★ D5 追加（`Build.md` §8.4 V8）：两任务并发打 SHT30 与 24C02 各 1000 次 ⇒ 两条总线
 *              `fail` 增量 = 0、无撕裂帧。⚠️ 截至本次落盘未上机。
 ****************************************************************************************************
 */

#include "./BSP/SOFTI2C/softi2c.h"
#include "./SYSTEM/delay/delay.h"

/* ===================== 引脚操作（全部经 bus 取引脚） ===================== */
#define SCL_H(bus)      HAL_GPIO_WritePin((bus)->port, (bus)->scl_pin, GPIO_PIN_SET)
#define SCL_L(bus)      HAL_GPIO_WritePin((bus)->port, (bus)->scl_pin, GPIO_PIN_RESET)
#define SDA_H(bus)      HAL_GPIO_WritePin((bus)->port, (bus)->sda_pin, GPIO_PIN_SET)
#define SDA_L(bus)      HAL_GPIO_WritePin((bus)->port, (bus)->sda_pin, GPIO_PIN_RESET)
#define SDA_READ(bus)   (HAL_GPIO_ReadPin((bus)->port, (bus)->sda_pin) == GPIO_PIN_SET)
#define SCL_READ(bus)   (HAL_GPIO_ReadPin((bus)->port, (bus)->scl_pin) == GPIO_PIN_SET)

#define I2C_DELAY()     delay_us(SOFTI2C_HALF_PERIOD_US)

/* 开漏模式下写 1 后 IDR 读回的是总线真实电平 —— 所以既能驱动也能输入，无需切换模式 */
#define WAIT_LIMIT      500u    /* 单次等待上限(约 0.5ms)：超时即判总线故障，绝不无界等待 */

/* ===================== 两条总线的实例 ===================== */
softi2c_bus_t g_bus_afe  = { SOFTI2C_AFE_GPIO_PORT,  SOFTI2C_AFE_SCL_PIN,  SOFTI2C_AFE_SDA_PIN,  0, 0, 0 };
softi2c_bus_t g_bus_sens = { SOFTI2C_SENS_GPIO_PORT, SOFTI2C_SENS_SCL_PIN, SOFTI2C_SENS_SDA_PIN, 0, 0, 0 };

/* ===================== 事务仲裁（D5 / C-96 ⑥） =====================
 * 封装成两个函数的唯一理由：**加锁与解锁必须成对**，写成三处内联的话漏掉一处
 * 就是"这条总线永远锁着"，而这种缺陷只在跑了很久之后才显形。
 * ⚠️ `g_bus_afe` 的钩子恒为 0（S6：保护路径不得被锁阻塞）⇒ 这里等价于直接放行。 */
static uint8_t bus_take(softi2c_bus_t *bus)
{
    if ((bus->lock != 0) && (bus->lock(bus) != 0))
    {
        bus->fail++;                  /* 拿不到锁也是这条总线的一次失败，⛔ 不得静默 */
        return 1;
    }

    return 0;
}

static void bus_give(softi2c_bus_t *bus)
{
    if (bus->unlock != 0)
    {
        bus->unlock(bus);
    }
}

/* ===================== 底层时序 ===================== */

/**
 * @brief       开时钟。F1 的 `HAL_GPIO_Init()` **不**代开 GPIO 时钟（见其注释），
 *              漏开时钟的症状是引脚毫无反应且不报错 —— 所以这里显式列全本工程可能用到的端口。
 */
static void gpio_clk_enable(GPIO_TypeDef *port)
{
    if (port == GPIOA)
    {
        __HAL_RCC_GPIOA_CLK_ENABLE();
    }
    else if (port == GPIOB)
    {
        __HAL_RCC_GPIOB_CLK_ENABLE();
    }
    else if (port == GPIOC)
    {
        __HAL_RCC_GPIOC_CLK_ENABLE();
    }
    else if (port == GPIOD)
    {
        __HAL_RCC_GPIOD_CLK_ENABLE();
    }
    else if (port == GPIOE)
    {
        __HAL_RCC_GPIOE_CLK_ENABLE();
    }
    else if (port == GPIOG)
    {
        __HAL_RCC_GPIOG_CLK_ENABLE();
    }
    else
    {
        /* GPIOF 在 144 脚（ZET6）不存在；落到这里只能是引脚配错，让调用方的后续读写成"无应答"而不是静默 */
    }
}

/**
 * @brief       拉高 SCL 并等待从机释放（时钟拉伸）
 * @retval      I2C_OK / I2C_ERR_BUS
 */
static uint8_t scl_raise(softi2c_bus_t *bus)
{
    uint32_t t = 0;

    SCL_H(bus);

    while (!SCL_READ(bus))
    {
        if (++t >= WAIT_LIMIT)
        {
            return I2C_ERR_BUS;     /* 时钟被拉死：SDA/SCL 短路、从机锁死或上拉失效 */
        }

        delay_us(1);
    }

    I2C_DELAY();

    return I2C_OK;
}

static void i2c_start(softi2c_bus_t *bus)
{
    SDA_H(bus);
    SCL_H(bus);
    I2C_DELAY();
    SDA_L(bus);                     /* SCL 高期间 SDA 下降 = START */
    I2C_DELAY();
    SCL_L(bus);
    I2C_DELAY();
}

static void i2c_stop(softi2c_bus_t *bus)
{
    SDA_L(bus);
    SCL_H(bus);
    I2C_DELAY();
    SDA_H(bus);                     /* SCL 高期间 SDA 上升 = STOP */
    I2C_DELAY();
}

/**
 * @brief       发一个字节并取应答
 * @retval      I2C_OK(ACK) / I2C_ERR_NACK / I2C_ERR_BUS
 */
static uint8_t i2c_write_byte(softi2c_bus_t *bus, uint8_t dat)
{
    uint8_t i;
    uint8_t ack;

    for (i = 0; i < 8; i++)
    {
        if (dat & 0x80)
        {
            SDA_H(bus);
        }
        else
        {
            SDA_L(bus);
        }

        dat <<= 1;
        I2C_DELAY();

        if (scl_raise(bus) != I2C_OK)
        {
            SCL_L(bus);
            return I2C_ERR_BUS;
        }

        SCL_L(bus);
        I2C_DELAY();
    }

    SDA_H(bus);                     /* 释放 SDA 让从机回 ACK */
    I2C_DELAY();

    if (scl_raise(bus) != I2C_OK)
    {
        SCL_L(bus);
        return I2C_ERR_BUS;
    }

    ack = SDA_READ(bus) ? 1 : 0;
    SCL_L(bus);
    I2C_DELAY();

    return ack ? I2C_ERR_NACK : I2C_OK;
}

/**
 * @brief       读一个字节并回 ACK
 * @param       ack: 1 = 回 ACK（还有后续字节）；0 = 回 NACK（最后一个字节）
 */
static uint8_t i2c_read_byte(softi2c_bus_t *bus, uint8_t ack, uint8_t *out)
{
    uint8_t i;
    uint8_t dat = 0;

    SDA_H(bus);                     /* 释放 SDA 交由从机驱动 */

    for (i = 0; i < 8; i++)
    {
        dat <<= 1;

        if (scl_raise(bus) != I2C_OK)
        {
            SCL_L(bus);
            return I2C_ERR_BUS;
        }

        if (SDA_READ(bus))
        {
            dat |= 0x01;
        }

        SCL_L(bus);
        I2C_DELAY();
    }

    if (ack)
    {
        SDA_L(bus);
    }
    else
    {
        SDA_H(bus);
    }

    I2C_DELAY();

    if (scl_raise(bus) != I2C_OK)
    {
        SCL_L(bus);
        return I2C_ERR_BUS;
    }

    SCL_L(bus);
    I2C_DELAY();
    SDA_H(bus);                     /* 交还总线 */
    I2C_DELAY();

    *out = dat;

    return I2C_OK;
}

/* ===================== 对外接口 ===================== */

/**
 * @brief       连读 `rlen` 字节（除最后一个外全部回 ACK）—— 两种读事务共用同一段实现
 */
static uint8_t i2c_read_burst(softi2c_bus_t *bus, uint8_t *rd, uint8_t rlen)
{
    uint8_t i;
    uint8_t err = I2C_OK;

    for (i = 0; (err == I2C_OK) && (i < rlen); i++)
    {
        err = i2c_read_byte(bus, (i < (rlen - 1u)) ? 1 : 0, &rd[i]);
    }

    return err;
}

/**
 * @brief       初始化一条总线（★ 不加锁：本函数只在启动序列跑一次，此刻调度器还没起来）
 */
void softi2c_init(softi2c_bus_t *bus)
{
    GPIO_InitTypeDef gpio;

    if (bus == 0)
    {
        return;
    }

    gpio_clk_enable(bus->port);

    gpio.Pin   = bus->scl_pin | bus->sda_pin;
    gpio.Mode  = GPIO_MODE_OUTPUT_OD;     /* 开漏：靠上拉出高，天然线与，不会双向冲突 */
    gpio.Pull  = GPIO_PULLUP;             /* 内部 ~40K 仅作台架兜底；正式硬件必须外接 4.7K/3.3V */
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;    /* 100kHz 用不到高速档，留余量改善边沿 */
    HAL_GPIO_Init(bus->port, &gpio);

    SCL_H(bus);
    SDA_H(bus);
    I2C_DELAY();

    bus->fail = 0;

    softi2c_release_bus(bus);             /* 上次掉电可能把总线停在半帧状态 */
}

void softi2c_release_bus(softi2c_bus_t *bus)
{
    uint8_t i;

    if (bus == 0)
    {
        return;
    }

    SDA_H(bus);

    for (i = 0; i < 9; i++)               /* 从机等第 9 个时钟释放 SDA */
    {
        SCL_H(bus);
        I2C_DELAY();
        SCL_L(bus);
        I2C_DELAY();

        if (SDA_READ(bus))
        {
            break;
        }
    }

    i2c_stop(bus);
}

uint8_t softi2c_write(softi2c_bus_t *bus, uint8_t addr7, const uint8_t *buf, uint8_t len)
{
    uint8_t err = I2C_OK;
    uint8_t i;

    if (bus == 0)
    {
        return I2C_ERR_PARAM;
    }

    if ((len != 0) && (buf == 0))
    {
        return I2C_ERR_PARAM;
    }

    if (bus_take(bus) != 0u)
    {
        return I2C_ERR_LOCK;
    }

    i2c_start(bus);

    err = i2c_write_byte(bus, (uint8_t)(addr7 << 1));

    for (i = 0; (err == I2C_OK) && (i < len); i++)
    {
        err = i2c_write_byte(bus, buf[i]);
    }

    i2c_stop(bus);

    if (err != I2C_OK)
    {
        bus->fail++;                      /* ★ 只记在本总线的账上（C-39） */
        softi2c_release_bus(bus);
    }

    bus_give(bus);

    return err;
}

uint8_t softi2c_read(softi2c_bus_t *bus, uint8_t addr7, const uint8_t *wr, uint8_t wlen, uint8_t *rd, uint8_t rlen)
{
    uint8_t err = I2C_OK;
    uint8_t i;

    if ((bus == 0) || (rd == 0) || (rlen == 0) || (wr == 0) || (wlen == 0))
    {
        return I2C_ERR_PARAM;
    }

    if (bus_take(bus) != 0u)
    {
        return I2C_ERR_LOCK;
    }

    i2c_start(bus);

    err = i2c_write_byte(bus, (uint8_t)(addr7 << 1));

    for (i = 0; (err == I2C_OK) && (i < wlen); i++)
    {
        err = i2c_write_byte(bus, wr[i]);
    }

    if (err == I2C_OK)
    {
        i2c_start(bus);                               /* 重复起始，不 STOP（§8.3 图 8-4） */
        err = i2c_write_byte(bus, (uint8_t)((addr7 << 1) | 1u));
    }

    if (err == I2C_OK)
    {
        err = i2c_read_burst(bus, rd, rlen);
    }

    i2c_stop(bus);

    if (err != I2C_OK)
    {
        bus->fail++;                                  /* ★ 只记在本总线的账上（C-39） */
        softi2c_release_bus(bus);
    }

    bus_give(bus);

    return err;
}

uint8_t softi2c_read_only(softi2c_bus_t *bus, uint8_t addr7, uint8_t *rd, uint8_t rlen)
{
    uint8_t err;

    if ((bus == 0) || (rd == 0) || (rlen == 0))
    {
        return I2C_ERR_PARAM;
    }

    if (bus_take(bus) != 0u)
    {
        return I2C_ERR_LOCK;
    }

    i2c_start(bus);

    err = i2c_write_byte(bus, (uint8_t)((addr7 << 1) | 1u));

    if (err == I2C_OK)
    {
        err = i2c_read_burst(bus, rd, rlen);
    }

    i2c_stop(bus);

    if (err != I2C_OK)
    {
        bus->fail++;
        softi2c_release_bus(bus);
    }

    bus_give(bus);

    return err;
}

uint32_t softi2c_fail_count(const softi2c_bus_t *bus)
{
    return (bus == 0) ? 0u : bus->fail;
}

void softi2c_clear_fail(softi2c_bus_t *bus)
{
    if (bus != 0)
    {
        bus->fail = 0;
    }
}

void softi2c_set_lock(softi2c_bus_t *bus,
                      uint8_t (*lock)(softi2c_bus_t *bus),
                      void (*unlock)(softi2c_bus_t *bus))
{
    if (bus == 0)
    {
        return;
    }

    /* 只给一个钩子 = 要么死锁要么漏解锁，两种都比"没有锁"糟 ⇒ 成对才收，否则一律清空 */
    if ((lock == 0) || (unlock == 0))
    {
        bus->lock   = 0;
        bus->unlock = 0;
        return;
    }

    bus->lock   = lock;
    bus->unlock = unlock;
}
