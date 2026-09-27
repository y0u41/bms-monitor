/**
 ****************************************************************************************************
 * @file        app_log.c
 * @brief       BMS 事件日志引擎：28 页环形 · 槽位追加 · 单条 CRC16 · 异步落盘
 * @note        对外契约见 `app_log.h`；几何与断言唯一来源 = `app_config.h` §13.1；
 *              协议侧（0x0200 起的日志读取）见 `Tech_Design.md` §6.1，存储设计见 §8。
 *
 * @note        ★ 为什么是"逐条追加"而不是 §8 旧稿的"页头记记录数 + 整页 CRC"（审查 C-101）：
 *              页头一旦要改，F103 就必须**擦整页**才能写回（FLASH 只能把 1 写成 0），
 *              于是"每追加一条 = 擦一页(20~40ms) + 整页回写(≈50ms) 的 CPU 全停"（= S25 的空窗），
 *              而事件恰恰会连着来。改成"槽位追加 + 单条 CRC"后：
 *                追加一条 = 8 个半字 ≈ 0.35ms；每 127 条才擦一页；
 *                识别损坏的能力**不降反升** —— 整页 CRC 只能说"这一页废了"(丢 127 条)，
 *                单条 CRC 能说"只有这一条坏"(丢 1 条)，其余照样读得出来。
 *
 * @note        ★ 提交位的顺序是本文件唯一"错了不报错"的地方，所以钉死在这里：
 *                 先编程 [0..6]（数据 + CRC），**最后**编程 [7] 提交半字(0x0000)。
 *              反过来的话，"写到一半掉电"的槽会有合法 CRC 却没有提交位 ⇒
 *              下次追加认为该槽可写 ⇒ 往非空半字上再编程，结果不可预期。
 *              现在的顺序下："没有提交位"= 要么空、要么撕裂，扫描一律跳过并计入 `damaged`，
 *              撕裂槽被永久让开 ⇒ 掉电自愈不需要任何额外动作（判据②的另一半）。
 *
 * @note        ★ 本模块**绝不**让 `stmflash_write()` 走"目标非空 ⇒ 整页读-擦-合并-写回"那条分支：
 *              它会不报错地把同页另外 127 条一起抹掉（C-67① 的"一页两写者"就是这个形状）。
 *              落盘前逐半字确认目标槽是 0xFFFF，不是就跳过该槽 —— 见 `log_slot_is_blank()`。
 *
 * @note        验证方式（`Build.md` D4 判据；实测记录写进 `Build.md` §8）：
 *              ① 掉电不丢：写 N 条 → 拔主电源 → 上电后条数与内容一致（实机级）；
 *              ② CRC 可见：用调试器把某槽 CRC 半字改成错值 → 上电 `app_log_damaged()` ≥ 1
 *                 且串口/页 3 出现一条 `EV_LOG_CORRUPT`，其余记录仍逐条读得出来；
 *              ③ 停采 5s：拔掉 SHT30 → `BMS_LINK_SHT30` 置位 → 日志里一条 `EV_LINK_STALE`；
 *              ④ 几何：`app_config.h` §13.1 的断言（含"日志区末尾 = Flash 最后一页"）编译期通过。
 ****************************************************************************************************
 */

#include "./App/app_log.h"
#include "./App/app_config.h"
#include "./App/app_data.h"
#include "./BSP/STMFLASH/stmflash.h"
#include "./BSP/RTC/rtc.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

/* ==================== 槽位布局（16 字节 = 8 个半字，与 app_log.h 的注释一一对应） ==================== */
#define SLOT_HW_TS_LO     0u
#define SLOT_HW_TS_HI     1u
#define SLOT_HW_EVENT     2u
#define SLOT_HW_CODE      3u
#define SLOT_HW_AUX_A     4u
#define SLOT_HW_AUX_B     5u
#define SLOT_HW_CRC       6u
#define SLOT_HW_COMMIT    7u
#define SLOT_HW_NUM       8u
#define SLOT_HW_DATA_N    6u                  /* CRC 只覆盖这 6 个半字（不含 CRC 自己与提交位） */

#define HW_ERASED         0xFFFFu
#define COMMIT_YES        0x0000u             /* ★ 最后编程；0xFFFF = 空闲或撕裂 */

/* 槽状态 */
#define SLOT_FREE         0u
#define SLOT_VALID        1u
#define SLOT_DAMAGED      2u                  /* 撕裂、或提交位不认识、或 CRC 不符 */

#define LOG_CLEAR_IDLE    0xFFFFu

/**
 * @brief       擦写空窗标志的"保持拍数"
 * @note        为什么不能"擦完立刻清零"：真正擦除的那 20~40ms 里 CPU 全停，谁也读不到这个标志；
 *              唯一可观测的窗口是它**前后**那几拍。取 12 拍(每拍 20ms ⇒ ≈240ms)的理由是
 *              界面 `Link` 行按 200ms 限流重绘 ⇒ 窗口不长于刷新周期就永远看不见，
 *              S25 要求的"擦写期间不得声称保护有效"会退化成一行注释。
 *              ⚠️ 它是**声明**不是**保护**：置位期间软件层确实可能空窗，硬件层(AFE 比较器)
 *              不受影响、仍必须有效（S1/S2）。
 */
#define APP_LOG_BUSY_HOLD_POLLS   12u

/* ==================== 运行期状态（全静态，AGENTS §2.3「内存」行） ==================== */
static uint32_t      s_seq[APP_LOG_PAGES];        /* 0 = 该页不是本格式的页 */
static uint16_t      s_valid[APP_LOG_PAGES];      /* 该页有效条数 */
static uint16_t      s_used[APP_LOG_PAGES];       /* 该页已占用的槽数(含撕裂/损坏)，也= 下一页写指针 */

static uint16_t      s_count;                     /* 全区已落盘有效条数(≤ APP_LOG_CAPACITY) */
static uint16_t      s_damaged;                   /* 本次上电扫出/写坏的条数 */
static uint16_t      s_dropped;                   /* 队列满被丢 + 落盘失败被丢（累计） */

static uint16_t      s_wpage;                     /* 写页 = 当前最新页 */
static uint16_t      s_wslot;                     /* 写页上下一个待用槽 */
static uint8_t       s_wpage_live;                /* 1 = 写页页头已在 Flash 里；0 = 待擦+待写头 */
static uint32_t      s_next_seq;                  /* 下次回收整页时要写的页序号 */
static uint8_t       s_ready;                     /* 0 = 引擎自认不可用（格式化失败过） */
static uint8_t       s_busy_left;                 /* 擦写空窗标志剩余拍数 */
static uint16_t      s_clear_pg;                  /* != LOG_CLEAR_IDLE 表示正在按页清空 */
static uint32_t      s_last_poll;

/* 待写队列：环形（head = 下一次写入位置，tail = 下一次取出位置） */
static app_log_rec_t s_q[APP_LOG_QUEUE_DEPTH];
static uint8_t       s_q_head;
static uint8_t       s_q_tail;
static uint8_t       s_q_n;

/* ==================== 地址换算 ==================== */
static uint32_t log_page_addr(uint16_t pg)
{
    return APP_LOG_ADDR + ((uint32_t)pg * (uint32_t)APP_FLASH_PAGE_SIZE);
}

static uint32_t log_slot_addr(uint16_t pg, uint16_t slot)
{
    return log_page_addr(pg) + (uint32_t)APP_LOG_HDR_SIZE
           + ((uint32_t)slot * (uint32_t)APP_LOG_SLOT_SIZE);
}

/* ==================== CRC16/CCITT-FALSE（poly 0x1021, init 0xFFFF） ==================== */
static uint16_t log_crc_step(uint16_t crc, uint8_t b)
{
    uint8_t i;

    crc ^= (uint16_t)((uint16_t)b << 8);

    for (i = 0u; i < 8u; i++)
    {
        crc = (uint16_t)(((crc & 0x8000u) != 0u) ? (uint16_t)((uint16_t)(crc << 1) ^ 0x1021u)
                                                 : (uint16_t)(crc << 1));
    }

    return crc;
}

/**
 * @brief       对若干个半字算 CRC
 * @note        半字按**小端两字节**喂进去（低字节在前），和它在 Flash 里的字节序一致 ⇒
 *              上位机照 0x0200 起的原始字节流重算能得到同一个值，不必猜定标。
 */
static uint16_t log_crc16(const uint16_t *hw, uint16_t n)
{
    uint16_t crc = 0xFFFFu;
    uint16_t i;

    for (i = 0u; i < n; i++)
    {
        crc = log_crc_step(crc, (uint8_t)(hw[i] & 0x00FFu));
        crc = log_crc_step(crc, (uint8_t)(hw[i] >> 8));
    }

    return crc;
}

/* ==================== 读侧原语 ==================== */
static void log_read_slot(uint16_t pg, uint16_t slot, uint16_t *hw)
{
    stmflash_read(log_slot_addr(pg, slot), hw, SLOT_HW_NUM);
}

static void log_hw2rec(const uint16_t *hw, app_log_rec_t *out)
{
    out->ts_unix = ((uint32_t)hw[SLOT_HW_TS_HI] << 16) | (uint32_t)hw[SLOT_HW_TS_LO];
    out->event   = hw[SLOT_HW_EVENT];
    out->code    = hw[SLOT_HW_CODE];
    out->aux_a   = hw[SLOT_HW_AUX_A];
    out->aux_b   = hw[SLOT_HW_AUX_B];
}

/**
 * @brief       判一个槽是"空闲 / 有效 / 损坏"
 * @note        SLOT_DAMAGED **不终止**扫描：要一直走到第一个 SLOT_FREE 为止，
 *              否则一个坏槽会把它后面那些好记录一起判成"不存在"（纪律③）。
 */
static uint8_t log_slot_state(uint16_t pg, uint16_t slot, app_log_rec_t *out)
{
    uint16_t hw[SLOT_HW_NUM];
    uint16_t i;

    log_read_slot(pg, slot, hw);

    if (hw[SLOT_HW_COMMIT] == HW_ERASED)
    {
        for (i = 0u; i < SLOT_HW_NUM; i++)
        {
            if (hw[i] != HW_ERASED)
            {
                return SLOT_DAMAGED;          /* 撕裂：数据写了一半，提交位还没落 */
            }
        }

        return SLOT_FREE;
    }

    if (hw[SLOT_HW_COMMIT] != COMMIT_YES)
    {
        return SLOT_DAMAGED;                  /* 提交位不是 0x0000：不是本格式写出来的 */
    }

    if (log_crc16(hw, SLOT_HW_DATA_N) != hw[SLOT_HW_CRC])
    {
        return SLOT_DAMAGED;
    }

    if (out != NULL)
    {
        log_hw2rec(hw, out);
    }

    return SLOT_VALID;
}

/**
 * @brief       落盘前的护栏：确认整个待写槽仍是擦除态
 * @retval      1 = 可以安全追加；0 = 槽里已有东西，⛔ 不得在此写（见文件头那条）
 */
static uint8_t log_slot_is_blank(uint16_t pg, uint16_t slot)
{
    uint16_t hw[SLOT_HW_NUM];
    uint16_t i;

    log_read_slot(pg, slot, hw);

    for (i = 0u; i < SLOT_HW_NUM; i++)
    {
        if (hw[i] != HW_ERASED)
        {
            return 0u;
        }
    }

    return 1u;
}

/* ==================== 写侧原语 ==================== */
/**
 * @brief       回收一页：擦除 + 写页头（全区唯一会擦整页的地方）
 * @param       pg   页号
 * @param       seq  该页的新页序号（单调 +1，上电扫描据此判定"谁是最新页"）
 * @retval      0 = 成功；1 = 失败（擦除报错或页头读回不符）
 * @note        本来就空的页跳过擦除（首次上电 / 刚清过 ⇒ 省掉 20~40ms 的 CPU 全停）。
 */
static uint8_t log_format_page(uint16_t pg, uint32_t seq)
{
    uint32_t addr = log_page_addr(pg);
    uint16_t hdr[4];

    s_busy_left = APP_LOG_BUSY_HOLD_POLLS;    /* ★ 必须在擦除**之前**置起（S25 的可见窗口） */

    if (stmflash_page_is_blank(addr) == 0u)
    {
        if (stmflash_erase_page(addr) != 0u)
        {
            printf("[log] page %u erase FAILED\r\n", (unsigned)pg);
            return 1u;
        }
    }

    hdr[0] = (uint16_t)APP_LOG_MAGIC;
    hdr[1] = (uint16_t)APP_LOG_FMT_VER;
    hdr[2] = (uint16_t)(seq & 0xFFFFu);
    hdr[3] = (uint16_t)(seq >> 16);

    stmflash_write(addr, hdr, 4u);

    if (stmflash_read_halfword(addr) != hdr[0] ||
        stmflash_read_halfword(addr + 2u) != hdr[1] ||
        stmflash_read_halfword(addr + 4u) != hdr[2] ||
        stmflash_read_halfword(addr + 6u) != hdr[3])
    {
        printf("[log] page %u header verify FAILED\r\n", (unsigned)pg);
        return 1u;
    }

    s_busy_left = APP_LOG_BUSY_HOLD_POLLS;    /* 编程同样占窗口 */

    s_seq[pg]    = seq;
    s_valid[pg]  = 0u;
    s_used[pg]   = 0u;
    s_wpage_live = 1u;

    return 0u;
}

/**
 * @brief       扫一页：读页头 + 逐槽定性，结果写进 RAM 索引
 * @note        纯读。页头魔数/版本/序号任一项不对 ⇒ 当作"不是本格式的页"（s_seq=0），
 *              但**不擦它** —— 那可能是别人的页；分区不重叠由 §13.1 的断言保证，
 *              这里只是"不信任"而不是"纠正"（⛔ 日志模块无权去抹别的区）。
 */
static void log_scan_page(uint16_t pg)
{
    uint32_t addr = log_page_addr(pg);
    uint16_t hdr[4];
    uint16_t slot, used = 0u, valid = 0u;
    uint8_t  st;

    s_seq[pg]   = 0u;
    s_valid[pg] = 0u;
    s_used[pg]  = 0u;

    stmflash_read(addr, hdr, 4u);

    if (hdr[0] != (uint16_t)APP_LOG_MAGIC || hdr[1] != (uint16_t)APP_LOG_FMT_VER)
    {
        return;
    }

    {
        uint32_t seq = ((uint32_t)hdr[3] << 16) | (uint32_t)hdr[2];

        if (seq == 0u)                        /* 页序号从 1 起，0 = 保留值 ⇒ 不认识 */
        {
            return;
        }

        s_seq[pg] = seq;
    }

    for (slot = 0u; slot < (uint16_t)APP_LOG_PER_PAGE; slot++)
    {
        st = log_slot_state(pg, slot, NULL);

        if (st == SLOT_FREE)
        {
            break;
        }

        used = (uint16_t)(used + 1u);

        if (st == SLOT_VALID)
        {
            valid = (uint16_t)(valid + 1u);
        }
        else if (s_damaged < 0xFFFFu)
        {
            s_damaged = (uint16_t)(s_damaged + 1u);
        }
    }

    s_valid[pg] = valid;
    s_used[pg]  = used;
}

/* ==================== 初始化 ==================== */
void app_log_init(void)
{
    uint16_t pg;
    uint16_t newest = 0u;
    uint8_t  found = 0u;

    s_count     = 0u;
    s_damaged   = 0u;
    s_dropped   = 0u;
    s_q_head    = 0u;
    s_q_tail    = 0u;
    s_q_n       = 0u;
    s_ready     = 1u;
    s_busy_left = 0u;
    s_clear_pg  = LOG_CLEAR_IDLE;
    s_last_poll = 0u;

    for (pg = 0u; pg < (uint16_t)APP_LOG_PAGES; pg++)
    {
        log_scan_page(pg);
    }

    /* 页序号单调 +1 ⇒ 序号最大的那页就是最新页（不依赖物理相邻） */
    for (pg = 0u; pg < (uint16_t)APP_LOG_PAGES; pg++)
    {
        s_count = (uint16_t)(s_count + s_valid[pg]);

        if (s_seq[pg] != 0u && (found == 0u || s_seq[pg] > s_seq[newest]))
        {
            newest = pg;
            found  = 1u;
        }
    }

    if (found == 0u)                          /* 全新区域（或全区页头没写过）：从第 0 页起用 */
    {
        if (log_format_page(0u, 1u) != 0u)
        {
            s_ready = 0u;
        }

        s_wpage    = 0u;
        s_wslot    = 0u;
        s_next_seq = 2u;
    }
    else
    {
        s_wpage    = newest;
        s_wslot    = s_used[newest];
        s_next_seq = (uint32_t)(s_seq[newest] + 1u);

        if (s_wslot >= (uint16_t)APP_LOG_PER_PAGE)
        {
            /* 最新页已写满 ⇒ 写指针推到下一页。那一页的旧记录**当场作废**（它一定会被擦），
             * 所以这里就把计数与索引摘干净，而不是等真擦时再改 —— 否则 `app_log_get()`
             * 会把"即将消失的旧页"排成"仅次于最新页"，顺序是错的，而且随时会少一截。 */
            uint16_t next = (uint16_t)((newest + 1u) % (uint16_t)APP_LOG_PAGES);

            s_count       = (uint16_t)(s_count - s_valid[next]);
            s_seq[next]   = 0u;
            s_valid[next] = 0u;
            s_used[next]  = 0u;

            s_wpage      = next;
            s_wslot      = 0u;
            s_wpage_live = 0u;                /* 第一条落盘时才擦 + 写头（能不擦就不擦） */
        }
        else
        {
            s_wpage_live = 1u;                /* 页头与已写槽都还在 ⇒ 直接往后追加，⛔ 不得擦 */
        }
    }

    if (s_damaged > 0u)                       /* 判据②的可见出口：坏了几条要说出来 */
    {
        printf("[log] %u damaged record(s), count=%u\r\n",
               (unsigned)s_damaged, (unsigned)s_count);
        app_log_add(EV_LOG_CORRUPT, s_damaged, s_wpage, s_wslot);
    }

    printf("[log] ready=%u count=%u/%u page=%u slot=%u seq=%lu\r\n",
           (unsigned)s_ready, (unsigned)s_count, (unsigned)APP_LOG_CAPACITY,
           (unsigned)s_wpage, (unsigned)s_wslot, (unsigned long)s_next_seq);
}

/* ==================== 时间戳 ==================== */
uint32_t app_log_now_unix(void)
{
    /* 唯一换算口在 BSP/RTC：公历 ⇄ Unix 秒不许有第二份实现（审查 C-22 的同类）。
     * 没装 VBAT 时 RTC 每次从编译时刻重新起算 ⇒ 时间戳只能当**相对**参考（app_log.h 已声明），
     * ⛔ 不得改成拿 HAL_GetTick() 凑一个"看起来像"的数。 */
    rtc_get_time();

    return rtc_time2unix(&calendar);
}

/* ==================== 待写队列的临界区（★ 调度器启动前不得用裸的 taskENTER_CRITICAL） ====================
 * FreeRTOS 的 ARM_CM3 端口把 `uxCriticalNesting` 初值设成哨兵 `0xaaaaaaaa`（`port.c:135`），
 * 而 `vPortExitCritical()` 只在计数**归零**时才把 BASEPRI 降回 0。⇒ 在 `vTaskStartScheduler()`
 * 之前跑一对 enter/exit，计数走的是 0xaaaaaaaa → 0xaaaaaaab → 0xaaaaaaaa，永远不归零
 * ⇒ **BASEPRI 被永久留在 0x50**，直到端口自己的 SVC 启动第一个任务时清掉（`port.c:231`）。
 * 后果分两段：
 *   ① 调度器之前：SysTick 优先级 = 15 > `configMAX_SYSCALL_INTERRUPT_PRIORITY` = 5 ⇒ 被屏蔽
 *      ⇒ `HAL_GetTick()` 冻住 ⇒ 任何"按 tick 计数的有界等待"变成无限循环（本项目的实测现场
 *      就是 `app_protect_wdg_start()` 里的 PVU 等待，屏全白、串口断在 `WDG :` 之前 —— `附录C` C-107）；
 *   ② 调度器之后：第一个任务已经切进来了，BASEPRI 被端口清过，所以一切正常
 *      —— 这正是"同一段锁在 14:34 那版能跑、14:41 那版跑不了"的原因：差别不在锁，
 *      而在锁的中毒窗口里**新加了一句依赖 tick 的等待**。
 * ⇒ 这里的判据不是"要不要加锁"而是"现在有没有并发可锁"：调度器没起来 = 单线程，
 *   跳过即可，⛔ 不得为了"看起来统一"而保留一对会造成永久中毒的调用。 */
static void log_lock(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    {
        taskENTER_CRITICAL();
    }
}

static void log_unlock(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    {
        taskEXIT_CRITICAL();
    }
}

/* ==================== 生产者 ==================== */
uint8_t app_log_add(uint16_t event, uint16_t code, uint16_t aux_a, uint16_t aux_b)
{
    app_log_rec_t rec;
    uint8_t       dropped = 0u;

    rec.ts_unix = app_log_now_unix();
    rec.event   = event;
    rec.code    = code;
    rec.aux_a   = aux_a;
    rec.aux_b   = aux_b;

    log_lock();                     /* 多个任务都会投事件 ⇒ 队列是共享的 */

    if (s_ready == 0u)
    {
        s_dropped = (uint16_t)(s_dropped + 1u);
        log_unlock();

        return 1u;
    }

    if (s_q_n >= (uint8_t)APP_LOG_QUEUE_DEPTH)
    {
        s_q_tail = (uint8_t)((s_q_tail + 1u) % (uint8_t)APP_LOG_QUEUE_DEPTH);  /* 丢最旧（纪律①） */
        s_q_n--;
        dropped   = 1u;
        s_dropped = (uint16_t)(s_dropped + 1u);
    }

    s_q[s_q_head] = rec;
    s_q_head      = (uint8_t)((s_q_head + 1u) % (uint8_t)APP_LOG_QUEUE_DEPTH);
    s_q_n++;

    log_unlock();

    return dropped;
}

/* ==================== 消费者（D5 起 = TaskLog） ==================== */
/**
 * @brief       落一条（内部）：一次最多写一条、最多滚一页
 * @note        调用方（`app_log_poll`）已限流 ⇒ 本函数就是"每拍最多一件擦写类动作"的那一件。
 */
static void log_write_one(const app_log_rec_t *rec)
{
    uint16_t hw[SLOT_HW_NUM];
    uint16_t chk[SLOT_HW_NUM];
    uint16_t pg   = s_wpage;
    uint16_t slot = s_wslot;

    /* 页满 ⇒ 滚到下一页并作废其内容。一次只滚一页，⛔ 不得在循环里连擦几页 */
    if (slot >= (uint16_t)APP_LOG_PER_PAGE)
    {
        uint16_t next = (uint16_t)((pg + 1u) % (uint16_t)APP_LOG_PAGES);

        s_count       = (uint16_t)(s_count - s_valid[next]);  /* 环形覆盖：最旧那 127 条让位 */
        s_seq[next]   = 0u;
        s_valid[next] = 0u;
        s_used[next]  = 0u;

        pg           = next;
        slot         = 0u;
        s_wpage      = next;
        s_wslot      = 0u;
        s_wpage_live = 0u;
    }

    if (s_wpage_live == 0u)
    {
        if (log_format_page(pg, s_next_seq) != 0u)
        {
            s_ready   = 0u;                   /* 连页头都写不进去 ⇒ 别再往这区里扔数据 */
            s_dropped = (uint16_t)(s_dropped + 1u);
            printf("[log] DISABLED (format page %u failed)\r\n", (unsigned)pg);
            return;
        }

        s_next_seq = (uint32_t)(s_next_seq + 1u);
    }

    if (log_slot_is_blank(pg, slot) == 0u)    /* 整页回写护栏 */
    {
        s_damaged  = (uint16_t)(s_damaged + 1u);
        s_dropped  = (uint16_t)(s_dropped + 1u);
        s_wslot    = (uint16_t)(slot + 1u);   /* 让开这个槽，下次不再撞它 */
        s_used[pg] = s_wslot;

        printf("[log] slot %u/%u not blank, skipped\r\n", (unsigned)pg, (unsigned)slot);
        return;
    }

    hw[SLOT_HW_TS_LO]  = (uint16_t)(rec->ts_unix & 0xFFFFu);
    hw[SLOT_HW_TS_HI]  = (uint16_t)(rec->ts_unix >> 16);
    hw[SLOT_HW_EVENT]  = rec->event;
    hw[SLOT_HW_CODE]   = rec->code;
    hw[SLOT_HW_AUX_A]  = rec->aux_a;
    hw[SLOT_HW_AUX_B]  = rec->aux_b;
    hw[SLOT_HW_CRC]    = log_crc16(hw, SLOT_HW_DATA_N);
    hw[SLOT_HW_COMMIT] = COMMIT_YES;

    /* ★ 顺序 = 先数据 + CRC，提交位单独最后写（文件头"唯一错了不报错"的那条） */
    stmflash_write(log_slot_addr(pg, slot), hw, (uint16_t)(SLOT_HW_DATA_N + 1u));
    stmflash_write(log_slot_addr(pg, slot) + ((uint32_t)SLOT_HW_COMMIT * 2u),
                   &hw[SLOT_HW_COMMIT], 1u);

    s_busy_left = APP_LOG_BUSY_HOLD_POLLS;

    log_read_slot(pg, slot, chk);

    if (memcmp(chk, hw, sizeof(chk)) != 0)    /* 读回不符 ⇒ 这条算坏，⛔ 不计入有效 */
    {
        s_damaged  = (uint16_t)(s_damaged + 1u);
        s_wslot    = (uint16_t)(slot + 1u);
        s_used[pg] = s_wslot;

        printf("[log] slot %u/%u verify FAILED\r\n", (unsigned)pg, (unsigned)slot);
        return;
    }

    s_used[pg]  = (uint16_t)(slot + 1u);
    s_valid[pg] = (uint16_t)(s_valid[pg] + 1u);
    s_wslot     = (uint16_t)(slot + 1u);

    if (s_count < (uint16_t)APP_LOG_CAPACITY)
    {
        s_count++;
    }

    printf("[log] #%u saved  ev=0x%04X code=0x%04X  page=%u slot=%u\r\n",
           (unsigned)s_count, (unsigned)rec->event, (unsigned)rec->code,
           (unsigned)pg, (unsigned)slot);
}

/**
 * @brief       清空的其中一步（一拍一页）
 * @note        ⛔ 不在 `app_log_clear()` 里连擦 28 页：28 × 40ms ≈ 1.1s 的 CPU 全停，
 *              虽然仍小于 2s 的 IWDG，但会把判活余量整个吃掉（§2.3「看门狗」行）。
 *              分页后每拍最多停 40ms，其余拍子照常喂狗。清完由第 0 页重新起序号 1。
 */
static void log_clear_step(void)
{
    if (s_clear_pg >= (uint16_t)APP_LOG_PAGES)
    {
        return;
    }

    s_busy_left = APP_LOG_BUSY_HOLD_POLLS;

    if (stmflash_page_is_blank(log_page_addr(s_clear_pg)) == 0u)
    {
        if (stmflash_erase_page(log_page_addr(s_clear_pg)) != 0u)
        {
            printf("[log] clear: page %u erase FAILED\r\n", (unsigned)s_clear_pg);
        }
    }

    s_seq[s_clear_pg]   = 0u;
    s_valid[s_clear_pg] = 0u;
    s_used[s_clear_pg]  = 0u;

    s_clear_pg = (uint16_t)(s_clear_pg + 1u);

    if (s_clear_pg >= (uint16_t)APP_LOG_PAGES)
    {
        s_clear_pg = LOG_CLEAR_IDLE;

        s_count    = 0u;
        s_damaged  = 0u;
        s_wslot    = 0u;
        s_next_seq = 2u;

        if (log_format_page(0u, 1u) != 0u)
        {
            s_ready = 0u;
        }

        s_wpage = 0u;

        printf("[log] cleared\r\n");
    }
}

void app_log_poll(uint32_t now_ms)
{
    app_log_rec_t rec;
    uint8_t       have = 0u;

    /* 每拍都推进的两件事（与限流无关）：空窗窗口递减 + 把它报进数据模型。
     * 报出去的是"本模块当前是否处在擦写窗口"这一个**输入**，
     * ⛔ 不是往告警帧里写东西 —— 告警帧只有一个写者（附录C · C-97 F1）。 */
    if (s_busy_left > 0u)
    {
        s_busy_left--;
    }

    app_data_flash_busy((uint8_t)((s_busy_left > 0u || s_clear_pg != LOG_CLEAR_IDLE) ? 1u : 0u));

    if (s_ready == 0u)
    {
        return;
    }

    if ((uint32_t)(now_ms - s_last_poll) < (uint32_t)APP_PERIOD_LOG_MS)
    {
        return;                               /* 限流：一拍最多一件擦写类动作 */
    }

    s_last_poll = now_ms;

    if (s_clear_pg != LOG_CLEAR_IDLE)
    {
        log_clear_step();
        return;
    }

    log_lock();

    if (s_q_n > 0u)
    {
        rec      = s_q[s_q_tail];
        s_q_tail = (uint8_t)((s_q_tail + 1u) % (uint8_t)APP_LOG_QUEUE_DEPTH);
        s_q_n--;
        have     = 1u;
    }

    log_unlock();

    if (have == 0u)
    {
        return;
    }

    log_write_one(&rec);
}

/* ==================== 只读出口 ==================== */
uint16_t app_log_count(void)
{
    return s_count;                           /* 已落盘的有效条数（队列里没落的不算） */
}

uint16_t app_log_damaged(void)
{
    return s_damaged;
}

uint16_t app_log_dropped(void)
{
    return s_dropped;
}

uint8_t app_log_pending(void)
{
    uint8_t n;

    log_lock();
    n = s_q_n;
    log_unlock();

    return n;
}

/**
 * @brief       按"从新到旧"取一条
 * @note        遍历顺序 = 从写页起按环形往前逐页回退（滚动永远是 +1 ⇒ 物理回退即序号回退）。
 *              每取一条都要重读槽头 ⇒ 界面只翻 5 行时开销很小；全量 `app_log_dump()` 才走满。
 */
uint8_t app_log_get(uint16_t idx, app_log_rec_t *out)
{
    uint16_t pg, slot, n = 0u;
    uint8_t  k;

    if (out == NULL || idx >= s_count)
    {
        return 0u;
    }

    pg = s_wpage;

    for (k = 0u; k < (uint8_t)APP_LOG_PAGES; k++)
    {
        for (slot = s_used[pg]; slot > 0u; slot--)
        {
            if (log_slot_state(pg, (uint16_t)(slot - 1u), out) == SLOT_VALID)
            {
                if (n == idx)
                {
                    return 1u;
                }

                n++;
            }
        }

        pg = (uint16_t)((pg + (uint16_t)APP_LOG_PAGES - 1u) % (uint16_t)APP_LOG_PAGES);
    }

    return 0u;
}

void app_log_clear(void)
{
    if (s_ready == 0u)
    {
        return;
    }

    s_clear_pg  = 0u;
    s_busy_left = APP_LOG_BUSY_HOLD_POLLS;    /* 从"决定清"的那一刻起就挂上空窗标志 */
}

/* ==================== 事件码 → 文本 ==================== */
/* ★ ASCII 短标签：LCD 一行只有 20 字符，页 3 还要放时间与原因码。中文名留给上位机查这张表。 */
static const uint16_t s_ev_code[] =
{
    EV_BOOT, EV_WDG_RESET,
    EV_ENV_ALARM, EV_ENV_CLEAR,
    EV_ALARM_L1, EV_ALARM_L2, EV_ALARM_L3,
    EV_MOS_CLOSE, EV_MOS_OPEN, EV_PRECHARGE_START, EV_PRECHARGE_FAIL,
    EV_LATCH, EV_LATCH_CLEAR,
    EV_LINK_STALE, EV_LINK_RECOVER, EV_TASK_STALLED,
    EV_AFE_COMM_FAIL, EV_HW_PROTECT, EV_LOG_CORRUPT
};

static const char *const s_ev_name[] =
{
    "BOOT",     "WDG-RST",
    "ENV-ALM",  "ENV-OK",
    "ALM-L1",   "ALM-L2",   "ALM-L3",
    "MOS-CLS",  "MOS-OPN",  "PRE-STA",  "PRE-FAIL",
    "LATCH",    "UNLATCH",
    "LINK-BAD", "LINK-OK",  "TASK-BAD",
    "AFE-ERR",  "HW-PROT",  "LOG-DAMG"
};

/* ★ 两张表**必须同序同长**：下面这条断言是唯一的看门人（少了名字只会打印错位，不会报错）。 */
APP_STATIC_ASSERT(sizeof(s_ev_code) / sizeof(s_ev_code[0]) == sizeof(s_ev_name) / sizeof(s_ev_name[0]),
                  ev_name_table_out_of_sync);

#define EV_TABLE_N   (sizeof(s_ev_code) / sizeof(s_ev_code[0]))

void app_log_event_str(uint16_t event, char *dst, uint16_t dstsz)
{
    uint16_t i;

    if (dst == NULL || dstsz == 0u)
    {
        return;
    }

    for (i = 0u; i < (uint16_t)EV_TABLE_N; i++)
    {
        if (s_ev_code[i] == event)
        {
            snprintf(dst, dstsz, "%s", s_ev_name[i]);
            return;
        }
    }

    snprintf(dst, dstsz, "EV-%04X", (unsigned)event);  /* 未知码原样打出来，⛔ 不得显示成空白 */
}

void app_log_ts_str(uint32_t ts_unix, char *dst, uint16_t dstsz)
{
    _calendar_obj t;

    if (dst == NULL || dstsz == 0u)
    {
        return;
    }

    if (ts_unix == 0u)                        /* 0 = 当时 RTC 没给出时间，照实标"无时间" */
    {
        snprintf(dst, dstsz, "-- -- --:--");
        return;
    }

    rtc_unix2time(ts_unix, &t);

    snprintf(dst, dstsz, "%02u-%02u %02u:%02u",
             (unsigned)t.month, (unsigned)t.date, (unsigned)t.hour, (unsigned)t.min);
}

/* ==================== 调试出口 ==================== */
void app_log_dump(void)
{
    app_log_rec_t rec;
    char          ev[12];
    char          ts[16];
    uint16_t      i;

    printf("--- BMS event log: %u/%u kept, %u damaged, %u dropped, %u pending ---\r\n",
           (unsigned)s_count, (unsigned)APP_LOG_CAPACITY,
           (unsigned)s_damaged, (unsigned)s_dropped, (unsigned)app_log_pending());

    if (s_count == 0u)
    {
        printf("(empty)\r\n");
        return;
    }

    for (i = 0u; i < s_count; i++)
    {
        if (app_log_get(i, &rec) == 0u)
        {
            printf("  #%-3u <unreadable>\r\n", (unsigned)i);
            continue;
        }

        app_log_event_str(rec.event, ev, sizeof(ev));
        app_log_ts_str(rec.ts_unix, ts, sizeof(ts));

        printf("  #%-3u %s  %-9s ev=0x%04X code=0x%04X a=%u b=%u\r\n",
               (unsigned)i, ts, ev, (unsigned)rec.event, (unsigned)rec.code,
               (unsigned)rec.aux_a, (unsigned)rec.aux_b);
    }
}
