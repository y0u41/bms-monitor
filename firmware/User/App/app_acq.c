/**
 ****************************************************************************************************
 * @file        app_acq.c
 * @brief       采集层：SHT30 环境温湿度 → 快照的 raw 盒（`Tech_Design.md` §4.2）
 * @note        ⛔ 本文件不得出现：告警判断、SOC 计算、Flash 写、执行器 GPIO、AFE 寄存器名（S15/§4.2）。
 * @note        ★ 滤波属主：本文件是"采集调度**与滤波**"的同一处（§4.2 职责表），
 *              `filt_lpf_step()` 就是 FR-ACQ-06 的实现原语，D7 的单体电压、D10 的电流必须复用它，
 *              ⛔ 不要在 `app_soc`/界面里再补一层（两处各滤一次 = 群延迟叠加，200ms 预算账就不成立了）。
 * @note        ★ 通道所有权：本文件目前**只**写 `temp_x10[APP_TEMP_IDX_ENV]` 与它的时间戳；
 *              其余 raw 字段仍由 D2 的假数据源 `app_data_fake_step()` 填写（各通道落点见 `app_acq.h`）。
 *              交接机制不是"开关"而是**顺序**：一个节拍内先跑假数据源、后跑 `app_acq_step()`，
 *              而 `app_data_raw_write()` 返回的是"上一已发布帧的副本" ⇒ 真实通道天然覆盖假通道，
 *              未接入的通道原样延续。⏳ 全部通道接入后删除假数据源（`Build.md` D2 口径）。
 ****************************************************************************************************
 */

#include "./App/app_acq.h"
#include "./App/app_config.h"
#include "./App/app_data.h"
#include "./BSP/SHT30/sht30.h"
#include "./BSP/AFE/afe.h"           /* ★ D7 交接：AFE 单体/组压 */
#include "./BSP/INA226/ina226.h"      /* ★ D10 交接：电流 + 冗余组压 */
#include "./BSP/ADC/adc.h"           /* ★ D11 交接：ADC1（PA5/PA6） */
#include "./BSP/NTC/ntc.h"           /* ★ D11 交接：NTC 查表换算 */
#include <stdio.h>

/* ============================ 定点一阶低通（FR-ACQ-06 的落地原语） ============================
 * y += (x - y) / 2^S ；累加器 yq 多带 S 位小数 ⇒ 移位只丢在小数位上，末位不会卡死（无死区）。
 * 只做移位/加减，⛔ 无浮点、无乘法/除法（§2.2 + M3 无硬件除法器）。
 *
 * ★ 为什么必须"首拍置入"：yq 从 0 起步时，25.0℃ 要按 1/2 逐拍逼近，前几拍会读出
 *   0.0 → 12.5 → 18.8℃ 这种**并不存在**的低温 —— 环境报警的低温判据据此能在上电瞬间
 *   误报一次。所以 primed=0 时直接取入 x，不做逼近。传感器判死→重新在位时同样重置。
 */
typedef struct
{
    int32_t yq;         /* y << APP_FILT_LPF_SHIFT */
    uint8_t primed;
} filt_lpf_t;

static void filt_lpf_reset(filt_lpf_t *f)
{
    f->yq     = 0;
    f->primed = 0u;
}

static int16_t filt_lpf_step(filt_lpf_t *f, int16_t x)
{
    int32_t d;
    int32_t r;

    if (f->primed == 0u)
    {
        f->yq     = (int32_t)x << APP_FILT_LPF_SHIFT;
        f->primed = 1u;

        return x;
    }

    d = ((int32_t)x << APP_FILT_LPF_SHIFT) - f->yq;

    /* 负数右移是实现定义行为 ⇒ 先取绝对值再补符号（d == INT32_MIN 不可能：|d| ≤ 量程 × 2^S） */
    r = (d >= 0) ? (d >> APP_FILT_LPF_SHIFT) : (-((int32_t)-d >> APP_FILT_LPF_SHIFT));

    f->yq += r;

    /* 输出四舍五入到 0.1 单位；小数位仍留在 yq 里，下一拍继续逼近 */
    return (int16_t)((f->yq + ((int32_t)1 << (APP_FILT_LPF_SHIFT - 1))) >> APP_FILT_LPF_SHIFT);
}

/* =========================================================================================== */

static uint8_t  s_env_present;      /* 1 = 器件在位（探测成功过且未被判死） */
static uint8_t  s_env_valid;        /* 1 = 至少有一帧好数据 */
static uint8_t  s_env_fail;         /* 连续失败次数 */
static int16_t  s_env_temp_x10;
static uint16_t s_env_humi_x10;
static uint32_t s_env_next;

/* ★ D7 交接的独立节拍：环境是慢量（`APP_ACQ_ENV_PERIOD_MS`），
 *   而单体/组压要跟采集拍（`APP_PERIOD_ACQ_MS` = 100ms）—— 两者合用一个节拍会让
 *   电压通道慢 10 倍，而三级告警的确认窗只有 200ms。 */
static uint32_t s_afe_next;
static uint32_t s_ina_next;
static uint32_t s_ntc_next;
static volatile uint16_t s_ntc_try;
static volatile uint16_t s_ntc_ok;
static uint16_t s_ntc_last[2];
static uint8_t  s_ntc_have[2];
static uint8_t  s_ntc_cnt[2];       /* ★ 连续稳定次数（不足 `APP_ACQ_NTC_STABLE_N` 不发布） */
static volatile uint16_t s_ina_try;
static volatile uint16_t s_ina_ok;

/* ★ 取证出口（⛔ 不参与判据）：交接点被调多少次、成功多少次。
 *   为什么要它：AFE 未接时本函数行为上**零影响**（一个字都不写），
 *   所以光看快照分不出它到底跑没跑 ⇒ 必须有一个可读的"试了 N 次、成功 0 次"。 */
static volatile uint16_t s_afe_try;
static volatile uint16_t s_afe_ok;         /* 下次动作的时刻（限流） */

static filt_lpf_t s_ftemp;          /* 环境温度滤波态 */
static filt_lpf_t s_fhumi;          /* 环境湿度滤波态 */

/**
 * @brief       采集层初始化：探测环境传感器
 */
void app_acq_init(void)
{
    s_env_present = (uint8_t)((sht30_probe() == SHT30_OK) ? 1u : 0u);
    s_env_valid   = 0u;
    s_env_fail    = 0u;
    s_env_next    = 0u;

    filt_lpf_reset(&s_ftemp);
    filt_lpf_reset(&s_fhumi);

    printf("ACQ  : env sensor %s (SHT30@0x44 on g_bus_sens), lpf 1/%u\r\n",
           s_env_present ? "present" : "absent",
           (unsigned)(1u << APP_FILT_LPF_SHIFT));
}

/**
 * @brief       把一个可信帧写进 raw 盒（只动环境通道，其余字段原样延续）
 * @note        入盒的是**滤波后**值：本通道的消费者是环境报警与界面，不是 BMS 保护判据
 *              （PRD:193 的"原始值喂保护"约束落在 D7/D10 的电压、电流通道上，见 `app_config.h` §12.11）。
 */
static void env_publish(int16_t t_x10, uint16_t h_x10, uint32_t now_ms)
{
    bms_raw_t *r = app_data_raw_write();

    t_x10 = filt_lpf_step(&s_ftemp, t_x10);

    s_env_temp_x10 = t_x10;
    /* 湿度量程 0~100.0%RH ⇒ x10 ∈ [0,1000]，整条通路都在 int16 正半区内，可直接共用同一个滤波器 */
    s_env_humi_x10 = (uint16_t)filt_lpf_step(&s_fhumi, (int16_t)h_x10);
    s_env_valid    = 1u;

    r->temp_x10[APP_TEMP_IDX_ENV] = t_x10;
    r->temp_valid |= (uint16_t)(1u << APP_TEMP_IDX_ENV);
    r->tick_ms_temp[APP_TEMP_IDX_ENV] = now_ms;     /* ★ 只推进**本测点**那一条（C-98） */

    app_data_raw_commit();
}

/**
 * @brief       失败处理：连续失败够多次就把器件判"不在位"，下个节拍重新探测
 * @note        ★ 这里**不**清 `temp_valid`、也**不**倒退 `tick_ms_temp[ENV]` —— 让旧值留在帧里，
 *              由 `acq_is_fresh()` 在 `APP_DATA_STALE_MS` 之后把 `BMS_LINK_SHT30` 置起来。
 *              判据"读不到就立刻抹成无效"会让界面闪一下 "----"，而"变陈旧"才是既不误清也不冒充的口径。
 *              ⛔ 本函数**只**推进环境测点那一条时间戳：电芯 NTC 的新鲜度属主是 D11，
 *              借这条心跳会让 `BMS_LINK_NTC`（= "NTC 失效 → 禁止充电"的判据）永不亮（审查 C-98）。
 */
static void env_fail(void)
{
    if (++s_env_fail >= APP_TH_RETRY)
    {
        s_env_fail    = 0u;
        s_env_present = 0u;
        s_env_valid   = 0u;     /* 取值口转"不可信"：遗留报警通路据此报 th_ok=0 */
    }
}

/**
 * @brief       采集节拍
 */
void app_acq_step(uint32_t now_ms)
{
    int16_t  t_x10 = 0;
    uint16_t h_x10 = 0;
    uint8_t  r;

    if ((uint32_t)(now_ms - s_env_next) < (uint32_t)APP_ACQ_ENV_PERIOD_MS)
    {
        return;
    }

    s_env_next = now_ms;

    if (!s_env_present)
    {
        /* 重新探测：与 `th_detect()` 一样只是发地址，⛔ 不在这里初始化总线（共享资源，C-39） */
        if (sht30_probe() == SHT30_OK)
        {
            s_env_present = 1u;
            s_env_fail    = 0u;

            /* 判死到重新在位之间隔了多久是未知的，旧状态一律作废（下一拍重新置入）。
             * 不清的话会把"停机前的最后读数"继续往新值逼近，等于凭空补一段假数据。 */
            filt_lpf_reset(&s_ftemp);
            filt_lpf_reset(&s_fhumi);

            printf("ACQ  : env sensor back online\r\n");
        }

        return;
    }

    /* 两阶段读法：首拍只发命令返回 BUSY，次拍才有数据（S12，转换窗 20ms 远小于本节拍 500ms） */
    r = sht30_measure(&t_x10, &h_x10);

    if (r == SHT30_OK)
    {
        s_env_fail = 0u;
        env_publish(t_x10, h_x10, now_ms);
    }
    else if (r != SHT30_BUSY)
    {
        env_fail();
    }
}

/**
 * @brief       ★★ D7 交接：AFE 单体电压 + 组压 → 快照的 raw 盒
 * @note        只在两读都成功时才写（见头文件声明）。⛔ 本函数不判告警、不算 SOC。
 */
void app_acq_afe_step(uint32_t now_ms)
{
    uint16_t mv[APP_CELL_COUNT_MAX];
    uint16_t pack_10mv = 0u;
    uint8_t  i;
    bms_raw_t *r;

    if ((uint32_t)(now_ms - s_afe_next) < (uint32_t)APP_PERIOD_ACQ_MS)
    {
        return;
    }

    s_afe_next = now_ms;

    if (s_afe_try < 0xFFFFu) { s_afe_try++; }

    if (afe_read_cells(mv, (uint8_t)APP_CELL_COUNT) != AFE_OK)
    {
        return;                     /* ★ 失败 ⇒ **一个字都不动**（旧值继续老化、链路位自然亮起） */
    }

    if (afe_read_pack(&pack_10mv) != AFE_OK)
    {
        return;
    }

    r = app_data_raw_write();

    for (i = 0u; i < (uint8_t)APP_CELL_COUNT; i++)
    {
        r->cell_mv[i] = mv[i];
    }

    /* ★ 只置**已接入那几串**的有效位；高位保持 0（="未接入"，不是"不可信"） */
    r->cell_valid = (uint16_t)((1u << (uint8_t)APP_CELL_COUNT) - 1u);

    r->pack_10mv  = pack_10mv;

    /* ★ 时间戳只在**本通道真读到**时推进（§4.4 新鲜度） */
    r->tick_ms_cell = now_ms;

    if (s_afe_ok < 0xFFFFu) { s_afe_ok++; }

    app_data_raw_commit();
}

/**
 * @brief       ★★ D10 交接：INA226 电流 + 冗余组压 → 快照的 raw 盒
 * @note        只在 `ina226_read()` 成功时才写（同 `app_acq_afe_step()`）。
 *              ★ 节拍 = `APP_PERIOD_ACQ_MS`（100ms），远大于驱动自己的
 *              `APP_INA226_REFRESH_US`（约 8.8ms）⇒ 不会把转换完成标志抢掉。
 */
void app_acq_ina_step(uint32_t now_ms)
{
    int16_t   curr_10ma = 0;
    uint16_t  vbus_10mv = 0u;
    bms_raw_t *r;

    if ((uint32_t)(now_ms - s_ina_next) < (uint32_t)APP_PERIOD_ACQ_MS)
    {
        return;
    }

    s_ina_next = now_ms;

    if (s_ina_try < 0xFFFFu) { s_ina_try++; }

    if (ina226_read(&curr_10ma, &vbus_10mv) != INA226_OK)
    {
        return;                     /* ★ 失败 ⇒ 一个字都不动 */
    }

    r = app_data_raw_write();

    r->curr_10ma     = curr_10ma;
    r->curr_valid    = 1u;
    r->pack_10mv_aux = vbus_10mv;
    r->tick_ms_curr  = now_ms;      /* ★ 同一颗器件的同一次读 ⇒ 两路共用时间戳 */

    if (s_ina_ok < 0xFFFFu) { s_ina_ok++; }

    app_data_raw_commit();
}

/**
 * @brief       ★★ D11 交接：两路电芯表面 NTC → 快照（见 `app_acq.h` 的四条纪律）
 */
void app_acq_ntc_step(uint32_t now_ms)
{
    static const uint8_t s_ch[2] = { ADC_CH_NTC1, ADC_CH_NTC2 };
    static const uint8_t s_ix[2] = { APP_TEMP_IDX_CELL_NTC, APP_TEMP_IDX_CELL_NTC2 };
    bms_raw_t *r = NULL;
    uint8_t  k;

    if ((uint32_t)(now_ms - s_ntc_next) < (uint32_t)APP_PERIOD_ACQ_MS)
    {
        return;
    }

    s_ntc_next = now_ms;

    if (s_ntc_try < 0xFFFFu) { s_ntc_try++; }

    for (k = 0u; k < 2u; k++)
    {
        int16_t  t_x10 = 0;
        uint16_t code  = adc_read_ch(s_ch[k]);
        uint16_t diff;

        if (ntc_code_to_temp_x10(code, &t_x10) != NTC_OK)
        {
            s_ntc_have[k] = 0u;     /* ★ 超量程（真板上 = NTC 开路）⇒ 不写，且下次重新建立历史 */
            s_ntc_cnt[k]  = 0u;
            continue;
        }

        /* ★★ 第二形态：**稳定性**。首次不发布；之后两次相邻读数差超限 ⇒ 当作“
         *   这不是一个真测点”（本板未接采样板时悬空引脚就是这样，码值上百码地跳）。 */
        if (s_ntc_have[k] == 0u)
        {
            s_ntc_last[k] = code;
            s_ntc_have[k] = 1u;
            s_ntc_cnt[k]  = 0u;
            continue;
        }

        diff = (uint16_t)((code > s_ntc_last[k]) ? (code - s_ntc_last[k]) : (s_ntc_last[k] - code));

        if (diff > (uint16_t)APP_ACQ_NTC_STABLE_CODES)
        {
            s_ntc_last[k] = code;
            s_ntc_cnt[k]  = 0u;     /* 跳变 ⇒ 历史重建，连续计数归零 */
            continue;               /* ⛔ 不写 */
        }

        s_ntc_last[k] = code;

        if (s_ntc_cnt[k] < 0xFFu) { s_ntc_cnt[k]++; }

        /* ★★ 连续不足 N 次 ⇒ **不发布**（悬空引脚偶尔能凑出两次相近，凑不出三次） */
        if (s_ntc_cnt[k] < (uint8_t)APP_ACQ_NTC_STABLE_N) { continue; }

        if (r == NULL) { r = app_data_raw_write(); }

        r->temp_x10[s_ix[k]] = t_x10;
        r->temp_valid |= (uint16_t)(1u << s_ix[k]);
        r->tick_ms_temp[s_ix[k]] = now_ms;      /* ★ 只推进**本测点**那一条（C-98） */

        if (s_ntc_ok < 0xFFFFu) { s_ntc_ok++; }
    }

    if (r != NULL) { app_data_raw_commit(); }
}

void app_acq_ntc_stat(uint16_t *try_n, uint16_t *ok_n)
{
    if (try_n != NULL) { *try_n = s_ntc_try; }
    if (ok_n  != NULL) { *ok_n  = s_ntc_ok; }
}

void app_acq_ina_stat(uint16_t *try_n, uint16_t *ok_n)
{
    if (try_n != NULL) { *try_n = s_ina_try; }
    if (ok_n  != NULL) { *ok_n  = s_ina_ok; }
}

void app_acq_afe_stat(uint16_t *try_n, uint16_t *ok_n)
{
    if (try_n != NULL) { *try_n = s_afe_try; }
    if (ok_n  != NULL) { *ok_n  = s_afe_ok; }
}

int16_t app_acq_env_temp_x10(void)
{
    return s_env_temp_x10;
}

uint16_t app_acq_env_humi_x10(void)
{
    return s_env_humi_x10;
}

uint8_t app_acq_env_valid(void)
{
    return s_env_valid;
}
