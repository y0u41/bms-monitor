/**
 ****************************************************************************************************
 * @file        app_data.c
 * @brief       BMS 快照的双缓冲发布与一致性读取（`Tech_Design.md` §4.4 的实现）
 * @note        ★ 为什么是"两个写者各一条双缓冲"，而不是一条共享快照：
 *                raw+derived 由 TaskAcq(100ms) 写，alarm 由 TaskProtect(10ms) 写。
 *                若两者共用同一个待填缓冲，优先级更高的 TaskProtect 可以在 TaskAcq
 *                填到一半时抢占并写同一块 ⇒ 发布出去的就是撕裂帧。
 *                拆成"单写者 · 一对缓冲"后**根本不存在争用**，也就不需要互斥量 ——
 *                而用互斥量正是 §4.4 明确禁止的形态：保护任务去等采集任务 = 保护路径被阻塞。
 *
 * @note        读取侧：先读序号 → 整块拷贝 → 再读序号，序号变了就重试。
 *                拷贝期间发生交换时读者会察觉并重来，因此永远不会拿到半帧。
 *                代价：一次读 ≈ 150 字节拷贝（72MHz 下数微秒），且读者不阻塞写者。
 *
 * @note        ⛔ 本文件不得出现任何保护判据、门限比较或 AFE 寄存器名（§4.2 职责 / S15）。
 ****************************************************************************************************
 */

#include "./App/app_data.h"
#include "./App/app_log.h"            /* 链路位变化 ⇒ 投事件（判据与可见出口同处，见 link_check） */
#include <string.h>

/* 一个"单写者双缓冲盒"：两份数据 + 当前对外可见的编号 + 发布序号 */
typedef struct
{
    bms_raw_t         raw[2];
    bms_derived_t     der[2];
    volatile uint8_t  idx;        /* 对外可见的那一份；写侧填 idx^1 */
    volatile uint16_t seq;        /* 每次发布 +1（回绕无所谓，只用"变没变"） */
    volatile uint8_t  published;  /* 0 = 从未发布过 */
} raw_box_t;

typedef struct
{
    bms_alarm_t       alarm[2];
    volatile uint8_t  idx;
    volatile uint16_t seq;
    volatile uint8_t  published;
} alarm_box_t;

/* 全静态分配（AGENTS §2.3「内存」行：禁止 malloc / pvPortMalloc） */
static raw_box_t   g_raw_box;
static alarm_box_t g_alarm_box;

/* ==================== "从未采集"的统一表达 ==================== */
static void raw_mark_invalid(bms_raw_t *r)
{
    uint8_t i;

    memset(r, 0, sizeof(*r));

    for (i = 0; i < (uint8_t)APP_CELL_COUNT_MAX; i++)
    {
        r->cell_mv[i] = APP_MV_INVALID;     /* ★ 无效是 0xFFFF 不是 0 —— 0 可能是"电芯被短接"的合法读数 */
    }
}

static void der_mark_invalid(bms_derived_t *d)
{
    memset(d, 0, sizeof(*d));

    /* SOC 未知 = 0：宁可显示"没电"也不冒充"还有电"，因为界面下游会据此决定要不要停充。
     * SOH 在没有实测容量前按 100% 算（⚠️ 待 D6 用一次完整放电替换，见 §7.2）。 */
    d->soh_x10 = 1000u;
    d->chem    = (chem_type_t)APP_BMS_CHEM;
}

static void alarm_mark_invalid(bms_alarm_t *a)
{
    memset(a, 0, sizeof(*a));

    /* ★ 上电瞬间四条关键通道都还没有数据 → 全部标不新鲜，"不可知即不安全"落到初值上。
     *   注意这里置的是"链路/通道"位，不是 BMS_LINK_FLASH_BUSY —— 擦写空窗由 app_log/app_cfg 报。 */
    a->link_health    = (uint8_t)(BMS_LINK_AFE | BMS_LINK_INA226 | BMS_LINK_NTC | BMS_LINK_VSW);
    a->derate_chg_pct = 100u;
    a->derate_dis_pct = 100u;
}

void app_data_init(void)
{
    uint8_t k;

    for (k = 0u; k < 2u; k++)
    {
        raw_mark_invalid(&g_raw_box.raw[k]);
        der_mark_invalid(&g_raw_box.der[k]);
        alarm_mark_invalid(&g_alarm_box.alarm[k]);
    }

    g_raw_box.idx        = 0u;
    g_raw_box.seq        = 0u;
    g_raw_box.published  = 0u;
    g_alarm_box.idx      = 0u;
    g_alarm_box.seq      = 0u;
    g_alarm_box.published = 0u;
}

/* ==================== 写者接口 ==================== */
/* 返回"上一帧的完整副本"：写者只改自己负责的字段，其余字段自然延续，
 * 不需要每条目都重写一遍（也就不会漏写导致某字段被清零）。 */
bms_raw_t *app_data_raw_write(void)
{
    uint8_t back = (uint8_t)(g_raw_box.idx ^ 1u);

    memcpy(&g_raw_box.raw[back], &g_raw_box.raw[g_raw_box.idx], sizeof(bms_raw_t));
    return &g_raw_box.raw[back];
}

bms_derived_t *app_data_der_write(void)
{
    uint8_t back = (uint8_t)(g_raw_box.idx ^ 1u);

    memcpy(&g_raw_box.der[back], &g_raw_box.der[g_raw_box.idx], sizeof(bms_derived_t));
    return &g_raw_box.der[back];
}

void app_data_raw_commit(void)
{
    /* ★ 交换即发布：单条字节写，M3 上天然原子，不需要关中断。
     *   序号写在后面，读者据此发现"我刚才那份拷贝期间有人发布过 → 重来"。 */
    g_raw_box.idx = (uint8_t)(g_raw_box.idx ^ 1u);
    __DSB();
    g_raw_box.seq = (uint16_t)(g_raw_box.seq + 1u);
    g_raw_box.published = 1u;
}

bms_alarm_t *app_data_alarm_write(void)
{
    uint8_t back = (uint8_t)(g_alarm_box.idx ^ 1u);

    memcpy(&g_alarm_box.alarm[back], &g_alarm_box.alarm[g_alarm_box.idx], sizeof(bms_alarm_t));
    return &g_alarm_box.alarm[back];
}

void app_data_alarm_commit(void)
{
    g_alarm_box.idx = (uint8_t)(g_alarm_box.idx ^ 1u);
    __DSB();
    g_alarm_box.seq = (uint16_t)(g_alarm_box.seq + 1u);
    g_alarm_box.published = 1u;
}

/* ==================== 读者接口 ==================== */
/* 两个盒的序号之和 = "这帧快照的新旧指纹"：任一盒发布过，指纹就变。 */
static uint16_t snap_seq(void)
{
    uint16_t s;

    s = g_raw_box.seq;
    __DSB();                            /* 取号与随后的拷贝之间不得被编译器换序 */
    s = (uint16_t)(s + g_alarm_box.seq);
    return s;
}

uint8_t app_data_read(bms_snapshot_t *out)
{
    uint8_t  retry;
    uint16_t s0;
    uint16_t s1;

    if (out == NULL)
    {
        return 1;
    }

    for (retry = 0u; retry < 4u; retry++)
    {
        s0 = snap_seq();
        __DSB();

        memcpy(&out->raw,   &g_raw_box.raw[g_raw_box.idx],       sizeof(bms_raw_t));
        memcpy(&out->der,   &g_raw_box.der[g_raw_box.idx],       sizeof(bms_derived_t));
        memcpy(&out->alarm, &g_alarm_box.alarm[g_alarm_box.idx], sizeof(bms_alarm_t));

        __DSB();
        s1 = snap_seq();

        if (s0 == s1)
        {
            return (uint8_t)((g_raw_box.published != 0u) ? 0u : 1u);
        }
    }

    /* 连续 4 次都被打断 ⇒ 返回"这一帧不可信"，⛔ 不得把可能撕裂的内容交给判据使用。
     * 实际上下一次 commit 之前最多只可能被打断一次，走到这里说明写者正在超速发布。 */
    return 2;
}

uint16_t app_data_seq(void)
{
    return g_raw_box.seq;
}

/* ==================== D2 假数据源 ==================== */
/* ⏳ 交接点：TaskAcq 从 D3 起逐通道写真实数据（AFE 组压/单体 → INA226 电流 → NTC → SHT30 → V_SW）。
 *   真实帧一 commit 就整帧覆盖这里的值，所以假数据不会掩盖真实故障；全部通道接入后
 *   删除本函数与 app_main.c 中那一次调用（Build.md D2 的验收口径本来就是"数据先填假值"）。
 *
 *   取值刻意落在 LFP 正常区内并随秒缓慢漂移，用来验证三件事：
 *     ① 单位换算与小数点（10mV / 10mA / 0.1℃ / 0.1% 四种量纲同屏）；
 *     ② 无效串的显示（第 5~16 槽保持 APP_MV_INVALID，界面必须显示 "----"）；
 *     ③ 压差/最高/最低的取数只遍历 APP_CELL_COUNT，⛔ 不得把无效槽算进最大值。 */
void app_data_fake_step(uint32_t now_ms)
{
    bms_raw_t     *r = app_data_raw_write();
    bms_derived_t *d = app_data_der_write();
    uint32_t       sec = now_ms / 1000u;
    uint8_t        i;

    r->cell_valid = 0u;

    for (i = 0u; i < (uint8_t)APP_CELL_COUNT_MAX; i++)
    {
        uint16_t mv;

        if (i >= (uint8_t)APP_CELL_COUNT)
        {
            r->cell_mv[i] = APP_MV_INVALID;     /* ★ 未接的槽保持"无效"，不是 0、也不是 3300 */
            continue;
        }

        mv = (uint16_t)(3300u + (i * 3u) + (uint16_t)((sec + i) % 7u));

        if (i == 2u)
        {
            mv = (uint16_t)(mv + 24u);          /* 第 3 串故意偏高，让"压差 + 最高串号"有东西可显示 */
        }

        r->cell_mv[i]  = mv;
        r->cell_valid |= (uint16_t)(1u << i);
    }

    r->pack_10mv     = (uint16_t)(1320u + (sec % 20u));     /* 13.20 ~ 13.39 V（10mV 单位） */
    r->pack_10mv_aux = (uint16_t)(r->pack_10mv - 1u);       /* 冗余通道差 1 个字，验证两路都在显示 */

    /* -5.00A ~ +2.00A 缓慢过零：10mA 单位、充正放负，负值必须带符号显示 */
    r->curr_10ma  = (int16_t)(((int32_t)(sec % 700u)) - 500);
    r->curr_valid = 1u;

    /* ★ 温度两个测点**都不**由本函数写（审查 C-98）：
     *   - 环境通道 `APP_TEMP_IDX_ENV` 的属主是 `app_acq`（D3 已接）；
     *   - 电芯表面 NTC 的属主是 D11 的 PA5，今天板上没有这个器件。
     *   原来这里假造 25.3~27.2℃ 并置 `temp_valid` 位，而 `Link` 行同时亮着 `NTC` ⇒
     *   界面自相矛盾：一个**编出来的数**顶着"该链路失效"的判据往下游走（温差/最高最低/禁充电判据）。
     *   本函数拿到的 `r` 是"上一已发布帧的副本"，不写 = 原样延续，真实通道天然覆盖假通道。 */
    /* ★ 2026-09-28（D6 软件侧前置）：**V_SW 这一路不再假造**，与温度两测点同批处理（C-98 的先例）。
     *   原来这里是
     *       r->v_sw_10mv = r->pack_10mv;   r->v_sw_valid = 1u;
     *   三条独立缺陷叠在同一行上，而且**没有一条会报错**：
     *     ① 注释与物理相反：主开关**闭合**时两端压差 ≈ 0，不是"≈组压"；假造出来的值表达的是
     *        "开关断开"，而 `v_sw_valid = 1` 又宣称它可信 ⇒ 下游拿到的是一个方向都反了的数；
     *     ② `BMS_LINK_VSW` 因此**永不亮**（判据 = `v_sw_valid == 0`），而 §5.2 失效安全表里
     *        "`V_SW` 通道失效 → 禁止闭合"整条判据挂在它身上 ⇒ C-97 F3 的同族、C-110 纪律 58
     *        说的"增量=0 型判据必须有存在性正证"，这里连正证的输入都被假值占掉了；
     *     ③ 状态机接上之后它**必然**在判据②上失败（假值 = 组压 ≫ `APP_CTR_PRECHARGE_DV_MV`），
     *        于是每次上电都是"预充重试 3 次 → 进锁定"，⚠️ 而且原因是一个不存在的缺陷 ——
     *        这类"假数据把一个正常现象伪装成真故障"的形态，比缺数据难查一个量级。
     *   ⇒ 正确表达 = **不写**：`app_data_init()` 给的"从未采集"初值 + `link_check` 置 `VSW` 位
     *      + 界面显示 `----` = "板上还没有这个测点"，与 NTC 的处理完全同形。 */

    r->tick_ms      = now_ms;
    r->tick_ms_cell = now_ms;
    r->tick_ms_curr = now_ms;
    /* ★ 不写 `tick_ms_temp[]` 的任何一格：每测点的时间戳只由**它的真实属主**在读到一帧时推进。
     *   假数据源每 500ms 把它刷新一次 = 永远"新鲜" = `BMS_LINK_NTC` / `BMS_LINK_SHT30` 永远不会亮，
     *   而这两一位正是"NTC 失效 → 禁止充电"与"环境温度读不到就别信温度保护"的判据（§4.4 新鲜度规则）。
     * ⚠️ 剩下的同类债（`附录C` C-97 F3，已排 D7/D10）：`tick_ms_cell` / `tick_ms_curr` 今天仍由本函数
     *   每拍刷新 ⇒ `BMS_LINK_AFE` / `BMS_LINK_INA226` 永不亮。真实属主接入时**值与时间戳同批删**。 */

    d->soc_x10 = (uint16_t)(856u - (sec % 30u));            /* 85.6% 起缓慢下滑 */
    d->soh_x10 = 982u;
    d->chg_mah = 12000u + sec;
    d->dis_mah = 9000u + (sec / 2u);
    d->cycles  = 12u;

    d->cell_max_mv = 0u;
    d->cell_min_mv = APP_MV_INVALID;

    for (i = 0u; i < (uint8_t)APP_CELL_COUNT; i++)
    {
        if ((r->cell_valid & (uint16_t)(1u << i)) == 0u)
        {
            continue;                                       /* ★ 不可信的串不参与极值 */
        }

        if (r->cell_mv[i] > d->cell_max_mv)
        {
            d->cell_max_mv = r->cell_mv[i];
        }

        if (r->cell_mv[i] < d->cell_min_mv)
        {
            d->cell_min_mv = r->cell_mv[i];
        }
    }

    d->delta_mv = (uint16_t)(d->cell_max_mv - d->cell_min_mv);

    /* 温度极值：⛔ 不得把"没采到"的测点算进来 —— 未写入的槽是初值 0，
     * 直接取 [0]/[1] 会得到"最低 0.0℃"这条**假欠温**判据（§4.4：0 不是无效值，0xFFFF 才是，
     * 而温度没有无效哨兵 ⇒ 唯一可信的依据就是 temp_valid 位图）。 */
    {
        int16_t tmax = 0;
        int16_t tmin = 0;
        uint8_t found = 0u;

        for (i = 0u; i < (uint8_t)APP_BMS_TEMP_ACTIVE_COUNT; i++)
        {
            if ((r->temp_valid & (uint16_t)(1u << i)) == 0u)
            {
                continue;
            }

            if (found == 0u)
            {
                tmax = r->temp_x10[i];
                tmin = r->temp_x10[i];
                found = 1u;
            }
            else
            {
                if (r->temp_x10[i] > tmax)
                {
                    tmax = r->temp_x10[i];
                }

                if (r->temp_x10[i] < tmin)
                {
                    tmin = r->temp_x10[i];
                }
            }
        }

        if (found != 0u)
        {
            d->temp_max_x10   = tmax;
            d->temp_min_x10   = tmin;
            d->temp_delta_x10 = (int16_t)(tmax - tmin);
        }
    }

    d->temp_rise_x10_per_min = 2;                           /* 0.2℃/min */

    app_data_raw_commit();

    /* ⛔ 原来这里最后一句是 `app_data_link_check(now_ms)` —— **D5 §8.4 M6 删掉了**，
     *   告警帧的写者身份交给 `app_protect_step()`（TaskProtect）。
     *   ★ 为什么必须删（`附录C` C-97 F1）：告警帧与 raw 盒共用同一套双缓冲机制，
     *   两个写者各自 `app_data_alarm_write()` + commit 会互相覆盖 half-written 帧，
     *   读者只能靠 `app_data_read()` 返回 2 发现 = 现场看是"偶发乱码"，查不到根因。
     *   判据（可机械检查）：全工程 `app_data_link_check()` 的调用点恰好 1 处，
     *   且在 `app_protect.c`。⛔ 不要因为"假源看起来什么都不写了"而把它加回来 ——
     *   那正是本函数在 D7~D11 被逐通道搬空后的**预期状态**。 */
}

/* ==================== 链路新鲜度（§4.4 判据处，D4 从假源尾部提取） ==================== */
/* `app_log` 报进来的"Flash 正在擦写"输入：⛔ 本模块不据此直接改告警帧（那会多出第二个写者），
 * 只存这一个 bool，由 `app_data_link_check()` 在下次发布时读走。 */
static uint8_t s_flash_busy_in;
static uint8_t s_link_prev;                   /* 上一次发布出去的 link_health */
static uint8_t s_link_prev_valid;             /* 0 = 上电第一次判 ⇒ 不投事件（否则开机就一条"恢复"） */

void app_data_flash_busy(uint8_t busy)
{
    s_flash_busy_in = (uint8_t)(busy != 0u ? 1u : 0u);
}

void app_data_link_check(uint32_t now_ms)
{
    const bms_raw_t *r = &g_raw_box.raw[g_raw_box.idx];     /* 已发布的最新帧，只读 */
    bms_alarm_t     *a;
    uint8_t          i;
    uint8_t          went_bad;
    uint8_t          came_back;
    uint8_t          mask;
    uint16_t         aux_a;
    uint16_t         aux_b;

    a = app_data_alarm_write();

    /* ★ 只算**本函数判得动**的那几位（审查 C-98）：`VSW` 的属主是 D11 的开关两端压差采样，
     *   原来这里写 `= 0` 会把上电初值里那条"不可知"一起抹掉 = 假源把"没有观测"报成"链路正常"，
     *   与 §4 自检项"不可知即不安全"相反。
     *   `FLASH_BUSY` 从"延续上一帧"改成"照输入重算"—— 它有属主了（app_log），延续旧值会让
     *   擦写结束之后那一位还挂着，等于永久宣称"保护空窗"，和永不亮一样错（方向相反）。 */
    a->link_health = (uint8_t)(((r->v_sw_valid != 0u) ? 0u : BMS_LINK_VSW)
                               | ((s_flash_busy_in != 0u) ? BMS_LINK_FLASH_BUSY : 0u));

    if (acq_is_fresh(r->tick_ms_cell, now_ms) == 0u)
    {
        a->link_health |= BMS_LINK_AFE;
    }

    if (acq_is_fresh(r->tick_ms_curr, now_ms) == 0u)
    {
        a->link_health |= BMS_LINK_INA226;
    }

    /* ★ 逐测点各判各的（原 F2 的修法）：一条时间戳两个属主时，"没在更新的那个"会借别人的心跳
     *   冒充新鲜 ⇒ `BMS_LINK_NTC` 永不亮，而"NTC 失效 → 禁止充电"整条判据挂在它身上。
     *   测点 → 链路位的映射：环境测点归 SHT30，其余（电芯表面）归 NTC ——
     *   D11 加第 2 个电芯测点时它自动落到 NTC 侧，本段不用再改。 */
    for (i = 0u; i < (uint8_t)APP_BMS_TEMP_ACTIVE_COUNT; i++)
    {
        if (acq_is_fresh(r->tick_ms_temp[i], now_ms) == 0u)
        {
            a->link_health |= (uint8_t)((i == (uint8_t)APP_TEMP_IDX_ENV) ? BMS_LINK_SHT30
                                                                        : BMS_LINK_NTC);
        }
    }

    /* ★ 位变化 ⇒ 投一条链路事件（`Build.md` D4 判据③：停采 5s 要在日志里看得见，不只是屏幕上闪一下）。
     *   判据与它的可见出口**必须同处**：拆成两处早晚漂移（判的那处不知道报的那处在报什么）。 */
    if (s_link_prev_valid != 0u)
    {
        /* 掩掉 FLASH_BUSY：这一位由日志模块自己产生。让它触发事件就成了
         * "写日志 ⇒ 置空窗 ⇒ 投一条日志 ⇒ 又写日志"的自激（每 50ms 一条，几秒就滚掉一整页）。 */
        mask      = (uint8_t)(~(uint8_t)BMS_LINK_FLASH_BUSY);
        went_bad  = (uint8_t)(a->link_health & (uint8_t)~s_link_prev & mask);
        came_back = (uint8_t)(s_link_prev & (uint8_t)~a->link_health & mask);

        aux_a = r->pack_10mv;                             /* 事件发生时的组压/电流，供事后复盘 */
        aux_b = (uint16_t)r->curr_10ma;

        if (went_bad != 0u)
        {
            app_log_add(EV_LINK_STALE, went_bad, aux_a, aux_b);
        }

        if (came_back != 0u)
        {
            app_log_add(EV_LINK_RECOVER, came_back, aux_a, aux_b);
        }
    }

    s_link_prev       = a->link_health;
    s_link_prev_valid = 1u;

    app_data_alarm_commit();
}
