/**
 ****************************************************************************************************
 * @file        app_log.h
 * @brief       **BMS 事件日志**（内部 Flash 环形追加 + 单条 CRC16 + 异步落盘）
 * @note        本文件是 `Tech_Design.md` §8「事件日志」与 §6.1「0x0200 日志读取协议」的代码落点，
 *              2026-09-27 D4 起取代前序工程的"环境报警历史"（旧版把 64 条温湿度/光照存在 Flash
 *              最后一页，那一页已随 `附录C` C-67 收回，见 `app_config.h` §13）。
 *
 * @note        ★ 三条硬纪律（决定了下面每个 API 的形状）：
 *              ① **调用方不得停**：`app_log_add()` 只把记录塞进一个静态环形待写队列就返回，
 *                 真正的 FLASH 擦写发生在 `app_log_poll()` 里（D5 起 = TaskLog）。
 *                 理由：一次页擦除 20~40ms 且 CPU 全停（S25），而报警回调可能来自 10ms 的
 *                 TaskProtect —— 在保护路径里擦 FLASH = 保护空窗，是最不能接受的一种"顺手"。
 *                 ⛔ 队列满时**丢最旧**并计数，⛔ 不得改成"等空位"（那等于把阻塞换了个地方）。
 *              ② **不可知即不安全**：擦写窗口内必须挂 `BMS_LINK_FLASH_BUSY`（§4.4），
 *                 由本模块通过 `app_data_flash_busy()` 报出，界面 `Link` 行的 `FLASH` 位据此亮。
 *                 ⛔ 不得只在本模块内部记一个 bool 就完事 —— 那个 bool 谁也看不见。
 *              ③ **一条坏不能拖累一片**：**提交半字最后编程**（CRC 半字在它前面），所以"写一半掉电"的记录
 *                 天然处于未提交态，上电扫描逐条跳过（并汇总成一条 `EV_LOG_CORRUPT`）。
 *
 * @note        存储布局（几何与断言的唯一来源 = `app_config.h` §13.1）：
 *                日志区 28 页 × 2KB，每页 = 页头 8B + 127 个 16B 槽 ⇒ 容量 3556 条（要求 ≥300）。
 *                页头  [0..1] 魔数 'BL'  [2..3] 格式版本  [4..7] 页序号(u32, 单调 +1)
 *                槽    ts_unix(u32) 事件码(u16) 原因码(u16) aux_a(u16) aux_b(u16) CRC16(u16) + 提交半字(u16)
 *              ★ 页头**没有**"记录数"与"整页 CRC"：那两样要求每追加一条就重写页头，而重写页头
 *                必须擦整页 ⇒ 与"追加"物理上不可共存。损坏识别全部下沉到单条 CRC（审查 C-101）。
 *
 * @note        时间戳 = Unix 秒，来自 RTC(`Drivers/BSP/RTC`)。没装 VBAT 纽扣电池时 RTC 从固件
 *              编译时刻重新起算 ⇒ 时间戳只能当**相对**参考，但记录本身照样存得住（判据① 不受影响）。
 ****************************************************************************************************
 */

#ifndef __APP_LOG_H
#define __APP_LOG_H

#include "./SYSTEM/sys/sys.h"

/* ==================== 事件码（★ 协议面向上位机，一经 D9 交付即冻结，只许追加不许改义） ====================
 * 与 `Tech_Design.md` §6.1 的 0x0202~0x0241 对齐：每条给 `[ts_hi ts_lo event code]`，
 * ASCII 原因串不进寄存器（放不下），LCD 与串口靠 `app_log_event_str()` 翻译。 */
#define EV_BOOT              0x0001u  /* 上电/复位；code = RCC CSR 复位标志原值 */
#define EV_WDG_RESET         0x0002u  /* ★ 单独一条：狗咬过 = 判活路径失败，页 3 要一眼看到 */
#define EV_ENV_ALARM         0x0010u  /* 环境告警开始（遗留通路，D4 起投到这里）；code = ALARM_FAULT_xxx */
#define EV_ENV_CLEAR         0x0011u  /* 环境告警恢复；code = 曾经出现过的位图 */
#define EV_ALARM_L1          0x0020u  /* ⏳ D8：三级告警 L1 */
#define EV_ALARM_L2          0x0021u  /* ⏳ D8 */
#define EV_ALARM_L3          0x0022u  /* ⏳ D8：切断；code = fault_bits，aux = 当时的组压/电流 */
#define EV_MOS_CLOSE         0x0030u  /* ⏳ D6：闭合；code = 位图(1=充 2=放 4=预充) —— ★ 位序与 §6.1 的
                                      *   0x0020「执行器状态」以及 `actuator.h` 的 ACT_ST_xxx **同一套**，
                                      *   ⛔ 不得再出现第二种"充/放"位序（原注释写的是 1=DSG 2=CHG，
                                      *   与 §6.1 相反 ⇒ D6 落代码时按唯一来源更正，历史 0 条不受影响：
                                      *   本事件在 2026-09-28 之前从未被投过） */
#define EV_MOS_OPEN          0x0031u  /* ⏳ D6：断开 */
#define EV_PRECHARGE_START   0x0032u  /* ⏳ D6 */
#define EV_PRECHARGE_FAIL    0x0033u  /* ⏳ D6：code = 第几次尝试 */
#define EV_LATCH             0x0040u  /* ⏳ D8：进锁定（S11）；code = fault_bits */
#define EV_LATCH_CLEAR       0x0041u  /* ⏳ D8：解除锁定；code = 1 长按 5s / 2 协议鉴权 */
#define EV_LINK_STALE        0x0050u  /* ★ 链路变陈旧（§4.4 新鲜度）；code = 新置起的 BMS_LINK_xxx 位 */
#define EV_LINK_RECOVER      0x0051u  /* ★ 链路恢复；code = 新清掉的位 */
#define EV_TASK_STALLED      0x0052u  /* ★ D5：判活失败（不喂狗 → ≤2s 复位）。code = `app_tasks_stalled_mask()`
                                      *   位图（bit i = `app_task_id_t` 第 i 个任务）。
                                      *   ★ 这条记录是验收项 WDG-3 / §8.4 V3 的**唯一现场证据**：
                                      *     复位后 `EV_WDG_RESET` 只说"狗咬过"，本条说"是谁卡的"。
                                      *   ★ D5 归因轮起 aux 有定义：**aux_a = 最坏那一路的年龄(ms)**、
                                      *     **aux_b = 它自己的判活窗口(ms)** ⇒ 判读= aux_a/aux_b。
                                      *     ⛔ 本事件不得再另用这两列（改语义会让历史 152 条与新读法不一致）。 */
#define EV_AFE_COMM_FAIL     0x0060u  /* ⏳ D7：AFE 通信连续失败（S2 失效安全入口）*/
#define EV_HW_PROTECT        0x0070u  /* ⏳ D8：硬件层解码命中（S26）；code = hw_fault_bits */
#define EV_LOG_CORRUPT       0x00F0u  /* 本模块自检：上电扫描发现的损坏条数（判据②的可见出口）*/

/* ==================== 一条记录（与 Flash 槽位一一对应，去掉 CRC 与填充） ==================== */
typedef struct
{
    uint32_t ts_unix;                   /* Unix 秒（RTC；无 VBAT 电池时是相对值） */
    uint16_t event;                     /* EV_xxx */
    uint16_t code;                      /* 原因码：fault_bits / BMS_LINK_xxx / 复位标志 / 次数 */
    uint16_t aux_a;                     /* 当时量 A：约定 = 组压 10mV（或告警时的温度 0.1℃） */
    uint16_t aux_b;                     /* 当时量 B：约定 = 电流 10mA（充正放负） */
} app_log_rec_t;

/**
 * @brief       上电扫描整个日志区：定位写指针、统计有效条数、发现损坏条则汇总一条事件
 * @note        纯读（存储器映射），不解锁不擦写 ⇒ 可以在启动序列里安全调用。
 */
void     app_log_init(void);

/**
 * @brief       投一条事件进待写队列（**不阻塞**，纪律①）
 * @param       event   EV_xxx
 * @param       code    原因码
 * @param       aux_a   当时组压 10mV（或该事件自定义的 A 量）
 * @param       aux_b   当时电流 10mA
 * @retval      0 = 已入队; 1 = 队列满 ⇒ 丢最旧一条（`app_log_dropped()` 会 +1）
 */
uint8_t  app_log_add(uint16_t event, uint16_t code, uint16_t aux_a, uint16_t aux_b);

/**
 * @brief       排空待写队列（每个周期最多落一条），并按需回收整页
 * @param       now_ms  当前毫秒（内部按 `APP_PERIOD_LOG_MS` 限流）
 * @note        ⛔ 不得在中断里调用（S8：Flash 擦写）。D5 起的正式调用点 = TaskLog。
 */
void     app_log_poll(uint32_t now_ms);

/* ==================== 只读出口 ==================== */
uint16_t app_log_count(void);                       /* 当前有效条数（≤ APP_LOG_CAPACITY） */
uint16_t app_log_damaged(void);                     /* 本次上电扫描出的 CRC 坏条数（判据②） */
uint16_t app_log_dropped(void);                     /* 队列满被丢弃的累计条数（纪律①的代价可见） */
uint8_t  app_log_pending(void);                     /* 待写队列里还有几条没落盘 */

/**
 * @brief       取一条记录
 * @param       idx  0 = 最新一条，依次往前翻
 * @param       out  输出
 * @retval      1 = 取到; 0 = 越界或该条已损坏
 */
uint8_t  app_log_get(uint16_t idx, app_log_rec_t *out);

void     app_log_clear(void);                       /* 清空：整区擦除按"一次一页"排进 poll，⛔ 不在本函数里停 1s */

/**
 * @brief       事件码 → 短标签（LCD/串口共用，ASCII）
 */
void     app_log_event_str(uint16_t event, char *dst, uint16_t dstsz);

/**
 * @brief       Unix 秒 → "MM-DD HH:MM"（页 3 的时间列）
 */
void     app_log_ts_str(uint32_t ts_unix, char *dst, uint16_t dstsz);

/**
 * @brief       读 RTC 换 Unix 秒（日志与 0x0202 协议共用这一个换算口）
 * @retval      时间戳；RTC 未就绪时返回 0 ⇒ 调用方照实写 0，⛔ 不得拿 `HAL_GetTick()` 凑
 */
uint32_t app_log_now_unix(void);

void     app_log_dump(void);                        /* 全部记录按从新到旧打到串口（⚠️ 目前没有调用者：串口命令入口排在 D9 协议层） */

#endif
