/**
 ****************************************************************************************************
 * @file        app_alarm.c
 * @brief       BMS 三级告警判定（规格 = `Tech_Design.md` §5.3 ／ `附录C` C-123；判据数值 = `PRD.md` §2.3）
 * @note        ★★ 本文件**只管 BMS 三级**；环境两级已在 `app_env_alarm.{c,h}`（`C-38`）。
 *              ★ 判据本体是**纯函数**：只读 `bms_raw_t`/`bms_derived_t`、只写 `bms_alarm_t`，
 *                ⛔ 不写快照、⛔ 不碰执行器、⛔ 不调 `delay`/`HAL_GetTick()`、不读外设（S15）。
 *              ★ 门限**只引用 `app_config.h` §12.7 的宏**，⛔ 本文件不出现任何门限字面量
 *                （唯二的例外是"时间常数"与"位号"，它们不是门限）。
 ****************************************************************************************************
 */

#include "./App/app_alarm.h"
#include <string.h>
#include <stdio.h>

/* ===================== 内部量种类（决定取哪个输入、用哪一组门限） ===================== */
typedef enum
{
    K_CELL_MAX = 0,     /* 最高单体 mV（r->cell_valid 有效时取 d->cell_max_mv） */
    K_CELL_MIN,         /* 最低单体 mV */
    K_T_MAX,            /* 最高温度 0.1℃（= 电芯表面温度；AGENTS §2.3 的多测点未接，见 §6 差距声明） */
    K_T_MIN,            /* 最低温度 0.1℃（同上） */
    K_T_DIF,            /* 温差 0.1℃ */
    K_T_RISE,           /* 温升速率 0.1℃/min */
    K_I_CHG,            /* 充电电流 10mA */
    K_I_DIS,            /* 放电电流 10mA */
    K_DISABLED          /* ★ 本版**无数值**、刻意不启用（⛔ 不编数值） */
} alm_kind_t;

/* 三级动作（§5.3 方向映射表） */
#define ACT_DERATE      0u      /* 只降额，不切断（温差 / 温升速率） */
#define ACT_CUT_CHG     1u      /* 断充电 */
#define ACT_CUT_DSG     2u      /* 断放电 */
#define ACT_DERATE_BOTH 3u      /* 两个方向都降额 */

/* 降额百分比（§5.3 等级→动作表；`FR-ALM-08`） */
#define DERATE_L1_PCT   50u
#define DERATE_L2_PCT   20u

/* ===================== 判据元数据（★ 顺序与 §6.2 位号顺序一致） ===================== */
typedef struct
{
    uint8_t     kind;
    uint8_t     fault_bit;      /* §6.2 位号（0..15） */
    uint8_t     action;
    uint8_t     latch;          /* 1 = 三级时进 S11 锁定；0 = B 类（0℃ 禁充） */
    const char *obj;            /* 原因串对象名 */
    const char *code;           /* 原因串判据码 */
} alm_meta_t;

static const alm_meta_t s_meta[APP_ALM_Q_COUNT] =
{
    /* 顺序必须与 app_alm_q_t 一致 */
    { K_CELL_MAX,  0u, ACT_CUT_CHG,  1u, "cell", "OV"   },   /* 0 单体过压 */
    { K_CELL_MIN,  1u, ACT_CUT_DSG,  1u, "cell", "UV"   },   /* 1 单体欠压 */
    { K_DISABLED,  2u, ACT_CUT_CHG,  1u, "pack", "OV"   },   /* 2 组过压 ⚠️ 无数值 */
    { K_DISABLED,  3u, ACT_CUT_DSG,  1u, "pack", "UV"   },   /* 3 组欠压 ⚠️ 无数值 */
    { K_I_CHG,     4u, ACT_CUT_CHG,  1u, "I",    "OC"   },   /* 4 充电过流 */
    { K_I_DIS,     5u, ACT_CUT_DSG,  1u, "I",    "OC"   },   /* 5 放电过流 */
    { K_DISABLED,  6u, ACT_CUT_DSG,  1u, "SW",   "hw"   },   /* 6 硬件层（见下 hw_apply） */
    { K_T_MAX,     8u, ACT_CUT_CHG,  1u, "T",    "chgOT"},   /* 7 充电过温 */
    { K_T_MIN,     9u, ACT_CUT_CHG,  1u, "T",    "chgUT"},   /* 8 充电过冷 */
    { K_T_MAX,    10u, ACT_CUT_DSG,  1u, "T",    "disOT"},   /* 9 放电过温 */
    { K_T_MIN,    11u, ACT_CUT_DSG,  1u, "T",    "disUT"},   /* 10 放电过冷 */
    { K_T_DIF,    12u, ACT_DERATE_BOTH, 0u, "T", "dif"  },   /* 11 温差 */
    { K_T_RISE,   13u, ACT_DERATE_BOTH, 0u, "T", "rise" },   /* 12 温升速率 */
    { K_T_MAX,    9u,  ACT_CUT_CHG,  0u, "no-chg", "0C" },   /* 13 ★ 0℃ 禁充（B 类，⛔ 不锁定） */
};

/* ★ 三级门限（升序或降序由 ladder 决定；值**全部来自宏**）。
 *   ⚠️ `NOCHG` 只用 [0] = 禁充阈值；[1]/[2] 同值 ⇒ "达到即三级动作力度"。 */
static const int32_t s_thr[APP_ALM_Q_COUNT][3] =
{
    { APP_BMS_CELL_OV_L1_MV,   APP_BMS_CELL_OV_L2_MV,   APP_BMS_CELL_OV_L3_MV   },
    { APP_BMS_CELL_UV_L1_MV,   APP_BMS_CELL_UV_L2_MV,   APP_BMS_CELL_UV_L3_MV   },
    { 0, 0, 0 },                                                             /* 组过压：无数值 */
    { 0, 0, 0 },                                                             /* 组欠压：无数值 */
    { APP_BMS_I_CHG_L1_10MA,   APP_BMS_I_CHG_L2_10MA,   APP_BMS_I_CHG_L3_10MA   },
    { APP_BMS_I_DIS_L1_10MA,   APP_BMS_I_DIS_L2_10MA,   APP_BMS_I_DIS_L3_10MA   },
    { 0, 0, 0 },                                                             /* 硬件层：见 hw_apply() */
    { APP_BMS_T_CHG_OT_L1_X10, APP_BMS_T_CHG_OT_L2_X10, APP_BMS_T_CHG_OT_L3_X10 },
    { APP_BMS_T_CHG_UT_L1_X10, APP_BMS_T_CHG_UT_L2_X10, APP_BMS_T_CHG_UT_L3_X10 },
    { APP_BMS_T_DIS_OT_L1_X10, APP_BMS_T_DIS_OT_L2_X10, APP_BMS_T_DIS_OT_L3_X10 },
    { APP_BMS_T_DIS_UT_L1_X10, APP_BMS_T_DIS_UT_L2_X10, APP_BMS_T_DIS_UT_L3_X10 },
    { APP_BMS_T_DIF_L1_X10,    APP_BMS_T_DIF_L2_X10,    APP_BMS_T_DIF_L3_X10    },
    { APP_BMS_TR_RATE_L1_X10,  APP_BMS_TR_RATE_L2_X10,  APP_BMS_TR_RATE_L3_X10  },
    { APP_BMS_T_NOCHG_X10,     APP_BMS_T_NOCHG_X10,     APP_BMS_T_NOCHG_X10     },
};

/* 1 = "越大越严重"（升序梯形）；0 = "越小越严重" */
static const uint8_t s_asc[APP_ALM_Q_COUNT] =
{
    1u, 0u, 1u, 0u, 1u, 1u, 1u, 1u, 0u, 1u, 0u, 1u, 1u, 0u
};

/* 每一级的解除迟滞（★ 温升速率 = 0：PRD 行 189 明令"不叠加迟滞"，靠持续 N 周期去抖） */
static const int32_t s_hyst[APP_ALM_Q_COUNT] =
{
    APP_BMS_CELL_HYST_MV, APP_BMS_CELL_HYST_MV, 0, 0,
    APP_BMS_I_HYST_CHG_10MA, APP_BMS_I_HYST_DIS_10MA, 0,
    APP_BMS_T_HYST_X10, APP_BMS_T_HYST_X10, APP_BMS_T_HYST_X10, APP_BMS_T_HYST_X10,
    APP_BMS_T_DIF_HYST_X10, 0,
    APP_BMS_T_NOCHG_REC_X10          /* ★ 0℃ 禁充的"解除" = 回升到 T_NOCHG_REC（迟滞语义） */
};

/* ★ 前置声明：原因串要给"哪一串"的序号（最高/最低单体的下标） */
static int alm_cell_idx(app_alm_q_t q, const bms_raw_t *r, const bms_derived_t *d);

/* 每一级的确认所需时间（ms）。★ 三级用 `DEBOUNCE_L3_MS`（PRD 行 192：三级不得用 3s） */
static uint32_t alm_need_ms(app_alm_q_t q, uint8_t lv)
{
    if (lv >= 3u)
    {
        /* 电流三级另有"持续时间"要求 ⇒ 与去抖取大（§5.3 时间状态机表） */
        if (q == APP_ALM_Q_I_CHG) { return (uint32_t)APP_BMS_I_HOLD_L3_MS; }
        if (q == APP_ALM_Q_I_DIS) { return (uint32_t)APP_BMS_I_HOLD_L3_MS; }
        return (uint32_t)APP_BMS_DEBOUNCE_L3_MS;
    }

    if (q == APP_ALM_Q_I_CHG)
    {
        return (uint32_t)((lv == 1u) ? APP_BMS_I_HOLD_L1_MS : APP_BMS_I_HOLD_L2_MS);
    }
    if (q == APP_ALM_Q_I_DIS)
    {
        return (uint32_t)((lv == 1u) ? APP_BMS_I_HOLD_L1_MS : APP_BMS_I_HOLD_L2_MS);
    }

    return (uint32_t)APP_BMS_CONFIRM_MS;
}

/* 温升速率与压差走"持续 N 个判定周期"（不是时间窗） */
static uint8_t alm_need_n(app_alm_q_t q, uint8_t lv)
{
    if (q != APP_ALM_Q_T_RISE) { return 0u; }       /* 0 = 不走 N 周期 */

    if (lv >= 3u) { return (uint8_t)APP_BMS_TR_HOLD_L3_N; }
    return (uint8_t)((lv == 1u) ? APP_BMS_TR_HOLD_L1_N : APP_BMS_TR_HOLD_L2_N);
}

/* ===================== 每判据的静态状态 ===================== */
typedef struct
{
    uint8_t  lvl;           /* 已确认等级 0..3 */
    uint8_t  cand;          /* 上一拍的候选等级（变化即重置计时） */
    uint8_t  n_hold;        /* 持续性计数（N 周期类判据） */
    uint32_t t_enter;       /* 候选等级的起始时刻 */
    int32_t  v;             /* 最近实测值（给原因串） */
    uint32_t t_lvl;         /* ★ 进入**当前**等级的时刻（原因串并列时的第二判据） */
} alm_st_t;

static alm_st_t s_q[APP_ALM_Q_COUNT];

/* 0℃ 禁充的"回升持续"计时（B 类：回升 ≥ 阈值 且持续 `T_NOCHG_REC_MS` 才解除） */
static uint32_t s_nochg_rec_t;
static uint8_t  s_nochg_rec_on;

/* 降额请求状态（§5.3 表：二级发起请求并计时；⚠️ 超时值在 PRD/宏里**没有** ⇒ 见文件末的缺口登记） */
static uint32_t s_derate_t;

/* 电流突变：上一拍的原始电流与时刻（★ 必须用**原始值**：滤波会掩盖突变） */
static int16_t  s_i_prev;
static uint32_t s_i_prev_t;
static uint8_t  s_i_prev_ok;

static uint16_t s_selftest_rc = 0xFFFFu;

void app_alarm_init(void)
{
    memset(s_q, 0, sizeof(s_q));
    s_nochg_rec_t = 0u;
    s_nochg_rec_on = 0u;
    s_derate_t = 0u;
    s_i_prev = 0;
    s_i_prev_t = 0u;
    s_i_prev_ok = 0u;
}

/* ===================== 取实测值 ===================== */
static int32_t alm_value(app_alm_q_t q, const bms_raw_t *r, const bms_derived_t *d)
{
    switch (s_meta[q].kind)
    {
        case K_CELL_MAX: return (int32_t)d->cell_max_mv;
        case K_CELL_MIN: return (int32_t)d->cell_min_mv;
        case K_T_MAX:    return (int32_t)d->temp_max_x10;
        case K_T_MIN:    return (int32_t)d->temp_min_x10;
        case K_T_DIF:    return (int32_t)d->temp_delta_x10;
        case K_T_RISE:   return (int32_t)d->temp_rise_x10_per_min;
        case K_I_CHG:    return (r->curr_10ma > 0) ? (int32_t)r->curr_10ma : 0;
        case K_I_DIS:    return (r->curr_10ma < 0) ? (int32_t)(-(int32_t)r->curr_10ma) : 0;
        default:         return 0;
    }
}

/* 该判据的输入是否可信（★ 不可知即不安全：无效通道 ⇒ 不得用它判定，见 §4.4） */
static uint8_t alm_input_valid(app_alm_q_t q, const bms_raw_t *r)
{
    switch (s_meta[q].kind)
    {
        case K_CELL_MAX:
        case K_CELL_MIN: return (r->cell_valid != 0u) ? 1u : 0u;
        case K_I_CHG:
        case K_I_DIS:    return (r->curr_valid != 0u) ? 1u : 0u;
        case K_T_MAX:
        case K_T_MIN:
        case K_T_DIF:
        case K_T_RISE:   return ((r->temp_valid & 0x01u) != 0u) ? 1u : 0u;  /* [0] = 电芯表面 */
        default:         return 0u;
    }
}

/* ★ 温度修正（FR-ALM-04）：T ≥ 45℃ 充电门限 ×0.5；T ≥ 50℃ ×0.2。只作用于**充电电流**门限。 */
static int32_t alm_thr_eff(app_alm_q_t q, uint8_t idx, int32_t t_max_x10)
{
    int32_t t = s_thr[q][idx];

    if (q != APP_ALM_Q_I_CHG) { return t; }

    if (t_max_x10 >= APP_BMS_T_CHG_OT_L2_X10) { return t / 5; }     /* ×0.2 */
    if (t_max_x10 >= APP_BMS_T_CHG_OT_L1_X10) { return t / 2; }     /* ×0.5 */

    return t;
}

/* 逐级候选等级（未确认、未迟滞） */
static uint8_t alm_cand(app_alm_q_t q, int32_t v, int32_t t_max_x10)
{
    uint8_t lv = 0u;

    if (s_meta[q].kind == K_DISABLED) { return 0u; }

    if (s_asc[q] != 0u)
    {
        if (v >= alm_thr_eff(q, 0u, t_max_x10)) { lv = 1u; }
        if (v >= alm_thr_eff(q, 1u, t_max_x10)) { lv = 2u; }
        if (v >= alm_thr_eff(q, 2u, t_max_x10)) { lv = 3u; }
    }
    else
    {
        if (v <= alm_thr_eff(q, 0u, t_max_x10)) { lv = 1u; }
        if (v <= alm_thr_eff(q, 1u, t_max_x10)) { lv = 2u; }
        if (v <= alm_thr_eff(q, 2u, t_max_x10)) { lv = 3u; }
    }

    return lv;
}

/* 迟滞解除判定：从等级 `lv` 退出的条件是"回到第 lv 级门限之外（按迟滞内收）" */
static uint8_t alm_released(app_alm_q_t q, uint8_t lv, int32_t v, int32_t t_max_x10)
{
    int32_t thr = alm_thr_eff(q, (uint8_t)(lv - 1u), t_max_x10);
    int32_t h   = s_hyst[q];

    if (s_asc[q] != 0u) { return (v <= (thr - h)) ? 1u : 0u; }      /* 升序：要掉到门限-迟滞以下 */
    return (v >= (thr + h)) ? 1u : 0u;                              /* 降序：要升回门限+迟滞以上 */
}

/**
 * @brief       逐判据的确认/迟滞/持续性状态机（§5.3 时间状态机）
 * @note        ★ 规则：**升级要确认窗（三级更短）＋N 周期；解除只要迟滞成立即生效，且逐级回落**
 *              （★ 逐级回落是本表新增口径：跳级回落会让恢复过程在日志里消失）
 */
static uint8_t alm_step(app_alm_q_t q, int32_t v, uint8_t valid, uint32_t now_ms, int32_t t_max_x10)
{
    alm_st_t *s = &s_q[q];
    uint8_t   c;

    s->v = v;

    if (valid == 0u)
    {
        /* ⚠️ 输入无效 ⇒ **保持**当前等级（⛔ 不据此升级，⛔ 也不据此解除）：
         * "读不到"既不是"越限"也不是"恢复"（§4.4：不可知即不安全 —— 向保守方向是"不放行"，
         * 而"放行"要靠明确的恢复证据）。 */
        return s->lvl;
    }

    c = alm_cand(q, v, t_max_x10);

    if (c > s->lvl)
    {
        if (s->cand != c) { s->cand = c; s->t_enter = now_ms; s->n_hold = 0u; }

        if (alm_need_n(q, c) != 0u)
        {
            if (s->n_hold < 0xFFu) { s->n_hold++; }
            if (s->n_hold >= alm_need_n(q, c)) { s->lvl = c; s->t_lvl = now_ms; s->cand = 0u; s->n_hold = 0u; }
        }
        else if ((uint32_t)(now_ms - s->t_enter) >= alm_need_ms(q, c))
        {
            s->lvl = c; s->t_lvl = now_ms; s->cand = 0u;
        }
    }
    else if (c < s->lvl)
    {
        s->cand = 0u;
        s->n_hold = 0u;

        if (alm_released(q, s->lvl, v, t_max_x10) != 0u)
        {
            s->lvl = (uint8_t)(s->lvl - 1u);        /* ★ 逐级回落 */
            s->t_lvl = now_ms;
        }
    }
    else
    {
        s->cand = 0u;
        s->n_hold = 0u;
    }

    return s->lvl;
}

/* ===================== 硬件层（AFE 已动作） =====================
 * ★ 软件**不重复动作**（硬件已经断了），只：记录 + 置位 + 锁定（越限型，S11）。
 * ⚠️ `a->hw_fault_bits` 由**外部**传入（D8 起 = `TaskProtect` 解码 AFE 状态寄存器后给出）；
 *    ★ 本条注释原写了寄存器名 ⇒ 被 `G7-①` 判违规（**那条机械规则不给注释豁免**）⇒ 改为全文不出现寄存器名。
 *    ⛔ 本模块不去读 AFE（S15）。 */
static void alm_hw_apply(const bms_alarm_t *a_in, bms_alarm_t *a_out, uint32_t now_ms)
{
    uint8_t hw = a_in->hw_fault_bits;

    if (hw == 0u) { return; }

    s_q[APP_ALM_Q_HW].lvl = 3u;
    s_q[APP_ALM_Q_HW].t_lvl = now_ms;
    s_q[APP_ALM_Q_HW].v = (int32_t)hw;
    a_out->hw_layer_state = 2u;                 /* 2 = ACTED（§4.4 的三态之一） */
}

/* ===================== 电流突变（L3，用原始值） =====================
 * 判据：`|ΔI| > APP_BMS_I_JUMP_10MA` 且间隔 ≤ `APP_BMS_I_JUMP_MS`（PRD：5A / 100ms）。 */
static uint8_t alm_jump(int16_t i_now, uint32_t now_ms, uint8_t *dir_chg)
{
    int32_t d;

    *dir_chg = 0u;

    if (s_i_prev_ok == 0u)
    {
        s_i_prev = i_now; s_i_prev_t = now_ms; s_i_prev_ok = 1u;
        return 0u;
    }

    if ((uint32_t)(now_ms - s_i_prev_t) > (uint32_t)APP_BMS_I_JUMP_MS) { return 0u; }  /* 窗口外不算 */

    d = (int32_t)i_now - (int32_t)s_i_prev;

    if (d > (int32_t)APP_BMS_I_JUMP_10MA)  { *dir_chg = 1u; return 1u; }   /* 正向突变 ⇒ 充电方向 */
    if (d < -(int32_t)APP_BMS_I_JUMP_10MA) { *dir_chg = 2u; return 1u; }   /* 反向 ⇒ 放电方向 */

    return 0u;
}

/* ===================== 原因串（FR-ALM-07 的确定性格式） =====================
 * ★ 并列选择规则（C-123 ⑤）：① 等级最高 ② 同级"进入更早" ③ 仍并列取故障位序号最小。 */
static void alm_build_reason(bms_alarm_t *a, const bms_raw_t *r, const bms_derived_t *d)
{
    uint8_t  best = 0xFFu;
    uint8_t  n_other = 0u;
    uint8_t  i;
    int32_t  val = 0;

    for (i = 0u; i < (uint8_t)APP_ALM_Q_COUNT; i++)
    {
        if (s_q[i].lvl == 0u) { continue; }

        if (best == 0xFFu)
        {
            best = i;
            continue;
        }

        if (s_q[i].lvl > s_q[best].lvl) { n_other++; best = i; continue; }
        if (s_q[i].lvl < s_q[best].lvl) { n_other++; continue; }

        /* 同级：进入更早者优先；再并列取故障位序号小者 */
        if (s_q[i].t_lvl < s_q[best].t_lvl) { n_other++; best = i; continue; }
        if (s_q[i].t_lvl > s_q[best].t_lvl) { n_other++; continue; }

        if (s_meta[i].fault_bit < s_meta[best].fault_bit) { n_other++; best = i; }
        else { n_other++; }
    }

    if (best == 0xFFu)
    {
        a->reason[0] = '\0';
        return;
    }

    val = s_q[best].v;

    /* 单位随量：mV→V（三位小数两位有效）、0.1℃→C、10mA→A */
    switch (s_meta[best].kind)
    {
        case K_CELL_MAX:
        case K_CELL_MIN:
            if (n_other != 0u)
            {
                (void)snprintf(a->reason, APP_REASON_LEN, "%s%d %s %d.%02dV +%u",
                               s_meta[best].obj, (int)alm_cell_idx(best, r, d), s_meta[best].code,
                               (int)(val / 1000), (int)((val % 1000) / 10), (unsigned)n_other);
            }
            else
            {
                (void)snprintf(a->reason, APP_REASON_LEN, "%s%d %s %d.%02dV",
                               s_meta[best].obj, (int)alm_cell_idx(best, r, d), s_meta[best].code,
                               (int)(val / 1000), (int)((val % 1000) / 10));
            }
            break;

        case K_I_CHG:
        case K_I_DIS:
            (void)snprintf(a->reason, APP_REASON_LEN, "%s %s%d %s %d.%02dA",
                           s_meta[best].obj, (s_meta[best].kind == K_I_CHG) ? "chg " : "dis ",
                           (int)0, s_meta[best].code, (int)(val / 100), (int)((val % 100) / 1));
            break;

        default:
            (void)snprintf(a->reason, APP_REASON_LEN, "%s %s %d.%01dC",
                           s_meta[best].obj, s_meta[best].code,
                           (int)(val / 10), (int)((val < 0) ? (-val % 10) : (val % 10)));
            break;
    }
}

/* ===================== "哪一串"（原因串的序号，1 起数） ===================== */
static int alm_cell_idx(app_alm_q_t q, const bms_raw_t *r, const bms_derived_t *d)
{
    uint16_t want;
    uint8_t  i;

    /* ⚠️ 只有单体电压类判据才带序号；其余返回 0（原因串不带序号） */
    if ((q != APP_ALM_Q_CELL_OV) && (q != APP_ALM_Q_CELL_UV)) { return 0; }

    /* ★ 用 `d` 的极值去 `r->cell_mv[]` 反查下标 —— ⛔ 不在本模块另存"哪个是最大"（那是第二个落点） */
    want = (q == APP_ALM_Q_CELL_OV) ? d->cell_max_mv : d->cell_min_mv;

    for (i = 0u; i < (uint8_t)APP_CELL_COUNT; i++)
    {
        if (((r->cell_valid & (uint16_t)(1u << i)) != 0u) && (r->cell_mv[i] == want))
        {
            return (int)i + 1;                  /* 1 起数 ⇒ 串号 */
        }
    }

    return 0;
}

/* ===================== 主判定 ===================== */
void app_alarm_eval(const bms_raw_t *r, const bms_derived_t *d, bms_alarm_t *a, uint32_t now_ms)
{
    int32_t t_max = (int32_t)d->temp_max_x10;
    uint16_t fault = 0u;
    uint8_t  level = 0u;
    uint8_t  cut = 0u;
    uint8_t  latch = 0u;
    uint8_t  derate_chg = 100u;
    uint8_t  derate_dis = 100u;
    uint8_t  any_derate = 0u;
    uint8_t  i;

    if ((r == 0) || (d == 0) || (a == 0)) { return; }

    /* ---- ① 逐判据：候选 → 确认窗/持续性 → 迟滞解除（逐级回落） ---- */
    for (i = 0u; i < (uint8_t)APP_ALM_Q_COUNT; i++)
    {
        uint8_t lv;
        uint8_t act;
        uint8_t pct;

        if (i == (uint8_t)APP_ALM_Q_HW) { continue; }        /* 硬件层在 ③ 单独处理 */

        lv = alm_step((app_alm_q_t)i, alm_value((app_alm_q_t)i, r, d),
                      alm_input_valid((app_alm_q_t)i, r), now_ms, t_max);

        if (lv == 0u) { continue; }

        fault |= (uint16_t)(1u << s_meta[i].fault_bit);
        if (lv > level) { level = lv; }

        act = s_meta[i].action;
        pct = (lv >= 3u) ? 0u : ((lv == 2u) ? DERATE_L2_PCT : DERATE_L1_PCT);

        /* ★★ 2026-09-29 修：**切断只在三级发生** —— 原实现把"动作类型 = 断充电"直接当成了"现在就断"，
         *   于是**一级/二级也会切断充电**，正好把 §5.3 的"**两级降额、三级断开**"这条核心竖线写反了。
         *   ⚠️ 现场证据：离线注入自检**用例 2**（单体 3.55V = OV_L2）判失败（`s_selftest_rc = 0x0002`）。
         *   ⇒ 降额百分比**每级都算**（那是降额），而 `cut`/`latch` **只在 lv >= 3** 才置。 */
        if ((act == ACT_CUT_CHG) || (act == ACT_DERATE_BOTH))
        {
            if ((lv >= 3u) && (act == ACT_CUT_CHG))
            {
                cut |= 1u;
                if (s_meta[i].latch != 0u) { latch = 1u; }
            }
            if (pct < derate_chg) { derate_chg = pct; }
        }
        if ((act == ACT_CUT_DSG) || (act == ACT_DERATE_BOTH))
        {
            if ((lv >= 3u) && (act == ACT_CUT_DSG))
            {
                cut |= 2u;
                if (s_meta[i].latch != 0u) { latch = 1u; }
            }
            if (pct < derate_dis) { derate_dis = pct; }
        }
        if (pct < 100u) { any_derate = 1u; }
    }

    /* ---- ② ★ 0℃ 禁充（B 类）：动作力度 = 三级，但 **⛔ 不锁定**，且解除要"回升 ≥3℃ 且持续 60s" ---- */
    {
        uint8_t lv = (uint8_t)s_q[APP_ALM_Q_NOCHG].lvl;

        if (lv >= 3u)
        {
            if (t_max >= (int32_t)APP_BMS_T_NOCHG_REC_X10)
            {
                if (s_nochg_rec_on == 0u) { s_nochg_rec_on = 1u; s_nochg_rec_t = now_ms; }
                else if ((uint32_t)(now_ms - s_nochg_rec_t) >= (uint32_t)APP_BMS_T_NOCHG_REC_MS)
                {
                    s_q[APP_ALM_Q_NOCHG].lvl = 0u;      /* 恢复 */
                    s_q[APP_ALM_Q_NOCHG].t_lvl = now_ms;
                    s_nochg_rec_on = 0u;
                }
            }
            else { s_nochg_rec_on = 0u; }

            if (s_q[APP_ALM_Q_NOCHG].lvl >= 3u)
            {
                fault |= (uint16_t)(1u << s_meta[APP_ALM_Q_NOCHG].fault_bit);
                cut |= 1u;                          /* 断充电 */
                /* ⛔ 刻意**不置 latch**：塞进三级锁定会让"冬天早上充不上电"变成"要长按 5s 才能解锁" */
                if (level < 3u) { level = 3u; }
            }
        }
    }

    /* ---- ③ 硬件层（AFE 已动作）：只记录 + 置位 + 锁定，⛔ 不重复动作 ---- */
    alm_hw_apply(a, a, now_ms);
    if (s_q[APP_ALM_Q_HW].lvl >= 3u)
    {
        uint8_t hw = a->hw_fault_bits;

        if ((hw & 0x01u) != 0u) { fault |= (uint16_t)(1u << 4);  cut |= 1u; }   /* OCC ⇒ 充电方向 */
        if ((hw & 0x06u) != 0u) { fault |= (uint16_t)(1u << 5);  cut |= 2u; }   /* OCD/SCD ⇒ 放电方向 */
        fault |= (uint16_t)(1u << 6);
        level = 3u;
        latch = 1u;
    }

    /* ---- ④ 电流突变（L3，★ 用**原始值**）：方向由 ΔI 符号定 ---- */
    {
        uint8_t dir = 0u;

        if (alm_jump(r->curr_10ma, now_ms, &dir) != 0u)
        {
            fault |= (uint16_t)(1u << ((dir == 1u) ? 4u : 5u));
            cut |= (dir == 1u) ? 1u : 2u;
            latch = 1u;
            if (level < 3u) { level = 3u; }
        }

        s_i_prev = r->curr_10ma;
        s_i_prev_t = now_ms;
        s_i_prev_ok = 1u;
    }

    /* ---- ⑤ 多参数融合（FR-ALM-05 可实现子集；★ 只升不降） ---- */
    if ((s_q[APP_ALM_Q_T_CHG_OT].lvl >= 2u) && (s_q[APP_ALM_Q_I_CHG].lvl >= 2u) &&
        (s_q[APP_ALM_Q_T_RISE].lvl >= 2u))
    {
        /* (a) 高温 + 大电流 + 温升快 ⇒ 直接升三级（热失控前兆） */
        cut |= 1u;
        latch = 1u;
        level = 3u;
    }
    else if (((s_q[APP_ALM_Q_CELL_OV].lvl | s_q[APP_ALM_Q_CELL_UV].lvl |
               s_q[APP_ALM_Q_PACK_OV].lvl | s_q[APP_ALM_Q_PACK_UV].lvl) == 0u) &&
             (s_q[APP_ALM_Q_T_RISE].lvl >= 2u))
    {
        /* (b) 电压全部正常但温升速率异常 ⇒ 升二级 */
        if (level < 2u) { level = 2u; }
    }
    /* (c) "压差大 ∧ 温差大"：⛔ 无数值（PRD Q12）⇒ 不实现，写进 §6 差距声明 */

    /* ---- ⑥ 落帧 ---- */
    a->level        = level;
    a->fault_bits   = fault;
    a->request_open = cut;
    a->request_latch = latch;
    a->derate_chg_pct = derate_chg;
    a->derate_dis_pct = derate_dis;
    a->derate_state   = (any_derate != 0u) ? 1u : 0u;    /* ⚠️ 见文件末缺口登记：无超时值 ⇒ 只到"已请求未应答" */
    if (latch != 0u) { a->latch_reason = 1u; }           /* 1 = 越限型（环境型走 request_open 不锁定） */
    if ((s_q[APP_ALM_Q_NOCHG].lvl >= 3u) && (latch == 0u)) { a->latch_reason = 2u; }  /* 2 = 环境型 */

    alm_build_reason(a, r, d);
}

void app_alarm_levels(uint8_t *out, uint8_t n)
{
    uint8_t i;

    if (out == 0) { return; }

    for (i = 0u; (i < n) && (i < (uint8_t)APP_ALM_Q_COUNT); i++) { out[i] = s_q[i].lvl; }
}

uint16_t app_alarm_selftest_result(void)
{
    return s_selftest_rc;
}

/* ===================== ★★ 离线注入自检 =====================
 * 判据要的量此刻全都不存在（AFE/INA226/NTC 未接），但判据本体是**纯函数** ⇒
 * 在本板本固件上把合成 raw/derived 灌进去，逐条核对"等级 / 动作位 / 锁定"。
 * ⛔ 只覆盖判据逻辑：不覆盖真实采样、不覆盖执行器动作、不替代 F2 的台架故障注入。 */
static void alm_mk_in(bms_raw_t *r, bms_derived_t *d, int32_t cell_mv, int32_t cell_valid,
                      int32_t t_x10, int16_t i_10ma, int32_t rise_x10)
{
    memset(r, 0, sizeof(*r));
    memset(d, 0, sizeof(*d));

    r->cell_mv[0] = (uint16_t)cell_mv;
    r->cell_valid = (uint16_t)((cell_valid != 0) ? 0x0001u : 0x0000u);
    r->curr_10ma  = i_10ma;
    r->curr_valid = 1u;
    r->temp_x10[0] = (int16_t)t_x10;
    r->temp_valid  = 0x0001u;

    d->cell_max_mv = (uint16_t)cell_mv;
    d->cell_min_mv = (uint16_t)cell_mv;
    d->temp_max_x10 = (int16_t)t_x10;
    d->temp_min_x10 = (int16_t)t_x10;
    d->temp_delta_x10 = 0;
    d->temp_rise_x10_per_min = (int16_t)rise_x10;
}

uint16_t app_alarm_selftest(void)
{
    bms_raw_t   r;
    bms_derived_t d;
    bms_alarm_t a;
    uint16_t    n = 0u;
    uint32_t    t;

    /* --- 用例 1：单体 3.65V（≥ OV_L3 3600）⇒ 三级、断充电、锁定、原因串含 "cell" --- */
    n++;
    app_alarm_init();
    alm_mk_in(&r, &d, 3650, 1, 250, 0, 0);
    t = 0u;  memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t);
    t = 300u; memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t);
    if ((a.level != 3u) || ((a.request_open & 1u) == 0u) || (a.request_latch == 0u)) { s_selftest_rc = n; return n; }
    if (a.reason[0] == '\0') { s_selftest_rc = n; return n; }

    /* --- 用例 2：单体 3.55V（OV_L2）⇒ 二级、**不锁定**、降额 20% --- */
    n++;
    app_alarm_init();
    alm_mk_in(&r, &d, 3550, 1, 250, 0, 0);
    t = 0u;    memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t);
    t = 3200u; memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t);
    if ((a.level != 2u) || (a.request_latch != 0u)) { s_selftest_rc = n; return n; }

    /* --- 用例 3：解除要**逐级回落**且不限速（3.40V 低于 OV_L1−迟滞）⇒ 三次调用回 0 --- */
    n++;
    alm_mk_in(&r, &d, 3400, 1, 250, 0, 0);
    memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t + 100u);
    memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t + 200u);
    memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t + 300u);
    if (a.level != 0u) { s_selftest_rc = n; return n; }

    /* --- 用例 4：温度 56℃ ⇒ 充电过温三级（断充电）且原因串含 "chgOT" --- */
    n++;
    app_alarm_init();
    alm_mk_in(&r, &d, 3300, 1, 560, 0, 0);
    t = 0u;   memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t);
    t = 300u; memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t);
    if ((a.level != 3u) || ((a.request_open & 1u) == 0u)) { s_selftest_rc = n; return n; }
    if (strstr(a.reason, "chgOT") == 0) { s_selftest_rc = n; return n; }

    /* --- 用例 5：充电 5.2A ⇒ 三级但要等 1s（`I_HOLD_L3_MS` 与去抖取大）--- */
    n++;
    app_alarm_init();
    alm_mk_in(&r, &d, 3300, 1, 250, 520, 0);
    t = 0u;    memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t);
    t = 300u;  memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t);
    if (a.level >= 3u) { s_selftest_rc = n; return n; }        /* ⛔ 300ms 还不够 */
    t = 1200u; memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t);
    if ((a.level != 3u) || ((a.request_open & 1u) == 0u)) { s_selftest_rc = n; return n; }

    /* --- 用例 6：★ 0℃ 禁充 = 三级力度但 **⛔ 不锁定**（B 类）--- */
    n++;
    app_alarm_init();
    alm_mk_in(&r, &d, 3300, 1, 0, 0, 0);
    t = 0u;   memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t);
    t = 300u; memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t);
    if ((a.request_open & 1u) == 0u) { s_selftest_rc = n; return n; }   /* 必须断充电 */
    if (a.request_latch != 0u) { s_selftest_rc = n; return n; }         /* ⛔ 必须不锁定 */

    /* --- 用例 7：输入无效（cell_valid=0）⇒ ⛔ 不得据此升级 --- */
    n++;
    app_alarm_init();
    alm_mk_in(&r, &d, 3650, 0, 250, 0, 0);
    t = 0u;   memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t);
    t = 300u; memset(&a, 0, sizeof(a)); app_alarm_eval(&r, &d, &a, t);
    if (a.level != 0u) { s_selftest_rc = n; return n; }

    /* --- 用例 8：硬件层已动作 ⇒ 三级 + 锁定 + `hw_layer_state = 2`（ACTED）--- */
    n++;
    app_alarm_init();
    alm_mk_in(&r, &d, 3300, 1, 250, 0, 0);
    memset(&a, 0, sizeof(a));
    a.hw_fault_bits = 0x02u;                    /* SCD */
    app_alarm_eval(&r, &d, &a, 100u);
    if ((a.level != 3u) || (a.request_latch == 0u) || (a.hw_layer_state != 2u)) { s_selftest_rc = n; return n; }

    s_selftest_rc = 0u;
    return 0u;
}

/* ===================== ⚠️ 缺口登记（⛔ 不编数值） =====================
 * 1, **降额超时时长**：`PRD` §2.3 的 `FR-ALM-08` 写"二级 → 发降额请求并启动超时计时；超时未应答 → 升级为断开"，
 *    但 `PRD` 与 `app_config.h` 里**都没有这个超时值** ⇒ 本版 `derate_state` 只到 **1（已请求未应答）**，
 *    `2（已生效）`/`3（已升级断开）` **不落码**。⛔ 不自编一个"看起来合理"的秒数。
 * 2, **组过压/组欠压（§6.2 位 2/3）**：`PRD` §2.3 只有单体分档，**没有组压分档** ⇒ 本版 `K_DISABLED`。
 * 3, **"压差大 ∧ 温差大"融合（`FR-ALM-05` 第三条）**：`PRD` **Q12** 无数值 ⇒ 不实现。
 * 4, **多测点温度**：本版 `K_T_MAX/K_T_MIN` 取的是**电芯表面**那一路（`temp_valid` 位 0）；
 *    多测点取极值要等 NTC 两路都接上（`Build.md` D11）。
 * ⇒ 以上四条都必须出现在 `PRD.md` §6 的差距声明里（本轮已登记，⛔ 不得算作"已实现"）。
 */
