/**
 ****************************************************************************************************
 * @file        app_soc.c
 * @brief       派生层实现（规格见 `app_soc.h` 与 `Tech_Design.md` §4.4／`PRD.md` §2.2）
 * @note        ★★ 本文件是 `bms_derived_t` 的**唯一写者**。⛔ 不写 raw 盒、不碰告警/执行器/通信/Flash。
 *              ★ 纯整数实现：安时积分用"累加器 + 分母"的精确整数法（⛔ 不用浮点 —— "允许"不等于"必须"）。
 ****************************************************************************************************
 */

#include "./App/app_calib.h"      /* ★ OCV-SOC 表来自标定表区（§8 协议 (5) 的读侧） */
#include "./App/app_soc.h"
#include "./App/app_config.h"
#include <string.h>

/* ★ 积分分母的推导（量纲必须写出来，否则下一个人只能猜）：
 *   ΔSOC(0.1%) = 1000 × Δcharge_mAh / capacity_mAh
 *   Δcharge_mAh = curr_10ma × dt_ms / 360000      （10mA × ms → mAh）
 *   ⇒ ΔSOC(0.1%) = curr_10ma × dt_ms / (360 × capacity_mAh)
 *   ⇒ 分母 SOC_DEN = 360 × APP_NOM_CAPACITY_MAH（6000 ⇒ 2,160,000，装在 int32 里绰绰有余） */
#define SOC_DEN         ((int32_t)(360u * APP_NOM_CAPACITY_MAH))
/** mAh = 10mA·ms / 360000（同一个推导里的一步） */
#define CHARGE_TO_MAH   (360000u)

/** 进度计满的边界（SOC 在 [0,1000] = 0~100.0%） */
#define SOC_FULL_X10    1000

static int32_t  s_acc;              /* 积分余数累加器（10mA·ms） */
static int32_t  s_soc_x10;          /* 0.1%；⛔ 只有 s_anchored 为真时才当数值用 */
static uint8_t  s_anchored;         /* 是否已有一个可信锚点（FR-SOC-02 缺口 a） */
static uint8_t  s_trust;            /* 对外可信标志（= s_anchored && 电流通道有效 && 无断档） */
static uint8_t  s_have_last;        /* 是否已有上一拍时间戳（首拍不积分） */
static uint32_t s_last_ms;

/* 温升速率：**两点差商**（只留窗口外的那个老点；⛔ 不存环形缓冲——不需要） */
static int16_t  s_rise_old_x10;
static uint32_t s_rise_old_ms;
static uint8_t  s_rise_have;
static int16_t  s_rise_x10_per_min;

/* 累计电荷（10mA·ms，取证与 mAh 换算的来源） */
static uint32_t s_chg_raw;
static uint32_t s_dis_raw;

static uint16_t s_selftest_rc = 0xFFFFu;

void app_soc_init(void)
{
    s_acc = 0;
    s_soc_x10 = 0;
    s_anchored = 0u;
    s_trust = 0u;
    s_have_last = 0u;
    s_last_ms = 0u;
    s_rise_have = 0u;
    s_rise_old_x10 = 0;
    s_rise_old_ms = 0u;
    s_rise_x10_per_min = 0;
    s_chg_raw = 0u;
    s_dis_raw = 0u;
}

uint16_t app_soc_get_x10(void)
{
    return (s_trust != 0u) ? (uint16_t)s_soc_x10 : (uint16_t)APP_SOC_UNKNOWN;
}

uint8_t app_soc_trust(void)
{
    return s_trust;
}

uint8_t app_soc_set_anchor(uint8_t kind, uint16_t soc_x10)
{
    if (kind > APP_SOC_ANCHOR_USER) { return 1u; }

    s_soc_x10 = (int32_t)((soc_x10 > SOC_FULL_X10) ? SOC_FULL_X10 : soc_x10);
    s_acc = 0;
    s_anchored = 1u;
    s_trust = 1u;                   /* 锚点本身就给出"现在是多少"，与电流通道无关 */

    return 0u;
}

void app_soc_charge_raw(uint32_t *chg, uint32_t *dis)
{
    if (chg != NULL) { *chg = s_chg_raw; }
    if (dis != NULL) { *dis = s_dis_raw; }
}

/* ===================== ★★ 派生量本体（纯函数） ===================== */
void app_soc_update(const bms_raw_t *r, bms_derived_t *d, uint32_t now_ms)
{
    uint16_t i;
    uint16_t cell_max = 0u;
    uint16_t cell_min = 0u;
    uint8_t  cell_any = 0u;
    int16_t  t_max = 0;
    int16_t  t_min = 0;
    uint8_t  t_any = 0u;

    if ((r == NULL) || (d == NULL)) { return; }

    /* ---------- ① FR-SOC-05：单体极值与压差（★ 只统计 **cell_valid** 的串） ---------- */
    cell_min = APP_MV_INVALID;

    for (i = 0u; i < (uint16_t)APP_CELL_COUNT; i++)
    {
        if ((r->cell_valid & (uint16_t)(1u << i)) == 0u) { continue; }

        if (cell_any == 0u) { cell_max = r->cell_mv[i]; cell_min = r->cell_mv[i]; cell_any = 1u; }
        else
        {
            if (r->cell_mv[i] > cell_max) { cell_max = r->cell_mv[i]; }
            if (r->cell_mv[i] < cell_min) { cell_min = r->cell_mv[i]; }
        }
    }

    if (cell_any != 0u)
    {
        d->cell_max_mv = cell_max;
        d->cell_min_mv = cell_min;
        d->delta_mv    = (uint16_t)(cell_max - cell_min);
    }
    else
    {
        /* ★ 与既有口径一致（`app_data.c` 原来就是这么填的）：无效时
         *   max = 0、min = `APP_MV_INVALID`、压差 = 0 ⇒ 下游能一眼看出"没有可信单体"。 */
        d->cell_max_mv = 0u;
        d->cell_min_mv = APP_MV_INVALID;
        d->delta_mv    = 0u;
    }

    /* ---------- ② FR-SOC-05：温度极值/温差（★ 只统计 **temp_valid** 的测点） ---------- */
    for (i = 0u; i < (uint16_t)APP_BMS_TEMP_ACTIVE_COUNT; i++)
    {
        /* ★★ 只统计**电芯测点**（§4.4 v3.50 裁决 ④）：环境 [1] ⛔ 不进保护判据 ——
         *   §3.2 明写「环境温度不能替代电芯测点」，而温差是**切断级**判据的输入。 */
        if ((APP_TEMP_CELL_MASK & (uint16_t)(1u << i)) == 0u) { continue; }

        if ((r->temp_valid & (uint16_t)(1u << i)) == 0u) { continue; }

        if (t_any == 0u) { t_max = r->temp_x10[i]; t_min = r->temp_x10[i]; t_any = 1u; }
        else
        {
            if (r->temp_x10[i] > t_max) { t_max = r->temp_x10[i]; }
            if (r->temp_x10[i] < t_min) { t_min = r->temp_x10[i]; }
        }
    }

    if (t_any != 0u)
    {
        d->temp_max_x10   = t_max;
        d->temp_min_x10   = t_min;
        d->temp_delta_x10 = (int16_t)(t_max - t_min);
    }
    else
    {
        d->temp_max_x10 = 0; d->temp_min_x10 = 0; d->temp_delta_x10 = 0;
    }

    /* ---------- ③ FR-SOC-05：温升速率（两点差商，窗口 ≥ 60s；不足窗口时沿用上一值） ---------- */
    if (t_any != 0u)
    {
        if (s_rise_have == 0u)
        {
            s_rise_old_x10 = t_max;
            s_rise_old_ms  = now_ms;
            s_rise_have    = 1u;
        }
        else
        {
            uint32_t dt_ms = (uint32_t)(now_ms - s_rise_old_ms);

            if (dt_ms >= 1000u)     /* ★ <1s 的差商全是量化噪声，不 publish */
            {
                /* rate(0.1℃/min) = Δtemp(0.1℃) × 60 / Δt(s) */
                s_rise_x10_per_min = (int16_t)(((int32_t)(t_max - s_rise_old_x10) * 60) /
                                               (int32_t)(dt_ms / 1000u));
            }

            if (dt_ms >= 60000u)    /* 满 60s 才滚动老点 ⇒ 窗口恒定 ≈1min */
            {
                s_rise_old_x10 = t_max;
                s_rise_old_ms  = now_ms;
            }
        }
    }

    d->temp_rise_x10_per_min = s_rise_x10_per_min;

    /* ---------- ④ SOC：安时积分 ＋ **FR-SOC-03 闸门** ---------- */
    if (r->curr_valid == 0u)
    {
        /* ★ 不可知即不安全：电流不可信 ⇒ **停止积分**且 SOC 不可信。
         *   ⛔ 不得"用上一次的电流继续积" —— 那会输出一个正在漂的假数。 */
        s_trust = 0u;
        s_have_last = 0u;           /* 也丢掉时间基准：恢复后从新的一拍重新起算 */
    }
    else if (s_have_last != 0u)
    {
        uint32_t dt_ms = (uint32_t)(now_ms - s_last_ms);

        if (dt_ms > (uint32_t)APP_DATA_STALE_MS)
        {
            /* ★ 断档（超新鲜度窗）：这段时间的电荷**不可知** ⇒ 不计入且置不可信。
             *   ⚠️ 用 `APP_DATA_STALE_MS` 这一条既有口径，⛔ 不另编一个"合理"的秒数。 */
            s_trust = 0u;
            s_have_last = 0u;
        }
        else if (dt_ms > 0u)
        {
            int32_t dq = (int32_t)r->curr_10ma * (int32_t)dt_ms;

            s_acc += dq;

            if (s_acc >= SOC_DEN)       { s_soc_x10 += (s_acc / SOC_DEN); s_acc = s_acc % SOC_DEN; }
            else if (s_acc <= -SOC_DEN) { s_soc_x10 += (s_acc / SOC_DEN); s_acc = s_acc % SOC_DEN; }

            if (s_soc_x10 < 0)                 { s_soc_x10 = 0; }
            if (s_soc_x10 > SOC_FULL_X10)      { s_soc_x10 = SOC_FULL_X10; }

            if (dq > 0) { s_chg_raw += (uint32_t)dq; } else { s_dis_raw += (uint32_t)(-dq); }

            /* ★ 只有**有锚点**时 SOC 才可信（`FR-SOC-02` 缺口 a：OCV/满充/放空锚点等 D16） */
            s_trust = s_anchored;
            s_have_last = 1u;
            s_last_ms = now_ms;
        }
        else
        {
            /* dt == 0：同一拍重复调用 ⇒ 什么都不做（⛔ 不清可信位） */
        }
    }
    else
    {
        s_have_last = 1u;
        s_last_ms = now_ms;
        s_trust = s_anchored;
    }

    /* ---------- ⑤ 落派生盒（本模块拥有的字段） ---------- */
    d->soc_x10 = (s_trust != 0u) ? (uint16_t)s_soc_x10 : (uint16_t)APP_SOC_UNKNOWN;
    d->soh_x10 = 1000u;                                     /* ⚠️ 缺口 b：未测容量前按 100% */
    d->chg_mah = s_chg_raw / CHARGE_TO_MAH;
    d->dis_mah = s_dis_raw / CHARGE_TO_MAH;
    d->cycles  = 0u;                                        /* ⚠️ 缺口 b：需满充/放空检测（D16） */
    d->chem    = (chem_type_t)APP_BMS_CHEM;
}

void app_soc_step(uint32_t now_ms)
{
    bms_raw_t     *r = app_data_raw_write();
    bms_derived_t *d = app_data_der_write();

    /* ⚠️ 这里取到的是**发布前的 back 缓冲**（`app_data_*_write()` 的口径）⇒ 写进去的派生量
     *   由调用者那一轮的既有 commit 一并发布。⛔ 本函数自己 commit = 第二个发布点。 */
    app_soc_update(r, d, now_ms);
}

/* ===================== ★★ 离线注入自检 ===================== */
static void soc_mk(bms_raw_t *r, uint16_t c0, uint16_t c1, uint16_t c2, uint16_t c3,
                   uint16_t cvalid, int16_t t0, int16_t t1, uint16_t tvalid)
{
    memset(r, 0, sizeof(*r));
    r->cell_mv[0] = c0; r->cell_mv[1] = c1; r->cell_mv[2] = c2; r->cell_mv[3] = c3;
    r->cell_valid = cvalid;
    r->temp_x10[0] = t0; r->temp_x10[1] = t1;
    r->temp_valid = tvalid;
    r->curr_valid = 1u;
}

static uint16_t s_ocv_selftest_rc = 0xFFFFu;

uint16_t app_soc_ocv_selftest_result(void) { return s_ocv_selftest_rc; }

uint16_t app_soc_ocv_to_soc(uint16_t ocv_mv)
{
    uint16_t soc[APP_CALIB_OCV_MAX];
    uint16_t mv[APP_CALIB_OCV_MAX];
    uint8_t  n = 0u;
    uint8_t  i;

    app_calib_get_ocv(&n, soc, mv);

    if (n < 2u) { return APP_SOC_UNKNOWN; }      /* 表不够两点 ⇒ 无插值依据 */

    for (i = 0u; (uint8_t)(i + 1u) < n; i++)
    {
        uint16_t m_lo = (mv[i] < mv[i + 1u]) ? mv[i] : mv[i + 1u];
        uint16_t m_hi = (mv[i] < mv[i + 1u]) ? mv[i + 1u] : mv[i];

        if ((ocv_mv >= m_lo) && (ocv_mv <= m_hi))
        {
            int32_t den = (int32_t)mv[i + 1u] - (int32_t)mv[i];
            int32_t v;

            if (den == 0) { return soc[i]; }     /* 两点同压：退化，取左端 */

            v = (int32_t)soc[i] +
                (((int32_t)ocv_mv - (int32_t)mv[i]) * ((int32_t)soc[i + 1u] - (int32_t)soc[i])) / den;

            if (v < 0)    { v = 0; }
            if (v > 1000) { v = 1000; }

            return (uint16_t)v;
        }
    }

    return APP_SOC_UNKNOWN;      /* ★ 表外：⛔ 不外推（外推值没有任何测量依据） */
}

uint8_t app_soc_anchor_ocv(uint16_t ocv_mv)
{
    uint16_t s = app_soc_ocv_to_soc(ocv_mv);

    if (s == APP_SOC_UNKNOWN) { return 0u; }

    return app_soc_set_anchor(APP_SOC_ANCHOR_OCV, s);
}

/**
 * @brief       ★ 离线自检：查表与插值（⛔ 不需要器件）
 * @note        ★★ **单出口 ＋ 无条件还原**（`附录C` C-134 口径 a：⛔ 自检不得改变系统状态）。
 */
uint16_t app_soc_ocv_selftest(void)
{
    uint16_t save_soc[APP_CALIB_OCV_MAX];
    uint16_t save_mv[APP_CALIB_OCV_MAX];
    uint8_t  save_n = 0u;
    uint16_t rc = 0u;
    uint16_t s3[3] = { 100u, 500u, 900u };
    uint16_t m3[3] = { 3280u, 3300u, 3350u };

    app_calib_get_ocv(&save_n, save_soc, save_mv);

    if (app_calib_set_ocv(3u, s3, m3) != 0u)          { rc = 1u; }
    else if (app_soc_ocv_to_soc(3280u) != 100u)       { rc = 2u; }   /* 精确点 1 */
    else if (app_soc_ocv_to_soc(3300u) != 500u)       { rc = 2u; }   /* 精确点 2 */
    else if (app_soc_ocv_to_soc(3350u) != 900u)       { rc = 2u; }   /* 精确点 3 */
    else if (app_soc_ocv_to_soc(3290u) != 300u)       { rc = 3u; }   /* ★ 中点 ⇒ 线性插值 */
    else if (app_soc_ocv_to_soc(3200u) != APP_SOC_UNKNOWN) { rc = 4u; }   /* ★ 表下界外 ⇒ 未知 */
    else if (app_soc_ocv_to_soc(3400u) != APP_SOC_UNKNOWN) { rc = 4u; }   /* ★ 表上界外 ⇒ 未知 */
    else { /* 全过 */ }

    /* ★★ 无条件还原 */
    if (save_n >= 2u) { (void)app_calib_set_ocv(save_n, save_soc, save_mv); }
    else              { app_calib_clear_ocv(); }

    s_ocv_selftest_rc = rc;
    return rc;
}

uint16_t app_soc_selftest(void)
{
    bms_raw_t     r;
    bms_derived_t d;
    uint16_t      n = 0u;
    uint32_t      t;
    uint16_t      i;

    /* --- 用例 1：四串全有效 {3300,3400,3500,3600} ⇒ max 3600 / min 3300 / 压差 300 --- */
    n++;
    app_soc_init();
    soc_mk(&r, 3300, 3400, 3500, 3600, 0x000F, 250, 250, 0x0003);
    memset(&d, 0, sizeof(d));
    app_soc_update(&r, &d, 100u);
    if ((d.cell_max_mv != 3600u) || (d.cell_min_mv != 3300u) || (d.delta_mv != 300u)) { s_selftest_rc = n; return n; }

    /* --- 用例 2：只留 2 串有效（第 4 串无效）⇒ 极值只在有效串里取 --- */
    n++;
    app_soc_init();
    soc_mk(&r, 3300, 3400, 3500, 4999, 0x0007, 250, 250, 0x0003);
    memset(&d, 0, sizeof(d));
    app_soc_update(&r, &d, 100u);
    if ((d.cell_max_mv != 3500u) || (d.cell_min_mv != 3300u) || (d.delta_mv != 200u)) { s_selftest_rc = n; return n; }

    /* --- 用例 3：一串都无效 ⇒ max 0 / min = 无效码 / 压差 0（⛔ 不是"0V 压差正常"） --- */
    n++;
    app_soc_init();
    soc_mk(&r, 3300, 3400, 3500, 3600, 0x0000, 250, 250, 0x0003);
    memset(&d, 0, sizeof(d));
    app_soc_update(&r, &d, 100u);
    if ((d.cell_max_mv != 0u) || (d.cell_min_mv != APP_MV_INVALID)) { s_selftest_rc = n; return n; }

    /* --- 用例 4：温度只 1 个测点有效 ⇒ 极值=该点、温差 0 --- */
    n++;
    app_soc_init();
    soc_mk(&r, 3300, 3300, 3300, 3300, 0x000F, 250, 400, 0x0001);
    memset(&d, 0, sizeof(d));
    app_soc_update(&r, &d, 100u);
    if ((d.temp_max_x10 != 250) || (d.temp_min_x10 != 250) || (d.temp_delta_x10 != 0)) { s_selftest_rc = n; return n; }

    /* --- 用例 5：未锚定 ⇒ SOC = 未知（⛔ 不冒充数值） --- */
    n++;
    app_soc_init();
    soc_mk(&r, 3300, 3300, 3300, 3300, 0x000F, 250, 250, 0x0003);
    r.curr_10ma = 100;
    memset(&d, 0, sizeof(d));
    app_soc_update(&r, &d, 1000u);
    app_soc_update(&r, &d, 2000u);
    if ((d.soc_x10 != APP_SOC_UNKNOWN) || (app_soc_trust() != 0u)) { s_selftest_rc = n; return n; }

    /* --- 用例 6：锚 50% 后，1A 充 60s ⇒ ΔSOC ≈ 1A×60s = 16.67mAh / 6000mAh = +0.278% ⇒ +2.8(0.1%)
     *   （★ 期望值当场算出来，⛔ 不靠印象：Δsoc_x10 = 1000×16.67/6000 = 2.78 ⇒ 允许 2~3） --- */
    n++;
    app_soc_init();
    if (app_soc_set_anchor(APP_SOC_ANCHOR_USER, 500u) != 0u) { s_selftest_rc = n; return n; }
    soc_mk(&r, 3300, 3300, 3300, 3300, 0x000F, 250, 250, 0x0003);
    r.curr_10ma = 100;                          /* 100 × 10mA = 1A */
    t = 1000u;
    memset(&d, 0, sizeof(d));
    app_soc_update(&r, &d, t);
    for (i = 0u; i < 60u; i++) { t += 1000u; app_soc_update(&r, &d, t); }
    if ((d.soc_x10 < 502u) || (d.soc_x10 > 503u)) { s_selftest_rc = n; return n; }   /* 500 + 2~3 */

    /* --- 用例 7：★ FR-SOC-03 闸门：电流无效 ⇒ SOC 未知且不可信 --- */
    n++;
    app_soc_init();
    (void)app_soc_set_anchor(APP_SOC_ANCHOR_USER, 500u);
    soc_mk(&r, 3300, 3300, 3300, 3300, 0x000F, 250, 250, 0x0003);
    r.curr_valid = 0u;
    memset(&d, 0, sizeof(d));
    app_soc_update(&r, &d, 1000u);
    app_soc_update(&r, &d, 2000u);
    if ((d.soc_x10 != APP_SOC_UNKNOWN) || (app_soc_trust() != 0u)) { s_selftest_rc = n; return n; }

    /* --- 用例 8：断档（dt > 新鲜度窗）⇒ 不可信（⛔ 不把未知电荷当成 0） --- */
    n++;
    app_soc_init();
    (void)app_soc_set_anchor(APP_SOC_ANCHOR_USER, 500u);
    soc_mk(&r, 3300, 3300, 3300, 3300, 0x000F, 250, 250, 0x0003);
    r.curr_10ma = 100;
    memset(&d, 0, sizeof(d));
    app_soc_update(&r, &d, 1000u);
    app_soc_update(&r, &d, 1000u + (uint32_t)APP_DATA_STALE_MS + 1u);
    if (app_soc_trust() != 0u) { s_selftest_rc = n; return n; }

    s_selftest_rc = 0u;
    return 0u;
}

uint16_t app_soc_selftest_result(void)
{
    return s_selftest_rc;
}
