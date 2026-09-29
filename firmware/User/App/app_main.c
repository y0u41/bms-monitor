/**
 ****************************************************************************************************
 * @file        app_main.c
 * @brief       应用主逻辑: 温湿度(DHT11/SHT30) + 光敏 采集 -> 超限报警 -> LCD 本地显示
 *              可选: ESP8266 -> OneNET 上报
 * @note        1, 采集与上报永远工作, 联网是"尽力而为", 任何一步失败都只影响状态打印, 绝不阻塞主循环;
 *              2, 主循环按 20ms 节拍跑: 按键+刷屏每 20ms, 采集每 APP_SAMPLE_PERIOD_MS;
 *              3, APP_ENABLE_NET = 0 时完全不碰 ESP8266 / OneNET;
 *              4, ★ 界面已于 2026-09-26（Build.md D2）**整体交接给 `app_ui.c`** 的 BMS 三页界面：
 *                 本文件不再画任何像素，原先那套单页监护界面（`ui_setup`/`ui_show`/
 *                 `ui_redraw_all`/报警整屏闪烁/报警历史页）已整块删除。
 *                 ⛔ 删的**只有画像素的部分**：采样、报警（蜂鸣器+消音+迟滞）、看门狗、
 *                 OneNET 上报的判据与调用顺序一条未改，环境量状态改由 printf 出口。
 *              5, 本文件与 CubeMX 生成的代码完全隔离, 重新生成代码不会覆盖。
 ****************************************************************************************************
 */

#include "./App/app_main.h"
#include "./App/app_config.h"
#include "./App/app_env_alarm.h"
#include "./App/app_onenet.h"
#include "./App/app_mqtt.h"
#include "./App/app_log.h"
#include "./App/app_ui.h"
#include "./App/app_data.h"
#include "./App/app_acq.h"
#include "./App/app_protect.h"      /* ★ D5：唯一喂狗点/状态属主所在（本文件只注册钩子，不碰 IWDG） */
#include "./App/app_bus.h"          /* ★ D5：传感总线互斥（M3） */
#include "./App/app_modbus.h"       /* ★ Modbus RTU 从机（D9 提前，见其文件头） */
#include "./BSP/RS485/rs485.h"      /* ★ 板载 RS485 物理层（USART2 + PD7） */
#include "./App/app_tasks.h"        /* ★ D5：app_tasks_create() + 服务回调里补心跳 */
#include "./SYSTEM/usart/usart.h"   /* ★ v0.66：打印路径出口（上电段原文回放等）+ USART1 寄存器 */
#include "FreeRTOS.h"
#include "task.h"                   /* vTaskStartScheduler() / taskDISABLE_INTERRUPTS() */
#include "./BSP/LED/led.h"
#include "./BSP/LCD/lcd.h"
#include "./BSP/DHT11/dht11.h"
#include "./BSP/SHT30/sht30.h"
#include "./BSP/SOFTI2C/softi2c.h"
#include "./BSP/AT24CXX/at24cxx.h"
#include "./BSP/INA226/ina226.h"
#include "./App/app_cfg.h"          /* ★ 2026-09-29：门限参数区（§8 协议 / 附录C C-121） */
#include "./App/app_alarm.h"        /* ★ 2026-09-29：BMS 三级告警（§5.3 / 附录C C-123） */
#include "./App/app_soc.h"          /* ★ 2026-09-29：派生层（bms_derived_t 的唯一写者） */
#include "./App/app_calib.h"        /* ★ 2026-09-29：标定表区（§8 协议 / 附录C C-127） */
#include "./BSP/AFE/afe.h"         /* ★ D8：硬件门限编码往返自检 */
#include "./BSP/ADC/adc.h"          /* ★ D11 前置：ADC1（PA5/PA6） */
#include "./BSP/NTC/ntc.h"          /* ★ D11 前置：NTC 查表换算 */    /* ★ D10 前置：驱动本体 + 上电自检（⛔ 本文件不把它接进采集通路） */
#include "./BSP/LSENS/lsens.h"
#include "./BSP/ESP8266/esp8266.h"
#include "./BSP/KEY/key.h"
#include "./BSP/RTC/rtc.h"
#include "./SYSTEM/delay/delay.h"
#include <stdio.h>
#include <string.h>

/* ===================== 界面所有权（D2 交接） =====================
 * LCD 归 `app_ui.c`。两套绘制叠在同一屏上必然互相盖字，而"谁最后画谁赢"的
 * 界面没法验收 —— 所以遗留界面的行缓存、字号计算、历史页与整屏闪烁全部删除，
 * 本文件里 ⛔ 不得再出现 lcd_show_string / lcd_clear。
 * 按键同理：`key_scan()` 不再被调用，短按/长按由 `app_ui.c` 的状态机判定。
 * ==================================================================*/

/* ===================== 温湿度传感器类型 ===================== */
#define TH_NONE         0
#define TH_DHT11        1
#define TH_SHT30        2

/* th_read() 的返回码。★ "转换中(BUSY)"这一档不在这里了 —— SHT30 的两阶段流水由 `app_acq`
 * 自己消化（读不到好帧就当本次无效），本层只见"有值/没值"两种结果。 */
#define TH_RD_OK        0
#define TH_RD_FAIL      1
#define TH_RD_NOSENSOR  2

/* DHT11 判"0/1"靠 delay_us(40) 后读引脚电平, 而 "0" 的高电平只有 26~28us;
 * 115200 下 USART3 每约 87us 进一次中断, 会把采样点推后 -> 偶发 CRC 失败。
 * 所以读单总线器件期间暂停 USART3 中断(详见 esp8266.h)。没开联网时是空操作。
 * 注意只关 USART3, 不动 SysTick, HAL_GetTick()/delay_us() 不受影响。 */
#if APP_ENABLE_NET
#define TH_BUS_LOCK()       esp8266_rx_pause()
#define TH_BUS_UNLOCK()     esp8266_rx_resume()
#else
#define TH_BUS_LOCK()       do { } while (0)
#define TH_BUS_UNLOCK()     do { } while (0)
#endif

/* ===================== 联网状态机 ===================== */
#define NET_ST_START        0
#define NET_ST_AT           1
#define NET_ST_ECHO         2
#define NET_ST_MODE         3
#define NET_ST_JOIN         4
#define NET_ST_TCP          5
#define NET_ST_TRANS        6
#define NET_ST_MQTT         7
#define NET_ST_ONLINE       8
#define NET_ST_RETRYWAIT    9

static uint8_t  g_th_type = TH_NONE;
static uint8_t  g_th_ok = 0;                                /* 最近一次温湿度读取是否有效 */
static uint8_t  g_th_fail = 0;                              /* 连续读取失败次数 */
static uint16_t g_th_period = APP_SAMPLE_PERIOD_MS;         /* 温湿度采样间隔(ms) */

#if APP_ENABLE_NET
static uint8_t  g_net_state;
static uint32_t g_net_timer;
static uint8_t  g_transparent;
static uint8_t  g_baud_scanned;     /* 波特率只全扫一次, 之后每轮只用快命令试, 避免拖慢报警/采样 */
static uint16_t g_net_attempts = 0; /* 第几次建链: 第 2 次起先 AT+RST, 把模块从残留状态复位干净 */
static uint16_t g_net_fails    = 0; /* 连续失败次数: 够多就允许重新全扫描波特率(模块可能自己复位过) */
static uint8_t  g_ping_fail    = 0; /* 连续几次心跳没收到 PINGRESP */
#endif

/* ===================== 独立看门狗 =====================
 * ★ D5（§8.4 M4）：整块搬进 `app_protect.c` —— 狗的钥匙寄存器全工程只允许出现在那一个文件，
 *   喂狗点唯一（TaskProtect 的**条件**喂狗），由 `Tools/gate_check.sh` G7-⑤ 机械检查。
 *   本文件因此 ⛔ 不得再出现喂狗写操作；启动狗的那一次调用改在 `app_main()` 末尾
 *   （`app_protect_wdg_start()`，在 `app_tasks_create()` 之前）。 */

#if APP_ENABLE_LOG
/**
 * @brief       遗留环境报警通路 → BMS 事件日志的适配器（注册给 `alarm_set_log_hook()`）
 * @note        ★ 为什么加一层适配器而不是把 `app_log_add()` 直接注册进去：
 *              `app_alarm.c` 的回调签名是工程带来的四参数形态（faults/温度/湿度/光照），
 *              改它要动 typedef + 调用点 + 注册点三处，而收益只有"少一个函数"。
 *              这里的口径与 §4.4 一致：**跨层只走契约**，形状不匹配就在边界上转一次。
 * @note        ⚠️ 差距声明（附录C · C-101）：**光照 cd/m² 不再入档** —— 一条记录只有两个 aux 口，
 *              按"越限时的组压/电流优先"的约定分给了温度与湿度，光照只留在实时显示里。
 */
static void app_log_env_event(uint8_t faults, int16_t temp_x10, uint16_t humi_x10, uint32_t lux_cdm2)
{
    (void)lux_cdm2;                   /* 显式丢掉，⛔ 不得为了"看起来用上了"塞进 aux 口挤掉湿度 */

    app_log_add(EV_ENV_ALARM, faults, (uint16_t)temp_x10, humi_x10);
}

/**
 * @brief       上电第一件事：把"为什么复位"记进日志（判据：狗咬过必须单独一条）
 * @note        复位标志必须在 `__HAL_RCC_CLEAR_RESET_FLAGS()` 之前读，读完就清 ——
 *              不清的话下一次是正常上电也会被读成"上次复位过"。
 * @note        ★ 自检项"看门狗：人为死循环 → IWDG 复位 → 复位后日志有复位记录、不自动闭合"
 *              的"有记录"这一半就落在这里（另一半"不自动闭合"属 D6/D8 的执行器默认态）。
 */
/* ★★ 2026-09-29：**启动自检的汇总出口**。
 * 为什么必须有：验收轮发现 `ina226_selftest()` / `app_modbus_selftest()` / `rs485_selftest()` 的判定
 * **只喂了 printf**，而本机没有 COM 通路 ⇒ "自检过没过"在板现场**读不到**。
 * 口径：**一处出口覆盖全部启动自检**（⛔ 不逐个模块各开一个，那会变成 N 个落点）。
 * ⚠️ 顺序即下面 `APP_SELF_*` 的编号，⛔ 改动要与读侧（调试器/自检表）同批。 */
#define APP_SELF_CFG        0u
#define APP_SELF_ALARM      1u
#define APP_SELF_SOC        2u
#define APP_SELF_INA226     3u
#define APP_SELF_AT24       4u
#define APP_SELF_MODBUS     5u
#define APP_SELF_RS485      6u
#define APP_SELF_N          7u

static uint8_t s_self_rc[APP_SELF_N] = { 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu };

static void self_rec(uint8_t idx, uint8_t rc)
{
    if (idx < (uint8_t)APP_SELF_N) { s_self_rc[idx] = rc; }
}

uint8_t app_selfcheck_rc(uint8_t idx)
{
    return (idx < (uint8_t)APP_SELF_N) ? s_self_rc[idx] : 0xFFu;
}

void app_selfcheck_report(uint8_t *out, uint8_t n)
{
    uint8_t i;

    if (out == NULL) { return; }

    for (i = 0u; (i < n) && (i < (uint8_t)APP_SELF_N); i++) { out[i] = s_self_rc[i]; }
}

static void app_log_boot_event(void)
{
    uint16_t reason = 0u;

    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PINRST)  != 0u) { reason = (uint16_t)(reason | 0x0001u); }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_PORRST)  != 0u) { reason = (uint16_t)(reason | 0x0002u); }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_SFTRST)  != 0u) { reason = (uint16_t)(reason | 0x0004u); }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_IWDGRST) != 0u) { reason = (uint16_t)(reason | 0x0008u); }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_WWDGRST) != 0u) { reason = (uint16_t)(reason | 0x0010u); }
    if (__HAL_RCC_GET_FLAG(RCC_FLAG_LPWRRST) != 0u) { reason = (uint16_t)(reason | 0x0020u); }

    __HAL_RCC_CLEAR_RESET_FLAGS();

    /* 狗咬过单独提一条：页 3 上"WDG-RST"要一眼看得见（判活失败是最不该被埋没的故障） */
    if ((reason & 0x0018u) != 0u)
    {
        app_log_add(EV_WDG_RESET, reason, 0u, 0u);
    }

    app_log_add(EV_BOOT, reason, 0u, 0u);
}
#endif

/**
 * @brief       每 20ms 的公共任务: BMS 界面(按键 + 到点刷屏) + 蜂鸣器报警节拍
 * @note        迁移容器（`app_tasks.c` 的 `task_legacy()`）和"网络等待期间的服务回调"都调它,
 *              所以联网阻塞时 KEY0/KEY1 依然有效(否则 AT+CWJAP 那 20 秒里按什么都没反应)。
 *
 *              ★ D2 的三处交接：
 *              ① 按键与刷屏改由 `app_ui_task()` 负责 —— 它自带消抖与短按/长按判定，
 *                 并且**只读快照、只发请求**（S9/S11）。遗留的 `key_scan()` 从此处拿掉：
 *                 它内部有 `delay_ms(10)` 阻塞，在任务里就是 S12 违例（D5 迁移前也不该留着）。
 *              ② 界面数据来自 `app_data_fake_step()` 的假帧（Build.md D2 口径"数据先填假值"）。
 *                 ⏳ D3 起改成**逐通道交接**：环境温湿度由 `app_acq_step()` 写真实值
 *                 （§4.2 里采集层是 SHT30 的唯一读者），其余通道暂时仍是假值。
 *                 ★ 顺序不可颠倒：真实帧必须写在假帧之后（两者都往 raw 盒 commit）。
 *              ③ `alarm_task()` 必须继续跑：蜂鸣器时序、消音、迟滞与确认窗都和界面无关。
 *
 *              ★ D5（§8.4 M6）拿走了两样东西，⛔ 不要加回来：
 *              ④ **告警帧**：`app_data_link_check()`（= 告警帧的 commit）已从假源尾部搬进
 *                 `app_protect_step()`。留在本函数里 = 告警帧有两个写者（本容器 + TaskProtect），
 *                 两写者共用一个双缓冲盒 = 撕裂帧，而读者只能靠 `app_data_read()` 返回 2 发现。
 *              ⑤ **落盘**：`app_log_poll()` 搬进 `TaskLog`。两个调用者同优先级时间片轮转
 *                 会在一次页擦除中途切走（S25 的"整页写"假设就这么没了），所以一个模块
 *                 只能有一个 poll 者。
 */
static void app_periodic(void)
{
    uint32_t now = HAL_GetTick();

    app_data_fake_step(now);
    app_soc_step(now);            /* ★ 派生层唯一写者：从 raw 算极值/差值/温升 + 安时积分 */      /* ⏳ 逐通道交接中：环境通道已由 app_acq 写真数据（D3），其余仍是假值 */
    app_acq_afe_step(now);        /* ★ D7 交接：AFE 单体/组压（AFE 未接 ⇒ 一个字节都不写） */
    app_acq_ina_step(now);        /* ★ D10 交接：INA226 电流/冗余组压（未接 ⇒ 一个字节都不写） */
    app_acq_ntc_step(now);        /* ★ D11 交接：两路电芯 NTC（未接 ⇒ 一个字节都不写） */
    app_acq_step(now);            /* 采集层：SHT30 两阶段读 → 快照的环境温度通道 */
    app_ui_task(now);             /* 按键扫描 + 200ms 限流重绘 */
    alarm_task();
}


#if APP_ENABLE_NET
/**
 * @brief       网络等待期间的服务回调(注册给 ESP8266 驱动)
 * @note        进入联网后, AT+CWJAP 最长等 20s、AT+CIPSTART 最长等 15s, 这期间调用方的
 *              一轮是停住的。没有这个回调时: 蜂鸣器节拍会冻结、KEY0 消音会漏掉、屏幕不更新。
 *
 *              ★ D5（§8.4 M5）改动的两件事，缺一不可：
 *              ① 原来的 `IWDG_FEED()` 换成 **给本容器补心跳**。
 *                 ⚠️ 这等于"**暂时放弃对 legacy 容器的判活**" —— 网络等待期间容器看起来永远活着，
 *                 真卡在这一段里狗不会咬。这是"要么天天被复位、要么接受这一段没判活"的两难里
 *                 选的后者（`AT+CWJAP` 20s 是设计如此，不是故障）。
 *                 ⛔ 不得反过来理解成"那就在等待里无条件喂狗"：那样其余四个任务的判活也会一起失效。
 *                 ⛔ 也不许在这里喂狗：喂狗点唯一（M4）。
 *              ② 补心跳的同时必须确认**等待本身有上限**：`esp8266_wait_ack()` 走的是带超时
 *                 的泵式等待；`esp8266_reset()` 原来是一段 2000ms 的**不泵钩子**死延时 ⇒
 *                 2s 无心跳 > 容器 deadline(80ms) ⇒ **第二次重连必被狗咬**，已改为泵式等待。
 */
static void app_poll(void)
{
    app_tasks_heartbeat(APP_TASK_UI);    /* 容器占用的心跳槽（见 app_tasks.c 的 task_legacy） */
    app_periodic();
}
#endif

/* ============================================================================
 *  温湿度传感器
 * ==========================================================================*/

/**
 * @brief       探测温湿度器件在位（SHT30 优先，其次 DHT11）
 * @note        ★ SHT30 只发地址问"你在不在"（`sht30_probe()`），**不**在这里初始化总线 ——
 *              总线是共享资源，反复 `softi2c_init()` 会把同线上 24C02/INA226 的事务切一半，
 *              还会把独立失败计数（C-39）清零。总线初始化只在启动序列做一次。
 */
static uint8_t th_detect(void)
{
#if (APP_SENSOR_TH == 1)
    return (sht30_probe() == SHT30_OK) ? TH_SHT30 : TH_NONE;
#elif (APP_SENSOR_TH == 0)
    uint8_t r;

    TH_BUS_LOCK();                              /* DHT11 时序敏感, 期间不收串口 */
    r = (dht11_init() == 0) ? TH_DHT11 : TH_NONE;
    TH_BUS_UNLOCK();

    return r;
#else
    uint8_t r;

    if (sht30_probe() == SHT30_OK)
    {
        return TH_SHT30;
    }

    TH_BUS_LOCK();                              /* 软件 I2C 由主机控时钟, 不用锁 */
    r = (dht11_init() == 0) ? TH_DHT11 : TH_NONE;
    TH_BUS_UNLOCK();

    return r;
#endif
}

static uint8_t th_read(int16_t *t_x10, uint16_t *h_x10)
{
    uint8_t t8, h8;
    uint8_t r;

    if (g_th_type == TH_SHT30)
    {
        /* ★ SHT30 的属主现在是 `app_acq`（§1.2：BSP → app_acq → 快照 → 报警）。
         *   两处同时调 `sht30_measure()` 会把两阶段流水互相消费掉（一方取走数据，
         *   另一方只拿到 BUSY），所以这里**只读取值口**，不再自己驱动器件。
         *   DHT11 那半边维持原样：它是遗留工程的后备件（PG11 单总线，不在 I2C 上），
         *   BMS 的环境通道按 `APP_SENSOR_TH=2` 只认 SHT30。 */
        if (app_acq_env_valid() == 0u)
        {
            return TH_RD_FAIL;      /* 还没拿到好帧（或已连续失败判死） */
        }

        *t_x10 = app_acq_env_temp_x10();
        *h_x10 = app_acq_env_humi_x10();

        return TH_RD_OK;
    }

    if (g_th_type == TH_DHT11)
    {
        /* 官方驱动的 CRC 校验失败时不赋值, 却又返回"成功", 这里用哨兵值识别出来,
         * 并把 ACK 失败与 CRC 失败区分开打印, 便于接线排错。 */
        t8 = 0xEE;
        h8 = 0xEE;

        /* ★ 整个读取过程约 25ms, 必须独占时序: 否则每 87us 一次的 USART3 中断
         *   会把 delay_us(40) 后的采样点推后, 把 "0"(高电平 26~28us) 读成 "1"。 */
        TH_BUS_LOCK();
        r = dht11_read_data(&t8, &h8);
        TH_BUS_UNLOCK();

        if (r)
        {
            printf("DHT11: no ACK (check DQ/PG11 wiring & 4.7K pull-up)\r\n");
            return 1;
        }

        if ((t8 == 0xEE) || (h8 == 0xEE))
        {
            printf("DHT11: CRC fail (data line too slow/noisy)\r\n");
            return 1;
        }

        *t_x10 = (int16_t)((int16_t)t8 * 10);
        *h_x10 = (uint16_t)((uint16_t)h8 * 10U);

        return 0;
    }

    return 2;
}

static const char *th_name(void)
{
    if (g_th_type == TH_SHT30)
    {
        return "SHT30";
    }

    if (g_th_type == TH_DHT11)
    {
        return "DHT11";
    }

    return "TH";
}

/* ============================================================================
 *  联网(可选)
 * ==========================================================================*/
#if APP_ENABLE_NET

static void net_fail(const char *msg)
{
    printf("NET FAIL: %s (resp: %s)\r\n", msg, esp8266_get_resp());

    LED0(1);

    if (g_net_state >= NET_ST_TCP)
    {
        if (g_transparent)
        {
            esp8266_exit_transparent();
            g_transparent = 0;
        }

        esp8266_cmd("AT+CIPCLOSE", "OK", 2000);
    }

    /* 连续失败够多次 → 允许重新全扫描波特率。
     * 模块若因为供电抖动/自己看门狗复位, 波特率可能回到默认值, 而 g_baud_scanned
     * 一旦置 1 就再也不会重扫, 那种情况下会永久连不上。 */
    if (++g_net_fails >= APP_NET_FAIL_RESCAN)
    {
        g_net_fails    = 0;
        g_baud_scanned = 0;
        printf("NET: rescan baudrate on next try\r\n");
    }

    mqtt_link_clear_closed();
    g_ping_fail = 0;

    g_net_timer = HAL_GetTick();
    g_net_state = NET_ST_RETRYWAIT;
}

static void net_step(void)
{
    if (g_net_state == NET_ST_ONLINE)
    {
        return;
    }

    switch (g_net_state)
    {
        case NET_ST_START:
            if (g_net_attempts > 0)
            {
                /* 重连: 模块可能停在透传 / 多连接 / 半开连接等残留状态,
                 * 直接重发 AT 命令经常一直失败, 先软复位到干净状态。 */
                printf("NET: reset ESP8266 before retry\r\n");

                esp8266_reset();
                (void)esp8266_set_baud(115200);     /* RST 后波特率回到模块默认 */
                esp8266_clear();
                g_baud_scanned = 0;                 /* 波特率可能变了, 允许重扫 */
                mqtt_link_clear_closed();
            }

            g_net_attempts++;

            esp8266_init();
            printf("NET: AT probe\r\n");
            g_net_state = NET_ST_AT;
            break;

        case NET_ST_AT:
        {
            uint8_t ok;

            if (g_baud_scanned)
            {
                ok = (esp8266_at() == ESP8266_STAT_OK) ? 1 : 0;     /* 已探测过: 直接用当前波特率快试 */
            }
            else
            {
                uint16_t rx_bytes = 0;
                uint32_t baud = esp8266_scan_baud(&rx_bytes);

                g_baud_scanned = 1;

                if (baud == 0)
                {
                    /* 按"有没有收到过字节"给出可执行的结论, 而不是笼统的 no module */
                    if (rx_bytes > 0)
                    {
                        net_fail("WiFi  : RX ok,no AT");            /* 接线通, 模块不是 AT 固件/被占用 */
                    }
                    else
                    {
                        net_fail("WiFi  : no resp");                /* 一个字节都没有: 供电/接线/波特率 */
                    }

                    break;
                }

                ok = 1;
            }

            if (ok)
            {
                g_net_state = NET_ST_ECHO;
            }
            else
            {
                net_fail("WiFi  : no module");
            }

            break;
        }

        case NET_ST_ECHO:
            esp8266_echo_off();
            g_net_state = NET_ST_MODE;
            break;

        case NET_ST_MODE:
            esp8266_set_mode(1);
            g_net_state = NET_ST_JOIN;
            break;

        case NET_ST_JOIN:
            printf("WiFi joining [%s] ...\r\n", APP_WIFI_SSID);

            if (esp8266_join_ap(APP_WIFI_SSID, APP_WIFI_PWD) == ESP8266_STAT_OK)
            {
                printf("WiFi OK\r\n");
                g_net_state = NET_ST_TCP;
            }
            else
            {
                net_fail("WiFi  : join FAIL");
            }

            break;

        case NET_ST_TCP:
            printf("NET: open TCP %s\r\n", ONENET_HOST);

            if (esp8266_open_tcp(ONENET_HOST, ONENET_PORT) == ESP8266_STAT_OK)
            {
                g_net_state = NET_ST_TRANS;
            }
            else
            {
                net_fail("Cloud : TCP FAIL");
            }

            break;

        case NET_ST_TRANS:
            if (esp8266_enter_transparent() == ESP8266_STAT_OK)
            {
                g_transparent = 1;
                g_net_state = NET_ST_MQTT;
            }
            else
            {
                net_fail("Cloud : transparent FAIL");
            }

            break;

        case NET_ST_MQTT:
            printf("NET: MQTT CONNECT\r\n");

            if (onenet_connect() == MQTT_STAT_OK)
            {
                printf("MQTT connected to OneNET\r\n");
                LED0(0);
                g_net_fails = 0;
                g_ping_fail = 0;
                g_net_state = NET_ST_ONLINE;
            }
            else
            {
                net_fail("Cloud : MQTT FAIL");
            }

            break;

        case NET_ST_RETRYWAIT:
        default:
            if ((HAL_GetTick() - g_net_timer) >= APP_NET_RETRY_MS)
            {
                printf("NET: retry\r\n");
                g_net_state = NET_ST_START;
            }

            break;
    }
}

#endif /* APP_ENABLE_NET */

/* ============================================================================
 *  主入口
 * ==========================================================================*/

/* ===================== 服务切片的跨轮状态（★ D5 / §8.4 M2） =====================
 * 这些**原来是 `app_main()` 的局部变量**。改成文件级静态不是风格问题：
 *   循环体现在每 20ms 被调一次并**返回**，栈上的计时器一返回就没了 ⇒
 *   `(now - t_sample) >= APP_SAMPLE_PERIOD_MS` 永远在第一次判断就成立 = 每 20ms 采一次样。
 * ⛔ 不得为了"看起来封装更好"把它们塞进结构体 —— 那只是给同一份状态换名字。
 * RAM 账：原来它们也常驻（主循环的栈帧整个生命周期不弹出），所以这次迁移**不增加一个字**。 */
static int16_t  s_t_x10;                        /* 最近一次温度（0.1℃） */
static uint16_t s_h_x10;                        /* 最近一次湿度（0.1%） */
static uint8_t  s_lux;                          /* 光照相对值 0~100 */
static uint32_t s_lux_cdm2;                     /* 光照估计值（cd/m²） */
static uint8_t  s_th_ok;                        /* 本轮温湿度是否可信 */
static uint8_t  s_r_th;                         /* th_read() 返回码 */
static uint32_t s_t_sample;                     /* 采样节拍计时 */
static uint32_t s_t_th;                         /* 温湿度节拍计时 */
#if APP_ENABLE_NET
static uint16_t s_msg_id;                       /* MQTT 报文序号 */
static uint32_t s_sent;                         /* 累计上报条数 */
static uint32_t s_t_report;                     /* 上报节拍计时 */
static uint32_t s_t_ping;                       /* 心跳节拍计时 */
#endif

/**
 * @brief       短按 KEY0 → 遗留环境蜂鸣器消音（注册给 `app_ui_set_ack_hook()`）
 * @note        ★ 这条通路自 D2 界面交接起**是断的**：`key_scan()` 拿掉后 `alarm_key_mute()`
 *              就没有调用者了（表现为"按 KEY0 不停蜂鸣"）。本次迁移顺手接回，不是新功能。
 * @note        为什么 ack 归这里、unlock 归 `app_protect`：短按消音动的是**本地蜂鸣器**
 *              （环境通路，不碰主回路）；长按解除动的是**主回路状态**（S9/S11 的属主）。
 */
static void app_env_mute(void)
{
    (void)alarm_key_mute();
}

/**
 * @brief       应用主入口(main.c 中调用)：只做上电初始化，然后把机器交给调度器
 * @param       无
 * @retval      无（不返回：`vTaskStartScheduler()` 之后要么在任务里，要么等狗咬）
 */
/* ==========================================================================
 * ★★ 2026-09-30（v0.66 = D9 读数出口的开始）串口命令入口（**极小**，⛔ 不是 shell）
 *   只认两个字符：
 *     `?`（或 `h`）= **回放"上电段打印的原文"** —— 真·冷启动的开机行在 COM6 上抓不到
 *                    （CH340 与 MCU 同电 ⇒ USB 枚举 ≈1s 晚于开机打印，见 `附录C` C-144）
 *                    ⇒ 这是它**唯一"事后可取"的形态**。
 *     `s`          = **状态行**（打印路径代价 ＋ 事件队列 ＋ 狗窗现状）。
 *   ★ 四条设计纪律：
 *     ① **非阻塞**：只问 `USART1->SR`，没有字节立刻返回（⛔ 不等待、⛔ 不 delay）；
 *     ② **有界**：一拍最多读 8 字节；`?` 的回放 ≈1KB ≈ 90ms，落在 `TaskLog` 的 200ms 判活窗内；
 *     ③ ⛔ **不认识的字静默丢弃**（不写完整 shell —— D9 的判据不是 shell，也⛔ 不回显）；
 *     ④ ⛔ **只读**：不改任何运行态、不写日志/参数。
 *   ★ 归属：由 `task_log()` 每 50ms 调一次（它就是"打印任务"，窗口装得下这次回放）。
 * ==========================================================================*/
static volatile uint8_t s_diag_ore;   /* 串口输入被 ORE 吃掉的次数（诊断出口，⛔ 不参与判据） */

void app_main_diag_poll(void)
{
    uint8_t n;

    /* ★ 输入丢失必须**看得见**：本入口是**轮询**的（`TaskLog` 每 50ms 一拍），而一个字符只要 87µs
     *   ⇒ 连发多个字符时，只有最后一个可能存活，其余被 USART 的**溢出（ORE）**吃掉。
     *   ⇒ 约定：**一次只发一个字符**（人在台架上敲键盘天然满足 ≳100ms 间隔）；
     *     真丢过就计进 `s_diag_ore` 并在 `STAT` 行打出来（⛔ 不静默）。
     *   ⏳ D9 后续（登记在案）：要支持"一串命令"就得给它一个 **RX 中断 + 环形缓冲**，
     *     那是另一笔改动面（NVIC 优先级契约 + 缓冲），⛔ 本轮不做。 */
    if ((USART1->SR & USART_SR_ORE) != 0u)
    {
        if (s_diag_ore < 0xFFu)
        {
            s_diag_ore++;
        }
    }

    for (n = 0u; n < 8u; n++)
    {
        uint8_t ch;

        if ((USART1->SR & USART_SR_RXNE) == 0u)
        {
            break;
        }

        ch = (uint8_t)(USART1->DR & 0xFFu);

        if ((ch == '?') || (ch == 'h'))
        {
            printf("\r\n[boot] replay %u B (ovf=%u) ...\r\n",
                   (unsigned)usart_retarget_bootlog_len(),
                   (unsigned)usart_retarget_bootlog_overflow());
            usart_retarget_bootlog_replay();
            printf("[boot] end\r\n");
        }
        else if (ch == 's')
        {
            printf("STAT pd=%u ps=%u ctx=%u txd=%u txt=%u ld=%u lp=%u boot=%u/%u ore=%u\r\n",
                   (unsigned)usart_retarget_print_drops(), (unsigned)usart_retarget_print_splits(),
                   (unsigned)usart_retarget_print_ctx_full(), (unsigned)usart_retarget_tx_dead(),
                   (unsigned)usart_retarget_tx_timeouts(), (unsigned)app_log_dropped(),
                   (unsigned)app_log_pending(), (unsigned)usart_retarget_bootlog_len(),
                   (unsigned)usart_retarget_bootlog_overflow(), (unsigned)s_diag_ore);
            app_protect_wdg_print();                 /* ★ 狗窗现状（只读，见其函数注） */
        }
        else
        {
            /* ⛔ 未识别的字符：静默丢弃（不写 shell、不回显） */
        }
    }
}

void app_main(void)
{
    delay_init(72);
    led_init();
    alarm_init();                                       /* 蜂鸣器 + KEY0 */

    printf("\r\n");
    printf("==================================================\r\n");
    printf("  BMS : 4S LFP monitor + protect");
#if APP_ENABLE_NET
    printf(" -> ESP8266 -> OneNET");
#endif
    printf("\r\n");
    printf("  MCU  : STM32F103ZET6 @72MHz (STM32CubeMX + HAL)\r\n");
    printf("  NET  : %s\r\n", APP_ENABLE_NET ? "enabled" : "disabled");
    printf("  COMFORT: temp %d.%d-%d.%d C, humi %u.%u-%u.%u %%, lux %u-%u cd/m2 (yellow only)\r\n",
           APP_ENV_COMFORT_TEMP_MIN_X10 / 10, APP_ENV_COMFORT_TEMP_MIN_X10 % 10,
           APP_ENV_COMFORT_TEMP_MAX_X10 / 10, APP_ENV_COMFORT_TEMP_MAX_X10 % 10,
           (unsigned)(APP_ENV_COMFORT_HUMI_MIN_X10 / 10), (unsigned)(APP_ENV_COMFORT_HUMI_MIN_X10 % 10),
           (unsigned)(APP_ENV_COMFORT_HUMI_MAX_X10 / 10), (unsigned)(APP_ENV_COMFORT_HUMI_MAX_X10 % 10),
           (unsigned)APP_ENV_COMFORT_LUX_MIN_CDM2, (unsigned)APP_ENV_COMFORT_LUX_MAX_CDM2);
    printf("  ALARM  : temp %d.%d-%d.%d C, humi %u.%u-%u.%u %%, lux max %u cd/m2 (beep+flash)\r\n",
           APP_ENV_TEMP_MIN_X10 / 10, APP_ENV_TEMP_MIN_X10 % 10,
           APP_ENV_TEMP_MAX_X10 / 10, APP_ENV_TEMP_MAX_X10 % 10,
           (unsigned)(APP_ENV_HUMI_MIN_X10 / 10), (unsigned)(APP_ENV_HUMI_MIN_X10 % 10),
           (unsigned)(APP_ENV_HUMI_MAX_X10 / 10), (unsigned)(APP_ENV_HUMI_MAX_X10 % 10),
           (unsigned)APP_ENV_LUX_MAX_CDM2);
    printf("  > UI   : 3 pages (overview/cells/events), see app_ui.c");
    printf("\r\n");
    printf("  > KEY0 = mute / hold %us to unlock latched fault",
           (unsigned)(APP_CTR_LATCH_HOLD_MS / 1000u));
    printf("   KEY1 = next page\r\n");
    printf("==================================================\r\n");

    /* ---------------- LCD 初始化 ---------------- */
    /* ---------------- LCD：屏幕交给 app_ui.c，本文件只负责把屏初始化好 ---------------- */
    lcd_init();
    lcd_display_dir(1);

    app_data_init();      /* 两份缓冲全置"从未采集"，界面据此显示 "no data yet" */
    app_ui_init();        /* ⚠️ 这里一个像素都不画：只做按键状态机 + `ui_layout()` 字号选型，
                           *   首帧由 `app_ui_task()` 的 `s_full_redraw` 触发 ⇒ 需要调度器。
                           *   原注释"第一页在这里画出来"是 D5 改造后的假注释，已更正
                           *   （`附录C` C-107④：白屏排查时它就是"排除法"的错误前提）。 */

    printf("LCD  : id=%x  %ux%u\r\n", lcddev.id, lcddev.width, lcddev.height);

#if APP_ENABLE_NET
    g_net_state   = NET_ST_START;
    g_transparent = 0;
#endif

    /* ---------------- 传感总线上电初始化（PB6/PB7：SHT30 + 24C02 + D10 的 INA226） ----------------
     * ★ 两条 I2C 总线**各由自己的属主初始化一次**：AFE 那条在 `User/main.c` 的 `afe_init()` 里
     *   （`g_bus_afe` = PD11/PD12），这里只碰 `g_bus_sens`。这就是 C-39 的收口形态：
     *   **实现一份、实例两个、账各记各的** —— SHT30/24C02 的偶发 NACK 抬的是 `g_bus_sens.fail`，
     *   永远不会把 `afe_link_ok()` 推过 `APP_AFE_FAIL_MAX`（否则就是"无意义跳闸"，S6 的软件半边）。
     * ⛔ 不得在探测/采样路径里重复调用：那会把同线上其他从机的事务切一半，还会清掉失败计数。 */
    softi2c_init(&g_bus_sens);

    /* ---------------- 24C02 共存自测（D3 判据"同一总线上 24C02 仍可读写"） ----------------
     * 只在启动跑一次：探测 → 写 0xA5 → 回读比对。它同时是"总线上挂第二个从机之后
     * SHT30 还能不能工作"的反向证明 —— 两步之间有完整的 STOP/START 交替。 */
    {
        uint8_t e2 = at24cxx_selftest();
        self_rec(APP_SELF_AT24, e2);

        if (e2 == AT24C_OK)
        {
            printf("EEPROM: AT24C02 @0x50 ok (write/verify on sens bus)\r\n");
        }
        else
        {
            printf("EEPROM: AT24C02 fail=%u (1=NACK:check A0/A1/A2 & PB6/PB7)\r\n", e2);
        }
    }

    /* ---------------- INA226 上电自检（D10 第 ④ 条：把"工程里还没有这个文件"补上） ----------------
     * ★ 它同时是 §2.2 A3 那条"三从机共总线验证"的**代码半**：
     *   `EEPROM:` 那行证明 24C02 在、`TH   :` 那行证明 SHT30 在、本行证明 INA226 在，
     *   三行都出现 = 0x40 / 0x44 / 0x50 三颗器件在同一条 `g_bus_sens` 上各自应答。
     * ⚠️ **两条边界必须跟着打印走，否则这一行会被读成"电流通道已经验收"**：
     *   ① 本步**只证明"寄存器写得进、读得回、两条形态自洽"**，⛔ 不证明电流方向符号
     *      与量值（那要台架与万用表，D10 判据①②）；
     *   ② 它**不接进采集通路** ⇒ `curr_10ma` / `pack_10mv_aux` / `tick_ms_curr` 此刻
     *      仍由假数据源填（逐通道交接按 C-97 F3 与实物到位的同一天做）。 */
    {
        uint8_t  ei = ina226_selftest();
        self_rec(APP_SELF_INA226, ei);
        uint16_t mfg = 0x0000u;
        uint16_t die = 0x0000u;
        int16_t  i_sh = 0;
        int16_t  i_cal = 0;
        uint16_t vb = 0;

        if (ei == INA226_OK)
        {
            (void)ina226_id_read(&mfg, &die);
            ina226_last_read(&i_sh, &i_cal, &vb);

            /* 两个电流都是 10mA 单位 ⇒ 打印成 A（`x/100` 与 `|x|%100`，⛔ 不用浮点）。
             * ★ 括号里那个数**不是装饰**：它是本驱动的第二形态（电流寄存器 / 依赖 CAL）。
             *   两个数一致 ⇒ "CAL 与分流器电压两条通路自洽"这条判据当场成立；
             *   不一致 ⇒ 用 20mA 容差判 `ERR_XCHECK`（见 `ina226.h` 的返回码表）。
             * ⚠️ MFG/DIE 只作打印**参照**：它们的期望值本轮没有取到手册原文，
             *   所以不参与任何判据，D10 首次上机以实读值为基线（见 `ina226.h` 文件头）。 */
            printf("INA226: @0x40 ok  mfg=%04X die=%04X  I=%d.%02dA(cal %d.%02dA) V=%u.%02uV  xchk_fail=%u\r\n",
                   (unsigned int)mfg, (unsigned int)die,
                   (int)(i_sh / 100), (int)((i_sh < 0) ? (-i_sh) % 100 : i_sh % 100),
                   (int)(i_cal / 100), (int)((i_cal < 0) ? (-i_cal) % 100 : i_cal % 100),
                   (unsigned int)(vb / 100u), (unsigned int)(vb % 100u),
                   (unsigned int)ina226_xcheck_fail_count());
        }
        else
        {
            /* ⚠️ 读数 1~6 的含义见 `ina226.h`；其中 3 = **没拿到总线锁**（事务没发生），
             *   ⛔ 与 1（器件不在位）不是同一件事，别照着"检查接线"去查。 */
            printf("INA226: fail=%u (1=NACK:check A0/A1@0x40 & PB6/PB7; 3=bus lock)\r\n", ei);
        }
    }

    /* ---------------- 温湿度传感器自动探测 ---------------- */
    g_th_type = th_detect();

    if (g_th_type == TH_NONE)
    {
        printf("TH   : no sensor found (SHT30@PB6/PB7 or DHT11@PG11)\r\n");
    }
    else
    {
        printf("TH   : %s detected\r\n", th_name());
    }

    if (g_th_type != TH_DHT11)
    {
        HAL_GPIO_WritePin(DHT11_DQ_GPIO_Port, DHT11_DQ_Pin, GPIO_PIN_SET);
    }

    /* DHT11 手册要求采样间隔 ≥1s, 单独给它一个慢节拍; SHT30 可以跟着刷屏周期走 */
    g_th_period = (g_th_type == TH_DHT11) ? APP_DHT11_PERIOD_MS : APP_SAMPLE_PERIOD_MS;
    g_th_ok = 0;
    g_th_fail = 0;

    /* ---------------- 采集层（★ SHT30 的唯一读者，§4.2） ----------------
     * 遗留报警通路（上面那套 th_* ）从这里取值，⛔ 不得两边各自驱动器件。 */
    app_acq_init();

    /* ---------------- ★ 门限参数区（§8「门限参数区协议」，`附录C` C-121） ----------------
     * ★ 位置 = **采集之后、日志之前**：它只用 `stmflash_read()` 读两页页头，
     *   ⛔ **一次 Flash 写都没有**（首次上电只置待提交标志，真正的擦写在 `TaskLog` 里）。
     * ⚠️ 三态里的③（被写过但两区都坏）**必须进 FAULT_LATCHED**（§5.2）——
     *   那是"拿一套不知来源的门限去切主回路"，⛔ 不得用默认值裸跑。 */
    {
        uint8_t  cfg_boot = app_cfg_load();
        uint8_t  cfg_self = app_cfg_selftest();
        self_rec(APP_SELF_CFG, cfg_self);

        printf("CFG  : boot=%u (0=fresh 1=loaded 2=LATCHED) selftest=%u %s\r\n",
               (unsigned)cfg_boot, (unsigned)cfg_self,
               (cfg_boot == APP_CFG_BOOT_LATCHED) ? "!! param area untrusted -> latch requested" : "");

        if (cfg_boot == APP_CFG_BOOT_LATCHED)
        {
            app_protect_request_latch();        /* ★ 只置标志；处置在 app_protect_step() 里（单写者） */
        }
    }

    /* ---------------- ★★ BMS 三级告警判据：**离线注入自检**（§5.3 / 附录C C-123） ----------------
     * ★ 为什么现在只跑自检、**不**把实时判定接进 TaskProtect：
     *   当前采集层喂的仍是 D2 的**假数据源**（真值要等 AFE/INA226/NTC 接上）—— 一旦假值越限，
     *   实时判定就会置 `request_latch`/`request_open`，**等于让板子被假数据真锁定**。
     *   而自检把合成输入灌进**局部** `bms_alarm_t`，⛔ 不碰真报警帧 ⇒ **对在板行为零影响**，
     *   却能对**这份编译产物**给出"等级/动作位/锁定/原因串"的逐条判决。
     * ⛔ 它的结论**只覆盖判据逻辑**：不覆盖真实采样、不覆盖执行器动作、不替代 F2 的台架故障注入。 */
    app_alarm_init();
    { uint16_t _sr = app_alarm_selftest(); self_rec(APP_SELF_ALARM, (uint8_t)_sr);
      printf("ALM  : selftest=%u (0=pass; N=case N failed)\r\n", (unsigned)_sr); }

    /* ---------------- ★ 派生层：离线注入自检（§4.4 / PRD §2.2） ----------------
     * ⚠️ SOC 在本版**没有初值来源**（OCV 表与满充/放空判据要 D16 标定）⇒ 未锚定期间
     *    `soc_x10` 一律回 `APP_SOC_UNKNOWN`（0xFFFF），⛔ 不冒充一个数值。 */
    app_soc_init();
    { uint16_t _sr = app_soc_selftest(); self_rec(APP_SELF_SOC, (uint8_t)_sr);
      printf("SOC  : selftest=%u (0=pass; N=case N failed)\r\n", (unsigned)_sr); }
    printf("SOC  : ocv-selftest=%u (0=pass; 7 cases: exact/interp/no-extrapolation)\r\n", (unsigned)app_soc_ocv_selftest());

    /* ---------------- ★ 标定表区（§8「标定表区协议」／ 附录C C-127） ----------------
     * ⚠️ **缺失 ≠ 故障**：两份副本都不可用时 valid = 0 ⇒ 系统走芯片全局 gain/offset，
     *   ⛔ **不拒绝启动**（口径见 §8 协议 (3)）。本版预期 valid = 0（填表的是 D16 的标定流程）。 */
    app_calib_init();
    printf("CAL  : valid=%u selftest=%u\r\n",
           (unsigned)app_calib_valid(), (unsigned)app_calib_selftest());

    /* ---------------- ★ D11 前置：ADC 与 NTC（§3.1.1 / `附录C` C-130 的代码前置） ----------------
     * ⚠️ 本轮**只做到"驱动就绪 + 离线自检"：尚未接管道**（NTC 读数进快照的交接、
     *   以及 §4.4 裁决的 ①~⑤ 五处，将在下一轮**一次改齐**）。
     * ★ NTC 自检是**纯查表**（不需接 NTC），而它的第 1 条正是**分压拓扑的判决点**（code 2047 ⇒ 25℃）。 */
    adc_init();
    printf("ADC  : selftest=%u/%u (0=pass)\r\n",
           (unsigned)adc_selftest(), (unsigned)adc_selftest_result());
    printf("NTC  : selftest=%u/%u (0=pass; 1=topology/table)\r\n",
           (unsigned)ntc_selftest(), (unsigned)ntc_selftest_result());
    printf("PROT : selftest=%u (0=pass; D8 hw-threshold encode round-trip)\r\n", (unsigned)afe_prot_selftest());
    /* ★★ D8：AFE **在位时**把参数区的硬件门限下发并回读校验（不在位 ⇒ NACK，一个寄存器都不写）。
     *   手册 §8.3 明写建议在上电序列里 reload；★ 结果码可读（`afe_prot_apply_result()`）。 */
    {
        uint8_t bad = 0u;

        printf("PROT : apply=%u bad=0x%02X (0=ok; 1~3=precond; 4/5=bus; 6=readback)\r\n",
               (unsigned)afe_prot_apply(&bad), (unsigned)bad);
    }

    /* ★★ D16 前置：把标定表区里**有效的**逐串 k/b 喂给 AFE 层（§8 (5)：BSP ⛔ 不读 Flash，由应用层喂）。
     *   ⚠️ 表无效（本版预期如此）⇒ 一个都不喂 ⇒ 走全局系数 —— **零行为变化**。 */
    printf("CALB : selftest=%u mask=0x%04X (0=pass; mask = cells fed)\r\n",
           (unsigned)afe_cell_calib_selftest(), (unsigned)afe_cell_calib_mask());

    if (app_calib_valid() != 0u)
    {
        uint8_t ci;
        uint8_t fed = 0u;

        for (ci = 0u; ci < (uint8_t)APP_CELL_COUNT; ci++)
        {
            uint32_t ck = 0u;
            int32_t  cb = 0;

            if ((app_calib_get_cell(ci, &ck, &cb) != 0u) &&
                (afe_set_cell_calib(ci, ck, cb) == AFE_OK))
            {
                fed++;
            }
        }

        printf("CALB : fed=%u (\u4ece\u6807\u5b9a\u8868\u533a\u5582\u5165\u9010\u4e32 k/b)\r\n", (unsigned)fed);
    }

    /* ★★ 2026-09-29：**让换算路径不能被常量折叠掉** —— 这一行用**运行期取值**（ADC）做入参。
     *   为什么非要这么做：自检全是常数参数 ⇒ 编译器把 `ntc_code_to_temp_x10()` 与整张表**全删**（map：
     *   `Removing ntc.o(.rodata.s_tab), (512 bytes)`）⇒ 于是"编进镜像的表到底对不对"根本无从取证。加上这一行，表必定入镜像 ⇒ 可 dump、可逐项对。
     *   ⚠️ 读出来的温度**不是测量结果**（今天 NTC 连线都不在），它只是"转换链路能跑"的证据。 */
    {
        int16_t  t_probe = 0;
        uint16_t c_probe = adc_read_ch(ADC_CH_NTC1);
        uint8_t  r_probe = ntc_code_to_temp_x10(c_probe, &t_probe);

        printf("NTC  probe: code=%u rc=%u t=%d (NOT a measurement)\r\n",
               (unsigned)c_probe, (unsigned)r_probe, (int)t_probe);
    }

#if APP_ENABLE_LOG
    /* ---------------- 事件日志: RTC 时间戳 + 内部 FLASH 环形追加 ---------------- */
    {
        uint8_t rtc_ret = rtc_init();

        app_log_init();                     /* 纯读扫描全区, 定位写指针 / 统计有效条数 / 报出损坏条数 */

    /* ★★★ 2026-09-30（还 `附录C` C-101 那笔「没有主人的导出函数」）：
     *   给 `app_log_dump()` 一个**串口触发的主人** —— 上电后 `APP_LOGDUMP_WINDOW_MS` 内收到**任意一个字节**就整份导出。
     *   ★ 为什么放在这里、窗口为什么 250ms：IWDG 窗口是 424ms（启动行 `rlr=4095 -> 424ms`）⇒ 本段必须**显著短于它**；
     *     且本段**不做任何喂狗**（全工程只有一处条件喂狗，⛔ 不在这里开第二个口子）。
     *   ★ 为什么不写完整 shell：D9 的判据是「AFE 接口定稿 ＋ 连续 1h 无通信失败」，⛔ 不是 shell。
     *   ★ 用法：打开串口(115200) → **复位板子** → 窗口内按一下回车 ⇒ 打印整份日志。
     *   ⛔ 只读：不写日志、不写参数、不改运行态；窗口外收到的字节一律忽略。 */
    if (APP_LOGDUMP_ON_BOOT != 0u)
    {
        uint32_t _t0 = HAL_GetTick();
        uint8_t  _hit = 0u;

        if ((USART1->SR & USART_SR_RXNE) != 0u)   /* 先丢掉可能的悬留字节 */
        {
            (void)USART1->DR;
        }

        while ((uint32_t)(HAL_GetTick() - _t0) < APP_LOGDUMP_WINDOW_MS)
        {
            if ((USART1->SR & USART_SR_RXNE) != 0u)
            {
                (void)USART1->DR;
                _hit = 1u;
                break;
            }
        }

        if (_hit != 0u)
        {
            printf("\r\n[log] dump by serial request ...\r\n");
            app_log_dump();
            printf("[log] dump done\r\n");
        }
    }

        /* ★ 参数区坏也要**留痕**（⛔ 不得只在串口上喊一句：本机没有 COM 通路的教训见验收轮） */
        {
            uint8_t  bs = 0u;
            uint8_t  bm = 0u;
            uint32_t sq = 0u;

            app_cfg_status(&bs, &sq, &bm);

            if (bs == APP_CFG_BOOT_LATCHED)
            {
                (void)app_log_add(EV_CFG_CORRUPT, (uint16_t)((uint16_t)bm | 0x8000u), 0u, 0u);
            }
        }
        app_log_boot_event();               /* ★ 第一条永远是"这次为什么复位" */

        /* ★ C-110：把上一次"卡死 → 咬狗"的现场从备份域读回来补投一条 `EV_TASK_STALLED`。
         *   位置是钉死的：必须在 `app_log_boot_event()` 之后（日志读成 复位原因 → 开机 → 谁卡的）、
         *   在调度器之前（那时还没有并发）、在任何动备份域的动作之前。
         *   出参只为了打印这一行 —— 携带通路的"第二形态"（§2.3 h) 那条债的同族）。 */
        {
            uint8_t  cm;
            uint16_t ca;
            uint16_t cd;

            if (app_protect_stall_carry_report(&cm, &ca, &cd) != 0u)
            {
                printf("STALL: carried from before reset  mask=0x%02X age=%ums dl=%ums\r\n",
                       cm, (unsigned int)ca, (unsigned int)cd);
            }
        }

        /* 报警一开始就通过这个回调把快照存进去(app_alarm 与 app_log 之间没有直接依赖) */
        alarm_set_log_hook(app_log_env_event);

        printf("RTC  : %s  now %04u-%02u-%02u %02u:%02u:%02u  (clock %s)\r\n",
               (rtc_ret == 0) ? "ok" : "INIT FAILED",
               calendar.year, calendar.month, calendar.date,
               calendar.hour, calendar.min, calendar.sec,
               (rtc_is_set() != 0) ? "set, battery-backed" : "from build time, not battery-backed");

        printf("LOG  : KEY1 -> page 3 for the event log (newest first); full dump waits for the D9 serial command\r\n");
    }
#endif

    lsens_init();

    s_t_sample = HAL_GetTick();
    s_t_th = s_t_sample;
#if APP_ENABLE_NET
    s_t_report = s_t_sample;
    s_t_ping   = s_t_sample;

    esp8266_set_poll_hook(app_poll);        /* 联网等待期间继续跑: 报警 / 消音 / 状态行 / 补心跳 */
#endif

    /* ---------------- ★ D5（§8.4 M3）：从这里开始"把机器交给调度器" ----------------
     * 顺序是钉死的，换任何一步都会得到一类难查的故障：
     *   ① `app_bus_init()` —— 传感总线互斥量必须在**任何任务能跑起来之前**建好。否则第一个
     *      事务看到的是"钩子已注册、句柄还是 NULL"，`app_bus.c` 的判空会把它当成
     *      "单线程期放行" —— 于是锁**静默不存在**（本项目最坏的一类缺陷：不报错、只是没兜住）。
     *   ② `app_protect_wdg_start()` —— 狗要在调度器之前有效：连启动过程卡住都要被咬。
     *   ②' `app_protect_actuator_start()` —— 三路使能钉在断开 + 引脚模式自检（D6，见下面那一段）。
     *   ③ `app_tasks_create()` → ④ `vTaskStartScheduler()`。
     * ⛔ 这两步之前不得出现任何"只有任务上下文才能安全调用"的东西（例如等信号量）。
     * ★★ 又收一层（C-107）：**从这里往上的一切都不许依赖 `HAL_GetTick()` 计时**。
     *   上面 `app_log_boot_event()` 那一对内核临界区在调度器前不会把 `BASEPRI` 降回 0
     *   （`port.c:135` 的哨兵值 → 要等 `vPortSVCHandler` 才清），SysTick 优先级 15 被屏蔽
     *   ⇒ tick 冻住 ⇒ "按 tick 计数的有界等待"= 无限循环；而狗要到 ② 里面才启动，
     *   所以后果不是复位而是**永不停、永不打印**（现象 = 屏停在 `lcd_init()` 的白）。
     *   要在这段里等，就用不依赖中断的轮询式延时（`delay_ms()` 走 `SysTick->VAL`）。 */
    if (app_bus_init() != 0u)
    {
        printf("FATAL: sens bus mutex create failed\r\n");    /* 静态内存参数写错 = 代码 bug */
    }

    app_protect_wdg_start();

    /* ★ D6 软件侧前置（2026-09-28）：执行器层紧接着狗启动。
     *   顺序 = ②' 在 ② 之后、③ 之前，两条理由各自独立成立：
     *     · 狗要先活着，才能咬住"配执行器 GPIO 时卡住"这一段（它内部有有界的回读校验）；
     *     · 执行器要在**任何任务能跑起来之前**已经钉在断开态，否则调度器里先动它的可能是别人。
     *   ⚠️ 外部 10K 下拉（S5）本来就是"MCU 死了也断着"的保证，本步不产生安全性、
     *   只把"引脚现在真的是低"变成一条**被观测过**的事实（C-106 的同一种修法：回读出口）。 */
    app_protect_actuator_start();

    /* ---------------- ★ Modbus RTU 从机（板载 RS485：USART2 + PD7 方向脚） ----------------
     * ★ 位置是钉死的：**在 `app_nvic_init()` 之后**（NVIC 分组/优先级已在 `main.c` 收口）、
     *   在 `app_protect_wdg_start()` 之后（自检里有有界等待，卡住必须被狗咬而不是静默）、
     *   在 `vTaskStartScheduler()` 之前（§4.3 的启动序列）。
     * ⚠️ **两条边界必须跟着这一行打印走**（`app_modbus.h` 文件头）：
     *   ① 本轮是把 D9 的从机提前交付，"能应答"这一半到位 ⇒ **⛔ 不等于 `附录C` C-100 的
     *      M0 已完成**（"应答方是谁"要等 RS485 主机与第三方工具接上才能证）；
     *   ② 从机地址/波特率的写**只存在于 RAM** ⇒ 掉电回默认（参数区还没建，登记为债务）。 */
    {
        uint8_t e_init = app_modbus_init(APP_MB_BAUD_DEFAULT);
        uint8_t e_dma  = 0xFFu;
        uint8_t e_pro  = 0xFFu;

        if (e_init == 0u)
        {
            /* ① 物理层真机判据：TX 的 DMA 请求映射到底通不通（挂错通道时它超时）。 */
            e_dma = rs485_selftest();
            self_rec(APP_SELF_RS485, e_dma);
            /* ② 协议级判据：CRC16 已知答案 + 一条合成请求的响应结构（不需要任何外部设备）。 */
            e_pro = app_modbus_selftest();
            self_rec(APP_SELF_MODBUS, e_pro);
        }

        printf("485  : USART2 %u 8N1 addr=%u  init=%u dma=%u crc=%u %s\r\n",
               (unsigned int)APP_MB_BAUD_DEFAULT,
               (unsigned int)APP_MB_ADDR_DEFAULT,
               (unsigned int)e_init, (unsigned int)e_dma, (unsigned int)e_pro,
               ((e_init == 0u) && (e_dma == 0u) && (e_pro == 0u)) ? "ok" : "FAIL");
    }

    /* ★ 按键的两个出口在这里接线（`Build.md` §2 D5 判据⑥b）：
     *   长按 5s → `app_protect`（S11 锁定的唯一解除入口，⛔ 界面不得自己清 request_latch）；
     *   短按   → 遗留环境蜂鸣器消音（见 `app_env_mute()` 上那条"D2 起断了"的说明）。 */
    app_ui_set_unlock_hook(app_protect_request_unlock);
    app_ui_set_ack_hook(app_env_mute);

    if (app_tasks_create() != 0u)
    {
        printf("FATAL: task create failed\r\n");
    }

    vTaskStartScheduler();

    /* 走到这里只有两种可能：任务创建失败（上面已打印），或静态栈写小导致内核拒绝启动。
     * ⛔ 不得退回"那我自己 while(1) 跑着" —— 那会让"调度器根本没起来"变成一种可以长期
     *   静默存在的状态（屏幕照画、串口照打，只有看门狗知道真相）。
     *   关中断停住，让狗在 ≤2s 内咬；现象 = 反复重启，一眼可辨。 */
    taskDISABLE_INTERRUPTS();

    for (;;)
    {
    }
}

/**
 * @brief       迁移容器的服务切片 = **原来 `app_main()` 里 `while(1)` 的循环体**
 * @note        ★ 与 §8.4 M2 原计划的偏差（理由留在这里，否则下一个人一定会问）：
 *              计划写的是"把主循环整段搬进 `app_tasks.c` 的容器"。实际改成**容器留在
 *              `app_tasks.c`、业务切片留在本文件由容器调用**，因为搬 150 行业务要连带
 *              把 20 个 static 函数与 12 个变量跨文件搬家 —— 风险最高的一天最不该做的
 *              就是大搬家。M2 的判据由"app_main.c 只剩初始化"改口径为
 *              **"app_main.c 不再有 while(1)"**（成立）；容器"只有 vTaskDelay、无 delay_ms"
 *              也成立（延时在容器里）。
 * @note        ⏳ 本函数连同 `APP_PERIOD_LEGACY_MS` 与 `task_legacy()` 在 D15 清空时一起删除。
 * @note        ⛔ 不得改动这里的逻辑顺序；要交接某个通道就往 `app_tasks.c` 的对应任务里搬。
 */

void app_main_service(void)
{
    uint32_t now;                           /* 本轮时刻（HAL_GetTick 口径，与写者同钟）*/

    now = HAL_GetTick();

    /* ========== 20ms 任务: 按键 + 报警(含 KEY0 消音) + 报警闪烁 + 联网状态机 ========== */
    app_periodic();                         /* 按键 + 采集 + 界面 + 报警，见 `app_periodic()` */
    /* ★ D5 §8.4 M4：这里原来的 `IWDG_FEED()` 已删 —— 全工程唯一喂狗点在
     *   `app_protect.c` 的**条件**喂狗（判活口径见 `app_tasks.c` 的 C-34 注释）。 */

#if APP_ENABLE_NET
    net_step();

    if (g_net_state == NET_ST_ONLINE)
    {
        /* ★ 上线后必须持续排空接收缓冲(P0)。
         *   不排空的后果: 768 字节环形缓冲只涨不落, 满了以后模块发来的
         *   CLOSED / PINGRESP 全被静默丢弃 —— 云端断了、模块掉线了,
         *   本地却一直显示 "Cloud : online" 并且 Sent 还在涨。
         *   这种"静默失效"对监护老人的设备是最危险的故障, 所以每轮都排空。 */
        mqtt_poll_rx();

        if (mqtt_link_closed())
        {
            mqtt_link_clear_closed();
            printf("ESP8266: TCP closed (peer or WiFi down)\r\n");
            g_transparent = 0;
            net_fail("Cloud : link lost");

            /* ★ D5 切片：原来这里是"延时 20ms 后 continue 下一轮"。
             *   容器每 20ms 回来一次 ⇒ 延时由 `vTaskDelay` 提供，本行只剩"本轮到此为止"
             *   的语义，故改 `return`。⛔ 不得在这里保留任何定点死延时（S12，
             *   且 G7-③ 对 `app_main.c` 的排除项本轮已删）。 */
            return;
        }
    }
#endif

    /* ========== 采样周期任务: 采集 + 显示 + 上报 ========== */
    if ((now - s_t_sample) >= APP_SAMPLE_PERIOD_MS)
    {
        s_t_sample = now;

        /* 1. 采集温湿度: 按 g_th_period 的节拍走(DHT11 需 ≥1s, 不做连发重试) */
        if (g_th_type == TH_NONE)
        {
            /* 没探到传感器: 每隔 2s 重新探测一次, 不永久锁死 */
            if ((now - s_t_th) >= APP_DHT11_PERIOD_MS)
            {
                s_t_th = now;
                g_th_type = th_detect();
                g_th_period = (g_th_type == TH_DHT11) ? APP_DHT11_PERIOD_MS : APP_SAMPLE_PERIOD_MS;
            }
        }
        else if ((now - s_t_th) >= g_th_period)
        {
            s_t_th = now;

            s_r_th = th_read(&s_t_x10, &s_h_x10);

            if (s_r_th == TH_RD_OK)
            {
                g_th_ok = 1;
                g_th_fail = 0;
            }
            else
            {
                g_th_ok = 0;

                /* 连续失败 → 重新探测(实验29 用 while(dht11_init()) 就是这个意思) */
                if (++g_th_fail >= APP_TH_RETRY)
                {
                    g_th_fail = 0;
                    g_th_type = th_detect();
                    g_th_period = (g_th_type == TH_DHT11) ? APP_DHT11_PERIOD_MS : APP_SAMPLE_PERIOD_MS;
                }
            }
        }

        s_th_ok = g_th_ok;

        /* 2. 采集光照, 并把 0~100 的相对值换算成 cd/m2 估计值 */
        s_lux = lsens_get_val();
        s_lux_cdm2 = (uint32_t)s_lux * (uint32_t)APP_ENV_LUX_FULL_SCALE_CDM2 / 100U;

        /* 3. 喂给报警模块(持续确认窗 + 迟滞, 触发阈值见 app_config.h) */
        alarm_feed(s_t_x10, s_h_x10, s_lux_cdm2, s_th_ok);

        /* 4. 环境量的状态出口 = 串口（LCD 已归 BMS 三页界面，见文件头注 4） */
        if (s_th_ok != 0)
        {
            printf("DATA %s temp=%d.%d humi=%u.%u lsens=%u cdm2=%u alarm=%s%s\r\n",
                   th_name(),
                   (int)(s_t_x10 / 10), (int)(s_t_x10 % 10),
                   (unsigned)(s_h_x10 / 10), (unsigned)(s_h_x10 % 10),
                   (unsigned)s_lux, (unsigned)s_lux_cdm2, alarm_reason(),
                   alarm_is_beeping() ? " BEEP" : "");
        }
        else
        {
            printf("DATA %s read fail (consecutive #%u) lsens=%u cdm2=%u alarm=%s%s\r\n",
                   th_name(), (unsigned)g_th_fail, (unsigned)s_lux, (unsigned)s_lux_cdm2,
                   alarm_reason(), alarm_is_beeping() ? " BEEP" : "");
        }

#if APP_ENABLE_NET
        /* 5. 周期上报 OneNET */
        if (g_net_state == NET_ST_ONLINE)
        {
            if ((now - s_t_report) >= APP_REPORT_PERIOD_MS)
            {
                s_t_report = now;
                s_msg_id++;

                if (onenet_publish(s_msg_id, s_t_x10, s_h_x10, s_lux) != MQTT_STAT_OK)
                {
                    printf("publish FAIL, will reconnect\r\n");
                    g_transparent = 0;
                    net_fail("Cloud : send FAIL");
                }
                else
                {
                    s_sent++;
                    printf("MQTT: published, total %u\r\n", (unsigned)s_sent);
                }
            }

            /* 6. MQTT 心跳保活 + 链路活性判定(全工程唯一能发现"静默断线"的地方) */
            if ((now - s_t_ping) >= APP_PING_PERIOD_MS)
            {
                s_t_ping = now;

                if (mqtt_ping() == MQTT_STAT_OK)
                {
                    g_ping_fail = 0;
                }
                else if (++g_ping_fail >= APP_PING_FAIL_MAX)
                {
                    g_ping_fail = 0;
                    printf("MQTT: link lost (%u pings no answer), reconnect\r\n",
                           (unsigned)APP_PING_FAIL_MAX);
                    g_transparent = 0;              /* TCP 多半已断, 不必再发 +++ */
                    net_fail("Cloud : link lost");
                }
                else
                {
                    printf("MQTT: ping no answer (%u/%u)\r\n",
                           (unsigned)g_ping_fail, (unsigned)APP_PING_FAIL_MAX);
                }
            }
        }
#endif /* APP_ENABLE_NET */

        LED1_TOGGLE();
    }
}
