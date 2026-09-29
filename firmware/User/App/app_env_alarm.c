/**
 ****************************************************************************************************
 * @note        ★★ 2026-09-29 由 `app_alarm.c` **原样搬来**（`C-38` 的执行）：本文件 = **环境两级**，
 *              `app_alarm.c` = **BMS 三级**。⛔ 两套门限不得再混在一个模块里。
 *              ⚠️ 本文件的 `alarm_*` API 名与语义一字未改 ⇒ 调用方（`app_main.c` / `app_ui.c`）
 *              只改 include，逻辑零改动。
 * @file        app_env_alarm.c
 * @brief       超限报警逻辑
 * @note        时序(全部由 app_config.h 配置):
 *                1, 采样时发现任一参数越界 -> 立刻开始报警, 蜂鸣器连续响 APP_ALARM_TIME_MS(10s);
 *                2, 10 秒到 -> 自动停, 并静默 APP_ALARM_GAP_MS(默认 10s); 若仍越界则再响 10s, 如此反复;
 *                3, 报警期间按 KEY0 -> 立即停, 并"消音"到所有参数恢复正常为止
 *                   (否则 0.5 秒后又会响, KEY0 就没意义了);
 *                4, 全部参数回到正常范围 -> 清除消音标志, 下次越界可再次报警。
 *
 * @note        误报抑制(2026-09 增加, 触发阈值未做任何改动):
 *                a, 持续确认窗 APP_ALARM_CONFIRM_MS: 越界要连续保持一段时间才确认, 滤掉瞬时尖峰;
 *                b, 迟滞 APP_ENV_HYST_xxx: 已在报警时, 恢复边界相对触发边界内收,
 *                   避免数值卡在阈值附近导致蜂鸣器反复启停。
 * @note        ★ 本模块的语义是**环境两级**（危险/舒适），电芯三级门限一律 APP_BMS_*、
 *              归 D14 重写后的 app_protect/app_alarm；两侧迟滞不得同宏（C-38 / S7）。
 ****************************************************************************************************
 */

#include "./App/app_env_alarm.h"
#include "./App/app_config.h"
#include "./BSP/BEEP/beep.h"
#include "./BSP/KEY/key.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>

/* 某一侧不做判断的哨兵值(光照阈值允许填 0 表示该方向不判断) */
#define ALARM_NO_LOW            INT32_MIN
#define ALARM_NO_HIGH           INT32_MAX

/* 单通道的判定结果 */
#define ALARM_LV_OK             0       /* 在舒适区内 */
#define ALARM_LV_WARN           1       /* 超出舒适区, 但还在危险区内 -> 只提示 */
#define ALARM_LV_FAULT          2       /* 超出危险区 -> 报警 */

static uint8_t  g_fault;            /* ALARM_FAULT_xxx 位或(危险区, 会响) */
static uint8_t  g_warn;             /* ALARM_FAULT_xxx 位或(舒适区外, 只黄字) */
static uint8_t  g_beeping;          /* 蜂鸣器正在响 */
static uint8_t  g_muted;            /* 已被 KEY0 消音, 等恢复正常后自动解除 */
static uint32_t g_start;            /* 本次报警开始时刻 */
static uint32_t g_next_allow;       /* 允许再次报警的最早时刻 */
static char     g_reason[64];
static uint8_t  g_hold_on[3];       /* 各参数"连续越界计时"是否在跑: 0=温度 1=湿度 2=光照 */
static uint32_t g_hold_t[3];        /* 各参数连续越界的起始时刻 */

/* 报警开始时把"快照"交给 app_log 存历史(解耦: app_alarm 不知道历史怎么存)。
 * 由 app_main 在初始化时注册; 传 NULL 表示不记录。 */
static void (*g_log_hook)(uint8_t faults, int16_t temp_x10,
                          uint16_t humi_x10, uint32_t lux_cdm2) = NULL;

/**
 * @brief       初始化(蜂鸣器 + KEY0)
 * @param       无
 * @retval      无
 */
void alarm_init(void)
{
    beep_init();
    key_init();

    g_fault = 0;
    g_warn = 0;
    g_beeping = 0;
    g_muted = 0;
    g_start = 0;
    g_next_allow = 0;

    memset(g_hold_on, 0, sizeof(g_hold_on));
    memset(g_hold_t, 0, sizeof(g_hold_t));

    strcpy(g_reason, "normal");
}

/**
 * @brief       注册"报警开始时"的回调(交给 app_log 存历史)
 * @param       hook: 回调(传 NULL 取消); 参数为危险区标志与当前光照值
 */
void alarm_set_log_hook(void (*hook)(uint8_t faults, int16_t temp_x10,
                                     uint16_t humi_x10, uint32_t lux_cdm2))
{
    g_log_hook = hook;
}

/**
 * @brief       往原因字符串尾部追加一个记号(用 '+' 连接)
 */
static void alarm_join(char *dst, const char *tok)
{
    size_t n = strlen(dst);

    if ((n + strlen(tok) + 2) > 64)
    {
        return;                                     /* 放不下就丢弃, 不做越界写 */
    }

    if (n != 0)
    {
        dst[n++] = '+';
        dst[n] = '\0';
    }

    strcat(dst, tok);
}

/**
 * @brief       拼出显示用的原因字符串
 * @note        危险区用大写记号(TEMP/HUMI/LIGHT), 舒适区外用小写 warn 记号(warnT/warnH/warnL),
 *              一眼能分清"在响"和"只是提示"。
 */
static void alarm_build_reason(void)
{
    g_reason[0] = '\0';

    if ((g_fault & ALARM_FAULT_TEMP) != 0)  alarm_join(g_reason, "TEMP");
    if ((g_fault & ALARM_FAULT_HUMI) != 0)  alarm_join(g_reason, "HUMI");
    if ((g_fault & ALARM_FAULT_LUX)  != 0)  alarm_join(g_reason, "LIGHT");

    if ((g_warn & ALARM_FAULT_TEMP) != 0)   alarm_join(g_reason, "warnT");
    if ((g_warn & ALARM_FAULT_HUMI) != 0)   alarm_join(g_reason, "warnH");
    if ((g_warn & ALARM_FAULT_LUX)  != 0)   alarm_join(g_reason, "warnL");

    if (g_reason[0] == '\0')
    {
        strcpy(g_reason, "normal");
    }
    else if (g_muted)
    {
        alarm_join(g_reason, "mute");
    }
}

/**
 * @brief       单个通道的两级判定: 持续确认窗 + 迟滞恢复
 * @param       idx  : 0=温度 1=湿度 2=光照(各自独立的确认窗计时器)
 * @param       v    : 当前值(与各阈值同单位)
 * @param       d_lo : 危险区下限, ALARM_NO_LOW 表示不判断
 * @param       d_hi : 危险区上限, ALARM_NO_HIGH 表示不判断
 * @param       c_lo : 舒适区下限
 * @param       c_hi : 舒适区上限
 * @param       hyst : 迟滞量(只在"已经处于某一级"时用于内收该级的恢复边界)
 * @param       bit  : ALARM_FAULT_xxx
 * @param       now  : 当前时刻
 * @retval      ALARM_LV_OK / ALARM_LV_WARN / ALARM_LV_FAULT
 * @note        必须在 g_fault / g_warn 更新之前调用 —— 迟滞要依赖上一轮的状态。
 */
static uint8_t alarm_eval(uint8_t idx, int32_t v, int32_t d_lo, int32_t d_hi,
                          int32_t c_lo, int32_t c_hi, int32_t hyst,
                          uint8_t bit, uint32_t now)
{
    uint8_t danger;
    uint8_t outside;

    if ((g_fault & bit) != 0)                   /* 已经在危险区: 危险边界内收 */
    {
        if (d_lo != ALARM_NO_LOW)  d_lo += hyst;
        if (d_hi != ALARM_NO_HIGH) d_hi -= hyst;
    }
    else if ((g_warn & bit) != 0)               /* 已经在舒适区外: 舒适边界内收 */
    {
        if (c_lo != ALARM_NO_LOW)  c_lo += hyst;
        if (c_hi != ALARM_NO_HIGH) c_hi -= hyst;
    }

    /* 注意: 危险判定永远用"危险边界", 舒适判定永远用"舒适边界"。
     * 两级是包含关系(舒适区 ⊂ 危险区), 所以先有危险、后有舒适, 不会互相串。 */
    danger  = ((v < d_lo) || (v > d_hi)) ? 1 : 0;
    outside = (uint8_t)(danger | (((v < c_lo) || (v > c_hi)) ? 1 : 0));

    if (outside == 0)                           /* 回到舒适区内: 立即清计时 */
    {
        g_hold_on[idx] = 0;
        return ALARM_LV_OK;
    }

    if (g_hold_on[idx] == 0)                    /* 刚开始越界: 只起计时, 先不报 */
    {
        g_hold_on[idx] = 1;
        g_hold_t[idx] = now;
    }

    if ((uint32_t)(now - g_hold_t[idx]) >= (uint32_t)APP_ALARM_CONFIRM_MS)
    {
        return danger ? ALARM_LV_FAULT : ALARM_LV_WARN;
    }

    return ALARM_LV_OK;
}

/**
 * @brief       喂入一次采样结果
 * @param       temp_x10: 温度, 0.1℃
 * @param       humi_x10: 湿度, 0.1%RH
 * @param       lux_cdm2: 光照, cd/m2
 * @param       th_valid: 温湿度读数是否有效(0 = 读失败, 不参与判断)
 * @retval      无
 */
/**
 * @brief       把单通道判定结果折进 fault / warn 位图
 */
static void alarm_apply(uint8_t lv, uint8_t bit, uint8_t *fault, uint8_t *warn)
{
    if (lv == ALARM_LV_FAULT)
    {
        *fault |= bit;
    }
    else if (lv == ALARM_LV_WARN)
    {
        *warn |= bit;
    }
}

/**
 * @brief       喂入一次采样结果
 * @param       temp_x10: 温度, 0.1℃
 * @param       humi_x10: 湿度, 0.1%RH
 * @param       lux_cdm2: 光照, cd/m2
 * @param       th_valid: 温湿度读数是否有效(0 = 读失败, 不参与判断)
 * @retval      无
 */
void alarm_feed(int16_t temp_x10, uint16_t humi_x10, uint32_t lux_cdm2, uint8_t th_valid)
{
    uint32_t now = HAL_GetTick();
    uint8_t  fault = 0;
    uint8_t  warn  = 0;
    uint8_t  lv;
    int32_t  lx_dlo = ALARM_NO_LOW;
    int32_t  lx_dhi = ALARM_NO_HIGH;
    int32_t  lx_clo = ALARM_NO_LOW;
    int32_t  lx_chi = ALARM_NO_HIGH;

    if (th_valid != 0)
    {
        lv = alarm_eval(0, (int32_t)temp_x10,
                        APP_ENV_TEMP_MIN_X10, APP_ENV_TEMP_MAX_X10,
                        APP_ENV_COMFORT_TEMP_MIN_X10, APP_ENV_COMFORT_TEMP_MAX_X10,
                        APP_ENV_HYST_TEMP_X10, ALARM_FAULT_TEMP, now);
        alarm_apply(lv, ALARM_FAULT_TEMP, &fault, &warn);

        lv = alarm_eval(1, (int32_t)humi_x10,
                        APP_ENV_HUMI_MIN_X10, APP_ENV_HUMI_MAX_X10,
                        APP_ENV_COMFORT_HUMI_MIN_X10, APP_ENV_COMFORT_HUMI_MAX_X10,
                        APP_ENV_HYST_HUMI_X10, ALARM_FAULT_HUMI, now);
        alarm_apply(lv, ALARM_FAULT_HUMI, &fault, &warn);
    }
    else
    {
        g_hold_on[0] = 0;                           /* 传感器读失败: 不据此报警(与旧版一致) */
        g_hold_on[1] = 0;
    }

#if (APP_ENV_LUX_MIN_CDM2 > 0)
    lx_dlo = APP_ENV_LUX_MIN_CDM2;
#endif
#if (APP_ENV_LUX_MAX_CDM2 > 0)
    lx_dhi = APP_ENV_LUX_MAX_CDM2;
#endif
#if (APP_ENV_COMFORT_LUX_MIN_CDM2 > 0)
    lx_clo = APP_ENV_COMFORT_LUX_MIN_CDM2;
#endif
#if (APP_ENV_COMFORT_LUX_MAX_CDM2 > 0)
    lx_chi = APP_ENV_COMFORT_LUX_MAX_CDM2;
#endif

    if ((lx_dlo != ALARM_NO_LOW) || (lx_dhi != ALARM_NO_HIGH) ||
        (lx_clo != ALARM_NO_LOW) || (lx_chi != ALARM_NO_HIGH))
    {
        lv = alarm_eval(2, (int32_t)lux_cdm2, lx_dlo, lx_dhi, lx_clo, lx_chi,
                        APP_ENV_HYST_LUX_CDM2, ALARM_FAULT_LUX, now);
        alarm_apply(lv, ALARM_FAULT_LUX, &fault, &warn);
    }
    else
    {
        g_hold_on[2] = 0;
    }

    g_fault = fault;                                /* 注意: 必须在上面全部判定完之后再更新 */
    g_warn  = warn;

    if ((g_fault == 0) && (g_warn == 0))
    {
        g_muted = 0;                                /* 完全回到舒适区, 解除消音 */
    }

    if (g_fault == 0)
    {
        g_beeping = 0;                              /* 危险区解除, 停响 */
    }
    else if ((g_beeping == 0) && (g_muted == 0) &&
             ((int32_t)(now - g_next_allow) >= 0))
    {
        g_beeping = 1;                              /* 开始一次 10 秒报警 */
        g_start = now;
        printf("ALARM: start (%u)\r\n", (unsigned)g_fault);

        if (g_log_hook != NULL)
        {
            g_log_hook(g_fault, temp_x10, humi_x10, lux_cdm2);  /* 记录一条报警历史(app_log) */
        }
    }

    alarm_build_reason();
}

/**
 * @brief       报警任务: 计时停响 + 驱动蜂鸣器(每 20ms 调一次)
 * @param       无
 * @retval      无
 * @note        按键扫描已经挪到 app_main.c 的按键任务里(因为 KEY1 要用来翻报警历史页),
 *              这里只负责"10 秒到就停"和把蜂鸣器电平打出去。
 */
void alarm_task(void)
{
    uint32_t now = HAL_GetTick();

    if (g_beeping && ((now - g_start) >= APP_ALARM_TIME_MS))    /* 10 秒到, 自动停 */
    {
        g_beeping = 0;
        g_next_allow = now + APP_ALARM_GAP_MS;
        printf("ALARM: %u ms elapsed, stop\r\n", (unsigned)APP_ALARM_TIME_MS);
        alarm_build_reason();
    }

    BEEP(g_beeping ? 1 : 0);
}

/**
 * @brief       KEY0 消音(由 app_main 的按键任务调用)
 * @param       无
 * @retval      1 = 这次真的消音了; 0 = 当前没在响, 什么都没做
 * @note        消音会一直保持到所有参数回到**舒适区**为止, 否则 0.5 秒后新一轮采样发现
 *              仍越界又会响, KEY0 等于没用。
 */
uint8_t alarm_key_mute(void)
{
    if (g_beeping == 0)
    {
        return 0;
    }

    g_beeping    = 0;
    g_muted      = 1;
    g_next_allow = HAL_GetTick() + APP_ALARM_GAP_MS;

    printf("ALARM: muted by KEY0\r\n");
    alarm_build_reason();
    BEEP(0);

    return 1;
}

/**
 * @brief       当前危险区标志
 */
uint8_t alarm_get_faults(void)
{
    return g_fault;
}

/**
 * @brief       当前舒适区标志(只提示, 不响)
 */
uint8_t alarm_get_warns(void)
{
    return g_warn;
}

/**
 * @brief       蜂鸣器是否正在响
 */
uint8_t alarm_is_beeping(void)
{
    return g_beeping;
}

/**
 * @brief       报警原因字符串
 */
const char *alarm_reason(void)
{
    return g_reason;
}
