/**
 ****************************************************************************************************
 * @file        app_config.h
 * @brief       工程配置: 传感器选择 + 界面字号 + 功能开关 + 报警阈值 + OneNET 非敏感参数
 * @note        ★ 平时只需要改这一个文件 ★ —— **例外**: WiFi 密码与 OneNET 凭据在
 *              app_config_local.h(不交付, 见第 4 节), 换热点时改那个文件。
 ****************************************************************************************************
 */

#ifndef __APP_CONFIG_H
#define __APP_CONFIG_H

/* ============================ 1. 温湿度传感器选择 ============================ */
/* 0 = 只用 DHT11  (PG11 单总线, 板载 U4 座自带 4.7K 上拉)
 * 1 = 只用 SHT30  (PB6=SCL / PB7=SDA 软件 I2C, 与板载 24C02 共总线, 地址不冲突)
 * 2 = 自动探测: 先找 SHT30, 找不到再找 DHT11 (推荐, 插哪个用哪个)
 * 串口与 LCD 上都会显示实际识别到的型号。 */
#define APP_SENSOR_TH           2

/* ============================ 2. 界面字号 ============================ */
/* 正点原子字库只支持 12 / 16 / 24 / 32 四种 ASCII 字号。
 * 这两个值是"上限", 程序会按屏幕实际高度、以及实际要显示的行数,
 * 自动向下取一档保证放得下:
 *     3.5 寸横屏 480x320, APP_ENABLE_NET=0 -> 32 / 32
 *     3.5 寸横屏 480x320, APP_ENABLE_NET=1 -> 32 / 32
 *     2.8 寸横屏 320x240, APP_ENABLE_NET=0 -> 32 / 32
 *     2.8 寸横屏 320x240, APP_ENABLE_NET=1 -> 24 / 16
 * 关掉联网时 WiFi/Cloud/Sent 三行不显示, 省下的高度全部让给字号。 */
#define APP_UI_FONT_TITLE       32
#define APP_UI_FONT_DATA        32

/* ============================ 3. 功能开关 ============================ */
/* 0 = 只用传感器 + LCD 本地显示, 完全不碰 ESP8266 和 OneNET (默认)
 * 1 = 打开 ESP8266 联网 + OneNET 上报(需要先填好下面第 4、5 节的参数)
 * 无论取值如何, 采集与 LCD 显示都照常工作, 联网失败不会卡住界面。 */
#define APP_ENABLE_NET          1

/* ============================ 4. WiFi 热点 (必须 2.4GHz) ============================ */
/* ★ 凭据不写在可见文件里(附录C · C-37 / AGENTS.md §4「无裸露 API key/token」)。
 *   真值在 User/App/app_config_local.h —— **交付或上传前删除那个文件**。
 *   它必须定义: APP_WIFI_SSID / APP_WIFI_PWD / ONENET_PID / ONENET_DEVICE / ONENET_TOKEN */
#include "app_config_local.h"

#if !defined(APP_WIFI_SSID) || !defined(APP_WIFI_PWD) || !defined(ONENET_PID) \
    || !defined(ONENET_DEVICE) || !defined(ONENET_TOKEN)
#error "app_config_local.h 缺少凭据: 必须定义 APP_WIFI_SSID / APP_WIFI_PWD / ONENET_PID / ONENET_DEVICE / ONENET_TOKEN"
#endif

/* ============================ 5. OneNET 接入参数 ============================ */
#define ONENET_HOST             "mqtts.heclouds.com"    /* 非加密接入点 */
#define ONENET_PORT             1883

/* 上报格式:
 *   0 = 数据流-数据点  主题 $sys/{pid}/{dev}/dp/post/json      (MQTT物联网套件, 不需要预先定义物模型)
 *   1 = 物模型属性    主题 $sys/{pid}/{dev}/thing/property/post (OneNET Studio, 需先建好 3 个属性)
 * 默认 0; 若串口显示 MQTT 已连上但平台看不到数据, 就改成 1 重新编译。 */
#define APP_ONENET_MODE         0

/* ============================ 6. 数据流名称 ============================ */
#define ONENET_DP_TEMP          "temp"                  /* 温度  x10 ℃ */
#define ONENET_DP_HUMI          "humi"                  /* 湿度  x10 %RH */
#define ONENET_DP_LUX           "lux"                   /* 光照  0~100 */

/* ============================ 7. 报警阈值(两级: 舒适区 / 危险区) ============================
 * ★★ 命名空间纪律（附录C · C-38，2026-09-25 定案）—— 本节全部是**环境监测**的门限，
 *   是前序工程遗留、**不属于本 BMS 的 9 项核心功能**：
 *     电池量 → 一律 APP_BMS_*（§12.7，将来 D14 的三级门限）
 *     环境量 → 一律 APP_ENV_*（✅ 2026-09-26 已按 C-38 完成改名，本节所有宏已带 ENV_）
 *     告警**输出通道**参数（时长/间隔/确认窗/闪烁）→ APP_ALARM_*，两侧共用，
 *                ⛔ 但**迟滞必须分域**：APP_ENV_HYST_* 与 APP_BMS_HYST_* 不得同宏
 *   理由：本文件现在同时承载两套门限，而 app_alarm.c 的语义是"环境两级"，
 *   D14 写"电芯三级 + 迟滞"时若沿用同名宏，**门限会跨域污染** —— 对会切断主回路的
 *   设备，这直接表现为误动作或拒动作（S7 的门限层次也随之失效）。
 *   ⛔ 在 BMS 代码（app_protect / app_acq / app_soc）里 include 或引用本节任何宏 = 违规。
 * ----------------------------------------------------------------------------
 * 这是本工程最重要的产品决策, 两级语义完全不同:
 *
 *   危险区 APP_ENV_xxx_X10 / APP_ENV_LUX_xxx_CDM2
 *       超出 -> 真正报警: 蜂鸣器响 10s + LCD 整屏闪烁 + 对应行变红。
 *       依据是医学风险点: 低温有低体温风险(独居老人冬天舍不得开暖气, 这才是最该报的),
 *       高温有中暑/脱水风险, 湿度极端值对应呼吸道不适与结露发霉。
 *
 *   舒适区 APP_ENV_COMFORT_xxx
 *       超出(但还在危险区内) -> 只把对应行和 Alarm 行变成黄色 + 显示 warnT/warnH/warnL,
 *       **不响蜂鸣器**。老人居室冬天 16~20℃、夏天空调房 26~28℃ 都属正常;
 *       把"舒适区"当报警线会让设备天天响, 老人会直接把电源拔掉, 报警功能归零。
 *
 * ★ 约束: COMFORT 必须落在危险区以内(即 DANGER_MIN <= COMFORT_MIN < COMFORT_MAX <= DANGER_MAX),
 *   程序不检查这个约束, 填反了会出现"只提示不报警"的怪现象。
 * 单位: 温度/湿度 0.1 单位; 光照 cd/m2(填 0 = 该方向不判断)。 */
#define APP_ENV_TEMP_MIN_X10            100     /* 危险: 10.0 ℃ 以下 */
#define APP_ENV_TEMP_MAX_X10            320     /* 危险: 32.0 ℃ 以上 */
#define APP_ENV_HUMI_MIN_X10            250     /* 危险: 25.0 %RH 以下 */
#define APP_ENV_HUMI_MAX_X10            850     /* 危险: 85.0 %RH 以上 */
#define APP_ENV_LUX_MIN_CDM2            0       /* 危险: 不做下限判断 */

#define APP_ENV_COMFORT_TEMP_MIN_X10    180     /* 舒适: 18.0 ℃ */
#define APP_ENV_COMFORT_TEMP_MAX_X10    280     /* 舒适: 28.0 ℃ */
#define APP_ENV_COMFORT_HUMI_MIN_X10    400     /* 舒适: 40.0 %RH */
#define APP_ENV_COMFORT_HUMI_MAX_X10    700     /* 舒适: 70.0 %RH */
#define APP_ENV_COMFORT_LUX_MIN_CDM2    100     /* 舒适: 100 cd/m2 */
#define APP_ENV_COMFORT_LUX_MAX_CDM2    4000    /* 舒适: 4000 cd/m2 == 光敏读数 20,
                                             * 正好等于旧版"2000 cd/m2 + FULL_SCALE 10000"那条
                                             * 会响的线 —— 也就是说同样的光照条件, 现在只变黄不响。 */

/* 光照量程说明(改之前务必看懂):
 * 板载 LS1 是光敏电阻, lsens_get_val() 给出的是 0~100 的"相对光强", 不是标定过的照度计。
 * FULL_SCALE 表示"光敏读数 100 相当于多少 cd/m2", 用来把 cd/m2 阈值换算到光敏读数。
 * 默认 20000(室内直射阳光量级) -> 光敏读数 1 对应 200 cd/m2。
 * 标定方法: 用手机照度计 App 对着同一光源, 同时看串口打印的 lsens= 读数,
 *           用 照度*(100/lsens) 反推 FULL_SCALE, 改这个宏即可。
 *
 * 光照的危险上限是"光敏饱和(强光直射)"。因为 alarm_eval 用的是 `>`
 * 比较, 取 FULL_SCALE-100 才能在读数打满量程的瞬间触发;
 * 写成表达式是为了标定后改 FULL_SCALE 时它能自动跟着走。 */
#define APP_ENV_LUX_FULL_SCALE_CDM2     20000
#define APP_ENV_LUX_MAX_CDM2            (APP_ENV_LUX_FULL_SCALE_CDM2 - 100)

/* 报警时序 */
#define APP_ALARM_TIME_MS       10000   /* 单次报警持续时间(蜂鸣器连续响) */
#define APP_ALARM_GAP_MS        10000   /* 两次报警之间的静默时间 */

/* 误报抑制 —— 上面两级阈值只定义"越界多少", 这里定义"什么时候才算越界/才算恢复"。
 *   误报是这类居家监护设备最大的敌人: 老人被反复吵到会直接把电源拔掉,
 *   一次误报造成的伤害比十次漏报还大。
 *
 *   CONFIRM_MS : 越界必须"连续"持续这么久才确认, 用来滤掉瞬时尖峰
 *                (光照被手电/阳光扫过、开关门、传感器抖动)。填 0 = 立即生效。
 *                危险区和舒适区共用这一个确认窗。
 *   HYST_xxx   : 迟滞量。已经在某一级上时, 恢复边界相对触发边界内收,
 *                否则数值卡在阈值上下跳动时, 会"响 10s 停 10s"无限循环,
 *                或者黄字一闪一闪。迟滞量必须小于阈值区间的一半。 */
#define APP_ALARM_CONFIRM_MS    3000    /* 连续越界确认窗(ms), 0 = 立即生效 */
#define APP_ENV_HYST_TEMP_X10 5       /* 温度迟滞 0.5℃ */
#define APP_ENV_HYST_HUMI_X10 20      /* 湿度迟滞 2.0%RH */
#define APP_ENV_HYST_LUX_CDM2 100     /* 光照迟滞 100 cd/m2 */

/* ============================ 8. 运行参数 ============================ */
#define APP_REPORT_PERIOD_MS    5000                    /* 数据上报周期(ms) */
#define APP_PING_PERIOD_MS      30000                   /* MQTT 心跳周期(ms), 须小于 MQTT_KEEPALIVE_S */
#define MQTT_KEEPALIVE_S        120                     /* MQTT keepalive(秒), 平台允许 10~1800 */
#define MQTT_PING_TIMEOUT_MS    3000                    /* 发 PINGREQ 后等 PINGRESP 的超时(ms) */
#define APP_PING_FAIL_MAX       2                       /* 连续几次心跳无应答就判定链路断了并重连 */
#define APP_TH_RETRY            3                       /* 温湿度连续失败多少次后重新探测传感器 */
#define APP_ACQ_ENV_PERIOD_MS   500                     /* 采集层环境通道(SHT30)节拍: 与刷屏同周期, 两阶段读天然错开一拍 */
#define APP_DHT11_PERIOD_MS     2000                    /* DHT11 采样间隔: 手册要求 ≥1s, 取 2s 更稳 */
#define APP_NET_RETRY_MS        15000                   /* 联网失败后的重试间隔(ms) */
#define APP_NET_FAIL_RESCAN     4                       /* 连续失败几次后允许重新全扫描波特率
                                                         * (模块自己复位过的话波特率可能变了,
                                                         *  只扫一次会永久连不上) */
#define APP_SAMPLE_PERIOD_MS    500                     /* 光照采样 + 刷屏周期(ms) */
/* ⛔ 原 `APP_ENABLE_IWDG`（"填 0 关闭看门狗"）已删除 —— 看门狗是保护层，
 *   不允许存在"交付版把狗关掉"的编译期开关（AGENTS S13）。参数见 §11.1。 */

/* ============================ 9. 报警时的整屏闪烁 ============================ */
/* 60 岁以上人群约三分之一有不同程度听力损失, 光靠蜂鸣器对一部分老人等于没有报警。
 * 打开后: 蜂鸣器响的同时, LCD 整屏在"红底白字 / 白底彩字"之间交替,
 *         既听得到也看得见; 消音或恢复正常后自动回到白底。
 * 报警开始/结束的时序、阈值、蜂鸣器行为一律不变, 只是多一层视觉提示。 */
#define APP_ALARM_FLASH         1                       /* 1 = 报警时整屏闪烁; 0 = 关闭 */
#define APP_ALARM_FLASH_MS      400                     /* 闪烁半周期(ms), 400 即每秒约 1.25 次 */

/* ============================ 10. 事件日志（D4 起 = BMS 事件日志） ============================ */
/* 1 = 每次"值得留痕"的事件（上电/复位、报警、链路变陈旧、开关动作…）落一条内部 FLASH 记录，
 *     页 3 用 KEY1 翻页查看，串口 `app_log_dump()` 整份导出;
 * 0 = 完全不碰 RTC 和 FLASH(省掉 RTC 初始化与落盘路径)。
 *
 * 硬件前提: PC14/PC15 的 32.768kHz 晶振(板上已有) + VBAT 纽扣电池座(CR1220)。
 *   装了电池 -> 掉电后时间继续走, 记录里的时间戳是真实时间;
 *   没装电池 -> 每次上电时间从**固件编译时间**开始(串口会打 [clock not battery-backed]),
 *               时间戳只能当相对参考, 但记录本身照样存得住。
 *
 * 存储位置: §13.1 —— 日志区 28 页环形**追加**, 容量 `APP_LOG_CAPACITY`(3556 条) 远超"≥300 条"要求。
 *   单条自带 CRC16；提交半字**最后编程** ⇒ 半条写坏只丢那一条, 上电扫描自动跳过并计数。
 *   ⚠️ `app_log_dump()` 目前**没有调用者**（串口命令入口排在 D9 的协议层），现在能看到的出口是
 *      页 3 与每条落盘时那一行串口打印。
 *   ⛔ 遗留环境日志(温湿度/光照 64 条)随"收回最后一页"整体切除: 它的报警通路改投事件码
 *      `EV_ENV_ALARM`, 而**光照 cd/m2 不再入档**(BMS 判据不消费它) —— 已写进差距声明。
 * 相关文件: Drivers/BSP/RTC, Drivers/BSP/STMFLASH, User/App/app_log.c */
#define APP_ENABLE_LOG          1

/* ==================== 11. FreeRTOS 任务参数（BMS 新增） ====================
 * 唯一来源：本文件（AGENTS.md §5.1"全部门限/参数集中在 app_config.h"）
 * 设计依据：Tech_Design.md §4.3  栈单位为 StackType_t(4 字节)，与 §4.3 的"字"一致
 * 栈余量：TaskProtect 384 / Acq 640 / Comm 896 / Log 512 / Ui 768 = 3200 字 ≈ 12.8KB
 *         实测用 uxTaskGetStackHighWaterMark() 复核，要求留 30% 余量（C-15 第 6 条）
 * 优先级：Protect(4) > Acq(3) > Comm(2) > Log(1) = Ui(1)  ← 保护任务必须最高
 * 周期：  TaskComm = 10ms（审查 C-04：50ms 会吃掉 Modbus 100ms 指标的一半预算）   */
#define APP_STK_IDLE            128     /* 空闲任务栈（字）；FreeRTOS 静态分配必需 */
#define APP_STK_PROTECT         384
#define APP_STK_ACQ             640
#define APP_STK_COMM            896
#define APP_STK_LOG             512
#define APP_STK_UI              768
#define APP_PRIO_PROTECT        4
#define APP_PRIO_ACQ            3
#define APP_PRIO_COMM           2
#define APP_PRIO_LOG            1
#define APP_PRIO_UI             1

#define APP_PERIOD_PROTECT_MS   10      /* 保护状态机 + 使能输出 + ALERT 响应 */
#define APP_PERIOD_ACQ_MS       100     /* 采集与滤波 */
#define APP_PERIOD_COMM_MS      10      /* Modbus 收帧/回帧（C-04） */
#define APP_PERIOD_LOG_MS       50      /* 事件落盘（队列空转周期） */
/* ★ **C-95 决议（D5 选定，方案 a）**：TaskUi 周期由 200ms 降到 **50ms**，刷屏仍按 200ms 分频
 *   （`app_ui_task()` 内部本来就自己限流重绘）。理由：人手一次短按约 80~150ms，200ms 的采样窗
 *   漏检率接近一半，而 **S11 的"长按 KEY0 5s 解除锁定"是唯一解锁出口** —— 它依赖不漏检的持续
 *   采样。⛔ 不得以"5s 很长、少采几次无所谓"维持原样（`附录C` C-95 / `Tech_Design.md` §4.3 注③）。
 *   ⚠️ 本值**今天还没有消费者**：D5 的迁移容器占用的是 `APP_TASK_UI` 身份、按
 *   `APP_PERIOD_LEGACY_MS` 跑；`task_ui()` 从容器里拆出来那天（≤D15）才按 50ms 创建。 */
#define APP_PERIOD_UI_MS        50

/* ★ **D5 迁移容器的节拍**（`Build.md` §8.4 M2 / C-40）：遗留超循环的循环体整段搬进
 *   `task_legacy()`，为了不改变任何时序，节拍沿用原来的 `delay_ms(20)` = 20ms。
 *   ⛔ 这是**临时项**：容器清空（≤D15）后本宏随之删除，`APP_PERIOD_UI_MS` 接管这一身份。 */
#define APP_PERIOD_LEGACY_MS    20

/* ★ **传感总线互斥的等待上限**（C-96 ⑥，锁只准加 `g_bus_sens` 一侧）：
 *   一次软 I²C 事务最长约 1ms 量位（SHT30 读 6 字节 @100kHz），100ms 已是三倍余量。
 *   ⛔ 不得用"无穷等"：那会把"某个任务持锁后卡死"从"丢一次采样"升级成"整机 2s 复位"，
 *   而复位现象与硬件故障同形（C-66 同族）。超时 = 本次事务放弃并计入该总线 `fail`（真失败，照实记）。 */
#define APP_BUS_LOCK_TIMEOUT_MS 100

/* ---------- 11.1 看门狗与判活窗口（★ 附录C · C-34 / C-36） ----------
 * ⛔ 原来的 `APP_ENABLE_IWDG` 开关**已删除**：看门狗是保护层，留着编译期开关
 *    就等于允许"交付版把狗关掉"（AGENTS S13 禁止条件编译掉保护）。
 *
 * ★ 判活口径换了（C-34 的教训）：**不用**"自上次检查以来每个任务都推进过"。
 *    `TaskProtect` 每 10ms 跑一次，而 `TaskUi` 周期 200ms —— 那种判据在一半
 *    以上的检查里都会把"活着的 Ui"误判成死，后果是**设备正常运行也周期复位**，
 *    而且现象和硬件故障同形。改为**逐任务 deadline**：
 *      `now - 该任务最后一次推进 > APP_WDG_ALIVE_K × 该任务自身周期` 才算死。
 *    ⇒ 与"检查调用得多频繁"无关，只与任务自己的周期有关。
 *
 * ⚠️ `APP_LSI_HZ` 是**标称** 40kHz（F103 数据手册给的范围 30~60kHz），
 *    所以 `APP_IWDG_RLR` 算出的 2s 只是理论值 —— **D5 必须实测 LSI 后回填**
 *    （测法见 `Build.md` §5 的 WDG-1）。三个窗口(判活 / 喂狗检查 / IWDG 超时)
 *    的自洽关系由 §12 末尾的编译期断言守住，写小/写大直接编译不过。 */
#define APP_WDG_ALIVE_K         4       /* 判活余量倍数：允许任务落后 K 个自身周期 */
#define APP_WDG_CHECK_MS        500     /* TaskProtect 的"判活 + 条件喂狗"周期 */
#define APP_IWDG_TIMEOUT_MS     2000    /* IWDG 超时设计值（Tech_Design.md §4.3） */
#define APP_LSI_HZ              40000   /* ⚠️ 标称值，待实测标定（30~60kHz） */
/* ★★ `IWDG->PR` 存的是**编码**，不是分频数：设备头 `stm32f103xe.h` 给 `IWDG_PR_PR_Msk = 0x7`
 *   ⇒ 只有 PR[2:0] 三位，编码 0~6 依次 = /4 /8 /16 /32 /64 /128 /256（RM0008 IWDG_PR）。
 *   本行曾经是 `#define APP_IWDG_PRESCALER 64` 并被 `app_protect.c` 直接写进 PR：
 *   64 = 0b1000_0000 → 掩码后 PR[2:0] = **0** → 实际分频是 4 而不是 64
 *   ⇒ 真实窗口 = RLR 1250 × 4 ÷ 40000 = **125ms**，而条件喂狗最快 `APP_WDG_CHECK_MS`=500ms
 *   才发生一次 ⇒ **结构性不可能喂得上，机器每 125ms 复位一次**（= 2026-09-27 首次上机的
 *   "屏幕一直闪烁、读不到任何信息"；论证与实测判据见 `附录C` C-104 / `Build.md` §8.19）。
 *   ⚠️ 为什么 D2~D4 没暴露：那几天是无条件每轮喂狗（~20ms < 125ms 够用），错值一直潜伏，
 *   D5 换成"判活 + 500ms 条件喂狗"才把它变成现象 —— 与硬件故障完全同形。
 *   ⚠️ 为什么 §12.6 的七条断言一条都没拦下：它们全部用**设计值** `APP_IWDG_TIMEOUT_MS` 算，
 *   算出来永远是 2000ms。⇒ 从此手写量只有"编码"，分频数与真实窗口都由编码**反算**出来。 */
#define APP_IWDG_PR             4u                                  /* 寄存器编码：4 → 64 分频 */
#define APP_IWDG_PRESCALER      (4u << APP_IWDG_PR)                 /* 分频数 = 4·2^编码 → 64 */
#define APP_IWDG_RLR            ((APP_LSI_HZ / APP_IWDG_PRESCALER) * APP_IWDG_TIMEOUT_MS / 1000u)
                                        /* = 1250 → 625Hz × 2s；上界 4095（12 位重载） */
#define APP_IWDG_REAL_MS        ((APP_IWDG_RLR) * APP_IWDG_PRESCALER * 1000u / APP_LSI_HZ)
                                        /* ★ 由"真正写进寄存器的这两个数"反算出的窗口（标称 LSI 下 = 2000ms） */

/* ==================== 12. BMS 编译期配置（§4.4 / C-01） ====================
 * 换电池规模 / 换 AFE 后缀只改这一段。⛔ 门限值不得散落到 .c（S14）。
 * ★ 本段与上面 §7 的环境监测门限**完全无关**（C-38）：电芯量一律走 APP_BMS_* 前缀。 */

/* ---------- 12.1 电池规模（冻结规格见 PRD.md §1.2，此处只做编译期落地） ---------- */
#define APP_CELL_COUNT          4                   /* ★ 当前串数：4S 磷酸铁锂 */
#define APP_CELL_COUNT_MAX      16                  /* 数组按最大预留 = Modbus 单体块 0x0000~0x000F，改宏不改结构体 */
#define APP_BMS_TEMP_ACTIVE_COUNT   2                   /* 使用中测点：[0]=电芯表面 NTC #1、[1]=SHT30 环境（索引宏见 app_data.h） */
#define APP_BMS_TEMP_COUNT_MAX      8                   /* 数组按最大预留（以后加 NTC 不改结构体） */
/* ⚠️ **D11 必须重新裁决上面这个数**（`附录C` C-48 / `Tech_Design.md` §4.4 ④）：C-48 已把 **PA6 提为"NTC #2 必用"**
 *   （`PRD.md` FR-SOC-05 要"温差 / 最高最低温度"，单路算不出温差，且环境温度不能替代电芯测点）⇒ 实际测点 = **3**，
 *   而 Modbus §6.1 的温度块只有 2 格（`0x0013~0x0014`）。改的时候**四处一起动**：§6.1 扩格（协议改动 = G4）／
 *   `app_data.h` 的索引排布（建议 [0][1]=两路电芯、[2]=环境）／ 本数 2→3 ／ 界面温度行与温差判据取数。
 *   ⛔ **裁决之前不得把 `temp_valid` 的 bit2 置位** —— 未写入的通道标可信 = 把 0.0℃ 当成读数交给判据。 */
#define APP_NOM_CAPACITY_MAH    6000
#define APP_DESIGN_CURRENT_MA   15000               /* 设计上限 15A（额定放 5A / 峰值 10A） */
#define APP_REASON_LEN          24                  /* 告警原因串长度（含 '\0'） */

/* ---------- 12.2 无效值口径（★ 不是 0，0 是"电芯被短接"的合法读数） ---------- */
#define APP_MV_INVALID          0xFFFF

/* ---------- 12.3 AFE 型号与后缀参数 ----------
 * 三个芯片宏只能有一个 = 1；能力表来自 `Tech_Design.md` §3.1。
 * 后缀参数（SLUSBK2I §5 已核实）：03 = 0x08 / CRC 出厂固化开启 / REGOUT 3.3V ← ★ 本版已购
 *                                 06 = 0x18 / CRC 关（唯一现货退路，改这两行即可）
 *                                 00 = REGOUT 2.5V → ALERT 高仅 1.88V < STM32 VIH 2.31V ⛔ 不要买 */
#define APP_AFE_CHIP_BQ76920    1                   /* 3~5S  ← ★ 本版：bq7692003PWR */
#define APP_AFE_CHIP_BQ76940    0                   /* 9~15S，不能配 4S（留档） */
#define APP_AFE_CHIP_BQ76952    0                   /* 3~16S，P2 升级路径 */

#if (APP_AFE_CHIP_BQ76920 + APP_AFE_CHIP_BQ76940 + APP_AFE_CHIP_BQ76952) != 1
#error "AFE 型号必须且只能选一个"
#endif

#define AFE_CHIP_MIN_CELLS      (APP_AFE_CHIP_BQ76920 ? 3u : (APP_AFE_CHIP_BQ76940 ? 9u : 3u))
#define AFE_CHIP_MAX_CELLS      (APP_AFE_CHIP_BQ76920 ? 5u : (APP_AFE_CHIP_BQ76940 ? 15u : 16u))

#define AFE_I2C_ADDR            0x08                /* 7 位地址；06 后缀 = 0x18 */
#define AFE_USE_CRC             1                   /* 03 后缀 = 1（不可关）；00/06 = 0 */

/* ---------- 12.4 测量合理性区间（用于"不可知即不安全"，⛔ 不是保护门限） ---------- */
/* 保护门限在 §12.5 之后随 app_alarm/app_protect 落地，且必须满足 S7：软件比硬件更保守。 */
#define APP_BMS_CELL_MV_VALID_MIN   1500            /* 低于此值不可能是真实电芯（线序错/芯片坏） */
#define APP_BMS_CELL_MV_VALID_MAX   4200            /* 高于此值同理；LFP 正常 2.5~3.65V */
#define APP_BMS_PACK_MV_VALID_MAX   16000           /* 4S × 4.0V 再放一点余量 */
#define APP_AFE_FAIL_MAX            5               /* 连续 I2C 失败上限，达到即判链路断 → 进安全态（S2-①） */

/* ---------- 12.5 编译期断言：把"4S 配 9~15S 芯片"这类错误挡在编译阶段（C-01） ---------- */
#define APP_STATIC_ASSERT(cond, msg)    typedef char app_static_assert_##msg[(cond) ? 1 : -1]

APP_STATIC_ASSERT(APP_CELL_COUNT        >= AFE_CHIP_MIN_CELLS,  cell_below_afe_min);
APP_STATIC_ASSERT(APP_CELL_COUNT        <= AFE_CHIP_MAX_CELLS,  cell_above_afe_max);
APP_STATIC_ASSERT(APP_CELL_COUNT        <= APP_CELL_COUNT_MAX,  array_smaller_than_cells);
APP_STATIC_ASSERT(APP_BMS_TEMP_ACTIVE_COUNT <= APP_BMS_TEMP_COUNT_MAX,  temp_count_overflow);
APP_STATIC_ASSERT(APP_CELL_COUNT_MAX    <= 16u,                 modbus_cell_block_too_small);
/* 采集节拍必须大于 SHT30 的转换窗（`sht30.h` 的 SHT30_MEAS_WAIT_MS = 20ms，手册单次测量
 * 高重复性 max 15ms）：节拍 ≤ 转换窗时两阶段读法**永远返回 BUSY**，环境通道静默无数据。
 * ⚠️ 这里写的是字面量 20 而不是引宏 —— `app_config.h` 不 include BSP 头（分层，§4.2）。 */
APP_STATIC_ASSERT(APP_ACQ_ENV_PERIOD_MS > 20u,                  acq_period_not_above_sht30_conv);
/* ⚠️ 原 §4.4 第 3 条写作 `APP_CELL_COUNT_MAX <= AFE_CHIP_MAX_CELLS`，那是**必然编译不过**的：
 *   数组上限按 Modbus 协议固定 16 槽，而 bq76920 只到 5S（16<=5 为假）。约束的对象是
 *   "实际串数"而不是"数组大小"，故按上面三行重写（见 2026-09-25 变更记录）。 */

/* ---------- 12.6 看门狗三个窗口必须自洽（★ C-34 / C-36） ----------
 * 违反任何一条 = "设备正常运行也会被狗复位"或"真卡死了狗却不咬"，
 * 两者的现场表现都与硬件故障同形，是最费时间的一类误诊 ⇒ 直接编译不过。 */
#define APP_WDG_ASSERT_MS(period_ms, name)  \
    APP_STATIC_ASSERT((period_ms) * APP_WDG_ALIVE_K < APP_IWDG_TIMEOUT_MS, wdg_win_##name)

APP_WDG_ASSERT_MS(APP_PERIOD_PROTECT_MS, protect);
APP_WDG_ASSERT_MS(APP_PERIOD_ACQ_MS,    acq);
APP_WDG_ASSERT_MS(APP_PERIOD_COMM_MS,   comm);
APP_WDG_ASSERT_MS(APP_PERIOD_LOG_MS,    log);
APP_WDG_ASSERT_MS(APP_PERIOD_UI_MS,     ui);
/* ★ 迁移容器（`task_legacy()`，占 `APP_TASK_UI` 这个身份）的窗口也要过同一把尺子：
 *   它跑的是遗留全部业务（采集/联网/落盘调用/界面），它卡死 = 整机没有业务，
 *   ⛔ 不能因为"它是临时容器"就不判它的活。容器删除时本行随之删除。 */
APP_WDG_ASSERT_MS(APP_PERIOD_LEGACY_MS, legacy);
APP_STATIC_ASSERT(APP_WDG_CHECK_MS * 2u < APP_IWDG_TIMEOUT_MS,   wdg_check_period_too_long);
APP_STATIC_ASSERT(APP_IWDG_RLR       <= 4095u,                   wdg_rlr_overflow_12bit);
APP_STATIC_ASSERT(APP_IWDG_RLR       >= 1u,                      wdg_rlr_zero);
/* ★★ C-104 新增的两条：**按真正写进寄存器的值**算，不按设计值算。
 *   上面七条全部引用 `APP_IWDG_TIMEOUT_MS`（人写的目标值），所以它们证明的是"我想要的
 *   三个窗口自洽"，而**不是**"我写进寄存器的那两个数产生的窗口自洽" —— 这两件事在
 *   "编码/数量写错一位"时完全不同，而错一位不报任何错（现象 = 复位循环，与硬件故障同形）。
 *   实测证据：把 `APP_IWDG_PR` 临时改成 4u<<2 那种"传分频数"的写法（= 曾经的 64），
 *   下面第一条立即 `negative size` 报错，而 §12.6 原有七条全绿。 */
APP_STATIC_ASSERT(APP_IWDG_PR            <= 6u,                  iwdg_pr_encoding_overflow);
APP_STATIC_ASSERT(APP_IWDG_REAL_MS       >= (APP_WDG_CHECK_MS * 2u), iwdg_real_window_below_feed_window);

/* ---------- 12.7 BMS 三级告警门限（★ 附录C · C-38：电池量一律 APP_BMS_*） ----------
 * 数值的**唯一来源是 `PRD.md` §2.3**，本节只把表里的十进制数换成内部量纲的宏，
 * 不引入新数值、不改数值。单位（§2.2 / `Tech_Design.md` §4.4，内部量纲 = Modbus 量纲）：
 *   单体电压 mV ｜ 电流 10mA ｜ 温度 0.1℃ ｜ 温升速率 0.1℃/min。
 * ⛔ 与 §7 的环境门限**不得混用**：环境宏不得进 BMS 代码，本节宏不得进环境代码。
 *
 * ⚠️ 三处**尚未落实**，不得当成已完成：
 *   ① 电流/温度表是按 LFP 6Ah（C=6A）换算的**模板**，电芯规格书到手必须复核
 *      "三选最小"（AFE 量程 / 线束 15A / 电芯最大持续倍率），PRD §2.3 同一条要求；
 *   ② **压差判据 PRD 没给数值**（FR-ALM-05 用到"压差大"）→ 下面填 0 = 该判据不启用；
 *      D14 之前不补数值，融合规则里这一条就是空转；
 *   ③ 硬件层的 3.65/2.50V 与 OCC/OCD/SCD 的**寄存器落点**在 D8 落实，
 *      数值必须与下面一致（S7 的层次关系由 §12.7.6 的断言守住）。
 *      ⚠️ **三条 D8 对册时才能收口的口径**（`附录C` · C-70）：
 *      a) 硬件过流比较器量的是**外接采样电阻两端的 mV**，不是安培 ⇒ 下面三个安培值
 *         只有在**采样电阻已焊、阻值/极性正确**时才成立（缺该电阻 = 硬件过流层静默不存在）；
 *      b) 寄存器只有**离散档位** ⇒ 6A / 12A / 30A 会被量化到最近档（12A 档对应约 12.2A），
 *         落点见 `Tech_Design.md` §3.1.1 ④「门限-寄存器对照」；
 *      c) 过流延迟最大档 = **1280ms**，原写的"2s"这一档**不存在** ⇒ 下面已改。 */

/* 12.7.1 单体电压（FR-ALM-01，mV）：一级提示+降额 50% ｜ 二级降额 20% ｜ 三级切断+锁定 */
#define APP_BMS_CELL_OV_L1_MV       3500
#define APP_BMS_CELL_OV_L2_MV       3550
#define APP_BMS_CELL_OV_L3_MV       3600    /* 三级：断充电 */
#define APP_BMS_CELL_UV_L1_MV       3000
#define APP_BMS_CELL_UV_L2_MV       2900
#define APP_BMS_CELL_UV_L3_MV       2800    /* 三级：断放电 */

/* 12.7.2 温度（FR-ALM-02，0.1℃；充电/放电必须分开） */
#define APP_BMS_T_CHG_OT_L1_X10     450     /* 充电过热 ≥45℃ */
#define APP_BMS_T_CHG_OT_L2_X10     500
#define APP_BMS_T_CHG_OT_L3_X10     550
#define APP_BMS_T_CHG_UT_L1_X10     0       /* 充电过冷 ≤0℃ */
#define APP_BMS_T_CHG_UT_L2_X10     -50
#define APP_BMS_T_CHG_UT_L3_X10     -100
#define APP_BMS_T_DIS_OT_L1_X10     550     /* 放电过热 ≥55℃ */
#define APP_BMS_T_DIS_OT_L2_X10     600
#define APP_BMS_T_DIS_OT_L3_X10     650
#define APP_BMS_T_DIS_UT_L1_X10     -150    /* 放电过冷 ≤-15℃ */
#define APP_BMS_T_DIS_UT_L2_X10     -200
#define APP_BMS_T_DIS_UT_L3_X10     -250
#define APP_BMS_T_DIF_L1_X10        80      /* 温差 ≥8℃ */
#define APP_BMS_T_DIF_L2_X10        120
#define APP_BMS_T_DIF_L3_X10        150
#define APP_BMS_TR_RATE_L1_X10      5       /* 温升速率 ≥0.5℃/min */
#define APP_BMS_TR_RATE_L2_X10      10
#define APP_BMS_TR_RATE_L3_X10      20
#define APP_BMS_TR_HOLD_L1_N        3       /* 持续 N 个判定周期才成立（PRD：3/3/2） */
#define APP_BMS_TR_HOLD_L2_N        3
#define APP_BMS_TR_HOLD_L3_N        2

/* ★ 0℃ 禁充（PRD §2.3 的 LFP 强制条款）—— 它**不是**温度三级的某一级，而是一条独立判据：
 *   动作 = 切断充电（三级力度），但归属"环境型" ⇒ **不锁定**，回升后自动恢复。
 *   ⚠️ 实现时别把它塞进 app_protect 的三级锁定路径（S11 的锁定只给"故障型"）。 */
#define APP_BMS_T_NOCHG_X10         0       /* ≤0℃ 禁止充电 */
#define APP_BMS_T_NOCHG_REC_X10     300     /* 回升 ≥3℃ */
#define APP_BMS_T_NOCHG_REC_MS      60000   /* 且持续 60s 才自动恢复 */

/* 12.7.3 电流（FR-ALM-03，10mA；充电为正、放电为负 ⇒ 判放电用绝对值） */
#define APP_BMS_I_CHG_L1_10MA       360     /* ≥3.6A 持续 5s */
#define APP_BMS_I_CHG_L2_10MA       420     /* ≥4.2A 持续 3s */
#define APP_BMS_I_CHG_L3_10MA       500     /* ≥5.0A 持续 1s */
#define APP_BMS_I_DIS_L1_10MA       650     /* ≥6.5A 持续 5s */
#define APP_BMS_I_DIS_L2_10MA       800     /* ≥8.0A 持续 3s */
#define APP_BMS_I_DIS_L3_10MA       1000    /* ≥10A 持续 1s */
#define APP_BMS_I_HOLD_L1_MS        5000
#define APP_BMS_I_HOLD_L2_MS        3000
#define APP_BMS_I_HOLD_L3_MS        1000
/* 电流突变（短路前兆判据）：|ΔI| > 5A / 100ms，★ 必须用**未滤波原始值** ——
 * 滑动平均的群延迟 (N-1)/2 × 100ms 会把突变抹平（PRD §2.3 底部两条★）。 */
#define APP_BMS_I_JUMP_10MA         500
#define APP_BMS_I_JUMP_MS           100

/* 12.7.4 硬件层（AFE 内部比较器，软件不可改；D8 写 PROTECT 寄存器时落实同样数值） */
#define APP_BMS_CELL_OV_HW_MV       3650
#define APP_BMS_CELL_UV_HW_MV       2500
#define APP_BMS_I_OCC_HW_10MA       600     /* 充电过流 6A */
#define APP_BMS_I_OCD_HW_10MA       1200    /* 放电过流 12A */
#define APP_BMS_I_SCD_HW_10MA       3000    /* 短路 30A，~100µs，⛔ 不得调慢 */
#define APP_BMS_HW_OCC_OCD_MS       1280    /* ★ 硬件延迟取**最大档 1280ms**（不是 2s）——
                                             * 审查 C-14：`"软件更保守"`必须在**门限**与**时间**
                                             * 两个维度同时成立，否则 >12A 时两层同时到点，"软件先动"是假的。
                                             * ⚠️ 原值 2000 作废：AFE 的过流延迟**最大只有 1280ms，2s 这一档不存在**
                                             * （`附录C` · C-70；PRD §2.3 已同步为 1280ms）。
                                             * 1280ms 仍 > 软件三级 1000ms ⇒ C-14 不破。 */

/* 12.7.5 误报抑制（FR-ALM-06）+ 分级确认窗（PRD §2.3 底部★） */
#define APP_BMS_CONFIRM_MS          3000    /* 一级/二级的连续越界确认窗 */
#define APP_BMS_DEBOUNCE_L3_MS      200     /* ★ 三级不得用 3s 窗（与"动作 ≤1s"矛盾）→ ≤200ms 去抖 */
#define APP_BMS_CELL_HYST_MV        20      /* 电压迟滞 20mV（级差 50mV 的 40% ✓） */
#define APP_BMS_T_HYST_X10          10      /* 温度迟滞 1℃ */
#define APP_BMS_T_DIF_HYST_X10      10
/* ★ 电流迟滞口径已修正（附录C · C-67）：PRD 原文"5% FS"在两个候选 FS 下都**大于 PRD
 *   自己的 L1→L2 级差**（INA226 满量程 16.384A → 0.82A；设计上限 15A → 0.75A；
 *   而 L1→L2 = 3.6→4.2A = 0.6A）。迟滞窗比级差还宽 ⇒ 一级的解除点比二级的触发点低了
 *   整个二级区间 ⇒ **电流维度的分级失效**。
 *   ⇒ 改为"5% × 该方向**最低级**门限"（最保守，且对最小级差仍满足"迟滞 < 级差的一半"）：
 *     充电 5%×3.6A = 18，放电 5%×6.5A = 33。口径已同步回 `PRD.md` §2.3。 */
#define APP_BMS_I_HYST_CHG_10MA     18
#define APP_BMS_I_HYST_DIS_10MA     33
/* 温升速率**不叠加迟滞**（C-67 的第二半）：1℃/min 的迟滞大于 L1(0.5)/L2(1.0) 的整个级差，
 * 加了会把速率告警永久卡住。该量靠"持续 N 周期"（TR_HOLD_*_N）去抖即可。 */
#define APP_BMS_TR_RATE_HYST_X10    0
/* 压差判据：⚠️ PRD §2.3 未给数值 → 0 = 不启用（"压差大+温差大 → 升二级"这一条空转）。 */
#define APP_BMS_CELL_DIF_MV         0
#define APP_BMS_CELL_DIF_HOLD_N     3       /* 持续性要求：避开均衡瞬间（均衡时压差会瞬时变大） */

/* ---------- 12.7.6 门限层次断言（★ AGENTS S7 / 审查 C-14，违反即编译不过） ----------
 * S7 的原话是"软件三级必须比硬件层更保守"，但这句话在电压、电流、时间三个维度上
 * 分别是三种比较方向（过压要更低、欠压要更高、动作要更早）—— 靠人工对照极容易搞反，
 * 搞反的后果是"硬件先动、软件不带日志地静默断电"或"软件根本不拦"。⇒ 全部写成断言。 */
#define APP_BMS_ASSERT_ASC(a, b, name)  APP_STATIC_ASSERT((a) < (b),  ord_##name)
#define APP_BMS_ASSERT_DESC(a, b, name) APP_STATIC_ASSERT((a) > (b),  ord_##name)
#define APP_BMS_ASSERT_HYST(hy, gap, name) \
    APP_STATIC_ASSERT((hy) * 2 < (gap), hyst_##name)

/* ① 软件内部三级必须单调（防止填表时串行） */
APP_BMS_ASSERT_ASC(APP_BMS_CELL_OV_L1_MV, APP_BMS_CELL_OV_L2_MV, ov12);
APP_BMS_ASSERT_ASC(APP_BMS_CELL_OV_L2_MV, APP_BMS_CELL_OV_L3_MV, ov23);
APP_BMS_ASSERT_DESC(APP_BMS_CELL_UV_L1_MV, APP_BMS_CELL_UV_L2_MV, uv12);
APP_BMS_ASSERT_DESC(APP_BMS_CELL_UV_L2_MV, APP_BMS_CELL_UV_L3_MV, uv23);
APP_BMS_ASSERT_ASC(APP_BMS_T_CHG_OT_L1_X10, APP_BMS_T_CHG_OT_L2_X10, cot12);
APP_BMS_ASSERT_ASC(APP_BMS_T_CHG_OT_L2_X10, APP_BMS_T_CHG_OT_L3_X10, cot23);
APP_BMS_ASSERT_DESC(APP_BMS_T_CHG_UT_L1_X10, APP_BMS_T_CHG_UT_L2_X10, cut12);
APP_BMS_ASSERT_DESC(APP_BMS_T_CHG_UT_L2_X10, APP_BMS_T_CHG_UT_L3_X10, cut23);
APP_BMS_ASSERT_ASC(APP_BMS_T_DIS_OT_L1_X10, APP_BMS_T_DIS_OT_L2_X10, dot12);
APP_BMS_ASSERT_ASC(APP_BMS_T_DIS_OT_L2_X10, APP_BMS_T_DIS_OT_L3_X10, dot23);
APP_BMS_ASSERT_ASC(APP_BMS_I_CHG_L1_10MA, APP_BMS_I_CHG_L2_10MA, ic12);
APP_BMS_ASSERT_ASC(APP_BMS_I_CHG_L2_10MA, APP_BMS_I_CHG_L3_10MA, ic23);
APP_BMS_ASSERT_ASC(APP_BMS_I_DIS_L1_10MA, APP_BMS_I_DIS_L2_10MA, id12);
APP_BMS_ASSERT_ASC(APP_BMS_I_DIS_L2_10MA, APP_BMS_I_DIS_L3_10MA, id23);

/* ② S7：软件三级必须比硬件层更保守（过压/欠压两个方向相反） */
APP_BMS_ASSERT_ASC(APP_BMS_CELL_OV_L3_MV, APP_BMS_CELL_OV_HW_MV, s7_ov);
APP_BMS_ASSERT_DESC(APP_BMS_CELL_UV_L3_MV, APP_BMS_CELL_UV_HW_MV, s7_uv);
/* ③ C-14：门限更严 + 动作更早 */
APP_BMS_ASSERT_ASC(APP_BMS_I_CHG_L3_10MA, APP_BMS_I_OCC_HW_10MA, s7_occ);
APP_BMS_ASSERT_ASC(APP_BMS_I_DIS_L3_10MA, APP_BMS_I_OCD_HW_10MA, s7_ocd);
APP_STATIC_ASSERT(APP_BMS_I_HOLD_L3_MS < APP_BMS_HW_OCC_OCD_MS,        s7_time);
APP_STATIC_ASSERT(APP_BMS_I_DIS_L3_10MA <= (APP_DESIGN_CURRENT_MA / 10), l3_over_design);
APP_STATIC_ASSERT(APP_BMS_I_SCD_HW_10MA > APP_BMS_I_OCD_HW_10MA,       scd_under_ocd);

/* ④ 迟滞必须小于相邻级差的一半（否则解除点跨过上一级 → 分级失效，见 C-67） */
APP_BMS_ASSERT_HYST(APP_BMS_CELL_HYST_MV, APP_BMS_CELL_OV_L2_MV - APP_BMS_CELL_OV_L1_MV, cell_ov);
APP_BMS_ASSERT_HYST(APP_BMS_CELL_HYST_MV, APP_BMS_CELL_UV_L1_MV - APP_BMS_CELL_UV_L2_MV, cell_uv);
APP_BMS_ASSERT_HYST(APP_BMS_T_HYST_X10, APP_BMS_T_CHG_OT_L2_X10 - APP_BMS_T_CHG_OT_L1_X10, t_chg);
APP_BMS_ASSERT_HYST(APP_BMS_T_HYST_X10, APP_BMS_T_DIS_UT_L1_X10 - APP_BMS_T_DIS_UT_L2_X10, t_dis);
APP_BMS_ASSERT_HYST(APP_BMS_T_DIF_HYST_X10, APP_BMS_T_DIF_L2_X10 - APP_BMS_T_DIF_L1_X10, t_dif);
APP_BMS_ASSERT_HYST(APP_BMS_I_HYST_CHG_10MA, APP_BMS_I_CHG_L2_10MA - APP_BMS_I_CHG_L1_10MA, i_chg);
APP_BMS_ASSERT_HYST(APP_BMS_I_HYST_DIS_10MA, APP_BMS_I_DIS_L2_10MA - APP_BMS_I_DIS_L1_10MA, i_dis);
/* ⑤ 合理性区间（§12.4）必须容得下所有门限，否则"合法读数"会被当成无效值丢掉 */
APP_STATIC_ASSERT(APP_BMS_CELL_MV_VALID_MAX > APP_BMS_CELL_OV_HW_MV, valid_ov);
APP_STATIC_ASSERT(APP_BMS_CELL_MV_VALID_MIN < APP_BMS_CELL_UV_HW_MV, valid_uv);
/* ⑥ 0℃ 禁充：切断点必须落在"充电过冷三级"以内，恢复点必须高于切断点 */
APP_STATIC_ASSERT(APP_BMS_T_NOCHG_X10 >= APP_BMS_T_CHG_UT_L3_X10,  nochg_in_range);
APP_STATIC_ASSERT(APP_BMS_T_NOCHG_REC_X10 > APP_BMS_T_NOCHG_X10,   nochg_recover_dir);
/* ⑦ 三级去抖 + 一个保护周期不得吃掉"动作 ≤1s"的预算（PRD §4） */
APP_STATIC_ASSERT(APP_BMS_DEBOUNCE_L3_MS + APP_PERIOD_PROTECT_MS <= 1000u, l3_budget);

/* ---------- 12.8 化学体系（★ 类型定义放配置头：§12.1 的规模参数与 app_data.h 的结构体都要用它，
 * 而 app_data.h 反过来包含本文件 —— 放在 app_data.h 里就成环。
 * Tech_Design.md §4.4 的"编译期配置"块本来就把这三行划给 app_config.h。） ---------- */
typedef enum
{
    CHEM_LFP = 0,
    CHEM_NCM,
    CHEM_NA_ION
} chem_type_t;

#define APP_BMS_CHEM            CHEM_LFP      /* 4S 磷酸铁锂（PRD.md §1.2 冻结规格） */

/* ---------- 12.9 数据新鲜度（§4.4「不可知即不安全」的量化口径） ----------
 * 任一通道超过这个时间没更新 → 标记该通道无效 + 置 link_health 对应位；
 * 保护判据不得使用无效通道的数据，降级方向一律朝"禁止动作"，不朝放行。 */
#define APP_DATA_STALE_MS       5000

/* ---------- 12.10 开关时序：长按解除锁定（§5.2 表；其余 APP_CTR_* 随 D5 的 app_protect 落地） ----------
 * S11：三级（切断）动作后必须锁定，而锁定**只能**由"长按 KEY0 达这个时长"解除。
 * 界面长按判据与将来的解除判据是同一条 → 只允许一处定义（S14）。 */
#define APP_CTR_LATCH_HOLD_MS   5000

/* ---------- 12.11 定点滤波（FR-ACQ-06，`PRD.md` §2 采集 / §4 时间预算） ----------
 * 一阶低通：`y += (x - y) / 2^S`。累加器多留 S 位小数 ⇒ 末位不会被"卡在离目标还差半格"
 * 的位置上（无死区），且全程只有移位与加减 —— ⛔ 不用浮点（§2.2：浮点只在 app_soc 与界面格式化）。
 *
 * ★ 落点与滞后账（PRD:193「保护判定必须用原始值，滤波值只用于 SOC 积分与显示」）：
 *   D4 只接环境通道（SHT30，500ms 一拍）。它的消费者是遗留环境报警与界面，
 *   ⛔ 不是 BMS 保护判据（`APP_BMS_T_*` 走电芯 NTC，属主 D11）⇒ 与本条不冲突。
 *   FR-ACQ-06 真正的两条落在后面，届时**必须复用这里同一个 `filt_lpf_step()`**：
 *     · 电压一阶低通 → D7 的 AFE 单体电压（TaskAcq 100ms 拍）
 *     · 电流滑动平均 → D10 的 INA226 电流（N 抽头平均的群延迟 = (N−1)/2 拍，
 *                        N=4 @100ms 即 150ms ≤ PRD 的 200ms 上限；⛔ 保护与"电流突变"
 *                        判据必须取**未平均的原始值**）
 *
 * ⚠️ **D7 前必须裁决的冲突（不在此处擅自解决）**：`bms_raw_t` 每通道只有**一个**值字段
 *   （`curr_10ma` / `cell_mv[]`），而 PRD:193 要求"原始值喂保护 + 滤波值喂 SOC/显示"**同时存在**。
 *   要么扩快照结构（§4.4 数据模型改动 = G4，Modbus 映射要说明取哪一个），
 *   要么滤波只发生在 `app_acq` 内部、原始值另走一条口给 `app_protect`。D7 动工前定。 */
#define APP_FILT_LPF_SHIFT      1                   /* S：每拍向目标靠近 1/2^S（1 = 一阶系数 0.5） */

/* 滤波不得吃掉报警确认窗：到 63% 约需 2^S 拍，必须明显小于 `APP_ALARM_CONFIRM_MS`。
 * 顺带钉住 S 的合理区间（S=0 等于不过滤；S 太大则环境报警被拖慢到不可用）。 */
#define APP_FILT_SETTLE_MS      (APP_ACQ_ENV_PERIOD_MS * (1u << APP_FILT_LPF_SHIFT))

APP_STATIC_ASSERT(APP_FILT_LPF_SHIFT >= 1u && APP_FILT_LPF_SHIFT <= 3u, filt_shift_out_of_range);
APP_STATIC_ASSERT(APP_FILT_SETTLE_MS < APP_ALARM_CONFIRM_MS,            filt_eats_alarm_confirm);

/* ==================== 13. 内部 Flash 分区（★ 地址的唯一来源） ====================
 * 设计依据与"为什么这么分"见 `Tech_Design.md` §8；本节只回答"每一页归谁、地址多少"。
 * 512KB = 0x0800_0000 ~ 0x0807_FFFF，页 2KB ⇒ 256 页；尾部 64KB 归数据，固件上限 448KB。
 *
 * ★ 分区纪律：**一个物理页只能有一个属主**。
 *   `stmflash_write()` 是"整页擦 + 整页写回"，两个写者共享一页时，后写那次会把另一方的
 *   数据一起抹掉 —— 现象是"日志偶发乱码"，是最难定位的一类。
 *
 * ⛔ 本节修正的来龙去脉（2026-09-25 发现，**2026-09-27 D4 已闭合**）：前序工程的报警日志把地址
 *   写死在**最后一页** 0x0807_F800，而设计把 0x0807_2000 起整段划给将来的 BMS 事件日志 ⇒ 最后一页
 *   出现了**两个写者**。⇒ 当时把该页显式划出（BMS 日志 28→27 页）；D4 把 `app_log.c` 重写为
 *   BMS 事件日志时**已收回这一页**（`APP_ENVLOG_ADDR` 已删除，遗留环境日志整体切除，
 *   它的报警通路改投新的事件码 `EV_ENV_ALARM`）。
 *
 * ⚠️ 下面只是"写宏"，链接器不会替你验证固件真的小于 448KB：Keil `IROM1 Size` 必须
 *   = `0x70000`（AGENTS §3 补丁 8 / 审查 C-16，已落实并编译验证）。 */
#define APP_FLASH_BASE          0x08000000u
#define APP_FLASH_SIZE_KB       512u
#define APP_FLASH_PAGE_SIZE     2048u
#define APP_FLASH_PAGE_COUNT    (APP_FLASH_SIZE_KB * 1024u / APP_FLASH_PAGE_SIZE)   /* 256 */
#define APP_FW_MAX_SIZE         (448u * 1024u)

#define APP_PARAM_A_ADDR        0x08070000u     /* 门限参数主区（1 页） */
#define APP_PARAM_B_ADDR        0x08070800u     /* 门限参数备份区（1 页，与 A 交替写） */
#define APP_AMPH_ADDR           0x08071000u     /* 累计量：Ah / 循环次数 / SOH（1 页） */
#define APP_CALIB_ADDR          0x08071800u     /* 标定表：每串 k/b + OCV-SOC 曲线（1 页） */
#define APP_LOG_ADDR            0x08072000u     /* ★ BMS 事件日志区起点（2026-09-27 D4 落地） */
#define APP_LOG_PAGES           28u
#define APP_LOG_SIZE            (APP_LOG_PAGES * APP_FLASH_PAGE_SIZE)   /* 57344B = 56KB */

/* ---------- 13.1 日志区几何（引擎 = 逐页环形**追加**，落点 `app_log.c`） ----------
 * 页头 8B = 魔数 + 格式版本 + 页序号；记录槽 16B（编程前 14B，末半字保持擦除态）。
 * ★ 为什么不照 `Tech_Design.md` §8 原写的"页头含记录数 + 整页 CRC"：
 *   那要求每追加一条就重写整页（2KB 编程 ≈ 50ms CPU 全停，S25 的空窗被拉长 100 倍），
 *   而追加式只在**页满时**擦下一页一次 ⇒ 平摊成本降两个数量级。
 *   代价 = "页头记录数 / 页 CRC"与"追加"物理上不可共存（改页头必须擦整页）
 *   ⇒ 损坏识别全部下沉到**单条 CRC16**，页尾空洞由"末半字 = 0xFFFF 即未提交"识别（审查 C-101）。
 *   ⇒ 同一条理由也解释了为什么页头不挂 CRC：半页写坏时页头仍是魔数，坏的是记录，逐条跳过即可。 */
#define APP_LOG_HDR_SIZE        8u
#define APP_LOG_SLOT_SIZE       16u
#define APP_LOG_PER_PAGE        ((APP_FLASH_PAGE_SIZE - APP_LOG_HDR_SIZE) / APP_LOG_SLOT_SIZE)  /* 127 */
#define APP_LOG_CAPACITY        (APP_LOG_PAGES * APP_LOG_PER_PAGE)                              /* 3556 */
#define APP_LOG_FMT_VER         1u              /* 记录格式版本，与 §6.1 的 0x0305 寄存器同口径 */
#define APP_LOG_MAGIC           0x424Cu         /* 'B','L' = BMS Log 页头魔数 */
#define APP_LOG_QUEUE_DEPTH     8u              /* 待写队列深度（条）：满则丢最旧并计数，⛔ 不得阻塞调用方 */

/* 分区边界由编译期把住：填错地址、或把日志区撑过最后一页，直接编译不过。 */
#define APP_FLASH_ASSERT_NEXT(a, b, name) \
    APP_STATIC_ASSERT((b) >= ((a) + APP_FLASH_PAGE_SIZE), flash_ovl_##name)

APP_STATIC_ASSERT(APP_PARAM_A_ADDR == APP_FLASH_BASE + APP_FW_MAX_SIZE, flash_fw_gap);
APP_STATIC_ASSERT((APP_PARAM_B_ADDR % APP_FLASH_PAGE_SIZE) == 0u, flash_align_pb);
APP_STATIC_ASSERT((APP_AMPH_ADDR    % APP_FLASH_PAGE_SIZE) == 0u, flash_align_amp);
APP_STATIC_ASSERT((APP_CALIB_ADDR   % APP_FLASH_PAGE_SIZE) == 0u, flash_align_cal);
APP_STATIC_ASSERT((APP_LOG_ADDR     % APP_FLASH_PAGE_SIZE) == 0u, flash_align_log);
APP_FLASH_ASSERT_NEXT(APP_PARAM_A_ADDR, APP_PARAM_B_ADDR, pa_pb);
APP_FLASH_ASSERT_NEXT(APP_PARAM_B_ADDR, APP_AMPH_ADDR,    pb_amp);
APP_FLASH_ASSERT_NEXT(APP_AMPH_ADDR,    APP_CALIB_ADDR,   amp_cal);
APP_FLASH_ASSERT_NEXT(APP_CALIB_ADDR,   APP_LOG_ADDR,     cal_log);
/* ★ D4 收回遗留环境日志那一页之后，日志区必须**正好**撑到 Flash 最后一页末尾
 *   （原 `log_size_vs_env` 断言随 APP_ENVLOG_ADDR 一起删除，本条接替它做"闭合"检查）。 */
APP_STATIC_ASSERT(APP_LOG_ADDR + APP_LOG_SIZE ==
                  APP_FLASH_BASE + APP_FLASH_SIZE_KB * 1024u, log_tail_full);
/* `PRD.md` §5 要求"≥300 条循环覆盖"：按**真实槽位布局**（不是估的 32B/条）算出的容量必须满足。 */
APP_STATIC_ASSERT(APP_LOG_CAPACITY >= 300u,     log_capacity_low);
APP_STATIC_ASSERT((APP_LOG_HDR_SIZE % 2u) == 0u && (APP_LOG_SLOT_SIZE % 2u) == 0u, log_hw_align);
APP_STATIC_ASSERT((APP_LOG_HDR_SIZE + APP_LOG_PER_PAGE * APP_LOG_SLOT_SIZE) <= APP_FLASH_PAGE_SIZE, log_page_fit);
APP_STATIC_ASSERT(APP_FLASH_PAGE_COUNT == 256u,   flash_pages_wrong);

#endif

