/**
 ****************************************************************************************************
 * @file        rtc.c
 * @brief       RTC 实时时钟驱动(带后备电池时掉电继续走)
 * @note        时钟换算(年月日/闰年/星期)与秒数算法移植自正点原子官方 HAL 例程「实验15 RTC实验」;
 *              初始化部分改成直接写 RCC->BDCR, 原因见 rtc_init() 的注释。
 ****************************************************************************************************
 */

#include "./BSP/RTC/rtc.h"
#include "./SYSTEM/delay/delay.h"
#include <stdio.h>

/* 后备寄存器分工(本工程约定):
 *   BKP0: 0x5050 = RTC 时钟源为 LSE, 0x5051 = 为 LSI(与官方一致, 用来判断是否配置过)
 *   BKP1: 0x5A5A = 时间被真正设置过(说明后备电池在供电 / 或用户手动设过) */
#define RTC_BKP_SRC_LSE     0x5050
#define RTC_BKP_SRC_LSI     0x5051
#define RTC_BKP_SET_FLAG    0x5A5A

RTC_HandleTypeDef g_rtc_handle;     /* RTC 控制句柄 */
_calendar_obj calendar;             /* 时钟结构体 */

/* --- 前置声明(这几个都是 .c 内部的静态函数, 定义在文件后部) --- */
static uint8_t rtc_is_leap_year(uint16_t year);
static long    rtc_date2sec(uint16_t syear, uint8_t smon, uint8_t sday,
                            uint8_t hour, uint8_t min, uint8_t sec);

/**
 * @brief       RTC 写入后备区域 SRAM
 * @param       bkrx: 后备区寄存器编号, 范围 0~41 (对应 RTC_BKP_DR1~RTC_BKP_DR42)
 * @param       data: 要写入的 16 位数据
 * @retval      无
 */
void rtc_write_bkr(uint32_t bkrx, uint16_t data)
{
    HAL_PWR_EnableBkUpAccess();                             /* 取消备份区写保护 */
    HAL_RTCEx_BKUPWrite(&g_rtc_handle, bkrx + 1, data);
}

/**
 * @brief       RTC 读取后备区域 SRAM
 * @param       bkrx: 后备区寄存器编号, 范围 0~41
 * @retval      读到的 16 位数据
 */
uint16_t rtc_read_bkr(uint32_t bkrx)
{
    return (uint16_t)HAL_RTCEx_BKUPRead(&g_rtc_handle, bkrx + 1);
}

/**
 * @brief       判断时间是否被真正设置过(说明后备电池在供电)
 * @retval      1 = 是; 0 = 没有(每次上电从编译时间重来)
 */
uint8_t rtc_is_set(void)
{
    return (rtc_read_bkr(1) == RTC_BKP_SET_FLAG) ? 1 : 0;
}

/**
 * @brief       取两位十进制数(解析 __DATE__ / __TIME__ 用)
 */
static uint16_t rtc_two(const char *p)
{
    return (uint16_t)(((p[0] - '0') * 10) + (p[1] - '0'));
}

/**
 * @brief       取四位十进制数(解析 __DATE__ 的年份用)
 */
static uint16_t rtc_four(const char *p)
{
    return (uint16_t)(((p[0] - '0') * 1000) + ((p[1] - '0') * 100) +
                      ((p[2] - '0') * 10) + (p[3] - '0'));
}

/**
 * @brief       把英文月份缩写(如 "Sep")转成 1~12
 */
static uint8_t rtc_month_from_str(const char *s)
{
    static const char *mon[12] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    uint8_t i;

    for (i = 0; i < 12; i++)
    {
        if ((s[0] == mon[i][0]) && (s[1] == mon[i][1]) && (s[2] == mon[i][2]))
        {
            return (uint8_t)(i + 1);
        }
    }

    return 1;
}

/**
 * @brief       首次上电时用固件编译时间给 RTC 打底
 * @note        __DATE__ = "Mmm dd yyyy"(日可能用空格补位), __TIME__ = "hh:mm:ss"。
 *              没有后备电池时每次上电都会走这里, 时间从编译时刻开始往前走,
 *              至少量级是对的(比硬编码一个 2020 年有用)。
 */
static void rtc_set_build_time(void)
{
    const char *d = __DATE__;
    const char *t = __TIME__;
    uint16_t    day;

    day = (d[4] == ' ') ? (uint16_t)(d[5] - '0') : rtc_two(d + 4);

    (void)rtc_set_time(rtc_four(d + 7), rtc_month_from_str(d), (uint8_t)day,
                       (uint8_t)rtc_two(t), (uint8_t)rtc_two(t + 3), (uint8_t)rtc_two(t + 6));
}

/**
 * @brief       RTC 底层初始化(由 HAL_RTC_Init() 在首次初始化时回调)
 * @param       hrtc: RTC 句柄
 * @retval      无
 * @note        时钟源在 rtc_init() 里已经选好, 这里只保证 RTCEN 置位。
 */
void HAL_RTC_MspInit(RTC_HandleTypeDef *hrtc)
{
    (void)hrtc;

    __HAL_RCC_RTC_ENABLE();
}

/**
 * @brief       RTC 初始化
 * @param       无
 * @retval      0, 成功; 1, 初始化失败
 * @note        ★ 这里没有照抄官方在 HAL_RTC_MspInit 里用 HAL_RCC_OscConfig +
 *              HAL_RCCEx_PeriphCLKConfig 选时钟源的写法, 原因:
 *              F1 的 HAL_RCCEx_PeriphCLKConfig() 在 RTCEN 已经置位时会**整段跳过**
 *              选源动作(源码里那句 `if ((RCC->BDCR & RCC_BDCR_RTCEN) != RCC_BDCR_RTCEN)`),
 *              而官方又先调了 __HAL_RCC_RTC_ENABLE(), 于是 RTCSEL 实际没被写进去。
 *              直接写 BDCR 语义明确、不依赖 HAL 内部判断:
 *                1, 开 LSE 并等 LSERDY(最多 1s), 失败则退回内部 LSI;
 *                2, 清 RTCSEL 后再写RTCSEL(LSE=01 / LSI=10);
 *                3, 最后置 RTCEN —— 注意 RTCSEL 一旦选定就不能再改, 必须先选源再使能。
 *              只有在后备域从没被配置过(BKP0 无效)时才做这些, 之后上电不再重配, 时间不会被重置。
 */
uint8_t rtc_init(void)
{
    uint16_t bkpflag;
    uint16_t retry;

    __HAL_RCC_PWR_CLK_ENABLE();                             /* 使能电源时钟 */
    __HAL_RCC_BKP_CLK_ENABLE();                             /* 使能备份时钟 */
    HAL_PWR_EnableBkUpAccess();                             /* 取消备份区写保护 */

    g_rtc_handle.Instance = RTC;

    bkpflag = rtc_read_bkr(0);                              /* BKP0 记录上次用的是哪种时钟源 */

    if ((bkpflag != RTC_BKP_SRC_LSE) && (bkpflag != RTC_BKP_SRC_LSI))
    {
        /* ---------------- 第一次配置: 选 RTC 时钟源 ---------------- */
        retry = 200;
        RCC->BDCR |= RCC_BDCR_LSEON;                        /* 开启外部 32.768kHz 晶振 */

        while (retry && ((RCC->BDCR & RCC_BDCR_LSERDY) == 0))
        {
            retry--;
            delay_ms(5);                                    /* 最多等 1s */
        }

        if (retry == 0)                                     /* LSE 起振失败 -> 退回 LSI */
        {
            printf("RTC: LSE not ready, fallback to LSI (time will drift)\r\n");

            RCC->CSR |= RCC_CSR_LSION;
            retry = 200;

            while (retry && ((RCC->CSR & RCC_CSR_LSIRDY) == 0))
            {
                retry--;
                delay_ms(5);
            }

            RCC->BDCR &= ~RCC_BDCR_RTCSEL;
            RCC->BDCR |= RCC_BDCR_RTCSEL_1;                 /* 10 = LSI */

            rtc_write_bkr(0, RTC_BKP_SRC_LSI);
        }
        else                                                /* LSE 正常 */
        {
            RCC->BDCR &= ~RCC_BDCR_RTCSEL;
            RCC->BDCR |= RCC_BDCR_RTCSEL_0;                 /* 01 = LSE */

            rtc_write_bkr(0, RTC_BKP_SRC_LSE);
        }

        RCC->BDCR |= RCC_BDCR_RTCEN;                        /* 使能 RTC 时钟 */
    }

    /* 注意 AsynchPrediv: LSE = 32768Hz -> 32767 刚好 1Hz;
     *                     LSI 约 40000Hz -> 同样取 32767 会走快约 20%, 这里沿用官方取值。 */
    g_rtc_handle.Init.AsynchPrediv = 32767;
    g_rtc_handle.Init.OutPut       = RTC_OUTPUTSOURCE_NONE;

    if (HAL_RTC_Init(&g_rtc_handle) != HAL_OK)
    {
        return 1;
    }

    if ((bkpflag != RTC_BKP_SRC_LSE) && (bkpflag != RTC_BKP_SRC_LSI))
    {
        /* 第一次配置: 用编译时间打底。
         * 必须放在 HAL_RTC_Init() 之后 —— 它内部会等 RTC 寄存器同步(RSF),
         * 同步没完成就写 CNT 是无效的。 */
        rtc_set_build_time();
    }

    rtc_get_time();

    return 0;
}

/**
 * @brief       设置时间(年月日时分秒)
 * @param       syear: 年(1970~2105)
 * @param       smon : 月
 * @param       sday : 日
 * @param       hour : 时
 * @param       min  : 分
 * @param       sec  : 秒
 * @retval      0, 成功
 * @note        HAL 库 F1 的 RTC 时间设置函数有 BUG 一直没修, 这里按官方做法直接用寄存器写 CNT。
 */
uint8_t rtc_set_time(uint16_t syear, uint8_t smon, uint8_t sday,
                     uint8_t hour, uint8_t min, uint8_t sec)
{
    uint32_t seccount = (uint32_t)rtc_date2sec(syear, smon, sday, hour, min, sec);

    __HAL_RCC_PWR_CLK_ENABLE();                             /* 使能电源时钟 */
    __HAL_RCC_BKP_CLK_ENABLE();                             /* 使能备份域时钟 */
    HAL_PWR_EnableBkUpAccess();                             /* 取消备份域写保护 */
    /* 上面三步是必须的! */

    RTC->CRL |= 1 << 4;                                     /* 进入配置模式 */

    RTC->CNTL = seccount & 0xFFFF;
    RTC->CNTH = seccount >> 16;

    RTC->CRL &= ~(1 << 4);                                  /* 退出配置模式, 配置生效 */

    while (!__HAL_RTC_ALARM_GET_FLAG(&g_rtc_handle, RTC_FLAG_RTOFF))
    {
        /* 等待 RTC 寄存器操作完成(RTOFF == 1) */
    }

    rtc_write_bkr(1, RTC_BKP_SET_FLAG);                     /* 记下"时间被设置过" */

    return 0;
}

/**
 * @brief       得到当前时间
 * @note        结果放在全局 calendar 里, 不通过返回值传递。
 * @param       无
 * @retval      无
 */
void rtc_get_time(void)
{
    static uint16_t daycnt = 0;
    uint32_t seccount;
    uint32_t temp;
    uint16_t temp1;
    const uint8_t month_table[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

    seccount  = RTC->CNTH;
    seccount <<= 16;
    seccount += RTC->CNTL;

    temp = seccount / 86400;                                /* 总天数 */

    if (daycnt != (uint16_t)temp)                           /* 跨天了才重算年月日 */
    {
        daycnt = (uint16_t)temp;
        temp1  = 1970;

        while (temp >= 365)
        {
            if (rtc_is_leap_year(temp1))
            {
                if (temp >= 366)
                {
                    temp -= 366;
                }
                else
                {
                    break;
                }
            }
            else
            {
                temp -= 365;
            }

            temp1++;
        }

        calendar.year = temp1;
        temp1 = 0;

        while (temp >= 28)                                  /* 至少一个月 */
        {
            if (rtc_is_leap_year(calendar.year) && (temp1 == 1))
            {
                if (temp >= 29)
                {
                    temp -= 29;
                }
                else
                {
                    break;
                }
            }
            else
            {
                if (temp >= month_table[temp1])
                {
                    temp -= month_table[temp1];
                }
                else
                {
                    break;
                }
            }

            temp1++;
        }

        calendar.month = (uint8_t)(temp1 + 1);
        calendar.date  = (uint8_t)(temp + 1);
    }

    temp = seccount % 86400;
    calendar.hour = (uint8_t)(temp / 3600);
    calendar.min  = (uint8_t)((temp % 3600) / 60);
    calendar.sec  = (uint8_t)((temp % 3600) % 60);
    calendar.week = rtc_get_week(calendar.year, calendar.month, calendar.date);
}

/**
 * @brief       判断某年是不是闰年
 * @param       year: 年份
 * @retval      0, 非闰年; 1, 闰年
 */
static uint8_t rtc_is_leap_year(uint16_t year)
{
    /* 闰年规则: 四年一闰, 百年不闰, 四百年再闰 */
    if (((year % 4 == 0) && (year % 100 != 0)) || (year % 400 == 0))
    {
        return 1;
    }

    return 0;
}

/**
 * @brief       把公历日期转成星期(基姆拉尔森计算公式)
 * @param       year : 年
 * @param       month: 月
 * @param       day  : 日
 * @retval      0, 星期日; 1~6: 星期一~星期六
 */
uint8_t rtc_get_week(uint16_t year, uint8_t month, uint8_t day)
{
    if (month < 3)
    {
        month += 12;
        --year;
    }

    return (uint8_t)((day + 1 + 2 * month + 3 * (month + 1) / 5 + year +
                      (year >> 2) - year / 100 + year / 400) % 7);
}

/**
 * @brief       把年月日时分秒转成从 1970-01-01 00:00:00 起的秒数
 * @note        最大表示到 2105 年(uint32_t 只能放 136 年的秒数)。参考 Linux mktime。
 * @param       syear: 年
 * @param       smon : 月
 * @param       sday : 日
 * @param       hour : 时
 * @param       min  : 分
 * @param       sec  : 秒
 * @retval      总秒数
 */
static long rtc_date2sec(uint16_t syear, uint8_t smon, uint8_t sday,
                         uint8_t hour, uint8_t min, uint8_t sec)
{
    uint32_t Y, M, D, X, T;
    signed char monx = (signed char)smon;

    if (0 >= (monx -= 2))                   /* 1..12 -> 11,12,1..10 */
    {
        monx += 12;                         /* 把 2 月放到最后, 因为闰日在其后 */
        syear -= 1;
    }

    Y = (uint32_t)((syear - 1) * 365 + syear / 4 - syear / 100 + syear / 400);
    M = (uint32_t)(367 * monx / 12 - 30 + 59);
    D = (uint32_t)(sday - 1);
    X = Y + M + D - 719162;                 /* 减去公元元年到 1970 年的天数 */
    T = ((X * 24 + hour) * 60 + min) * 60 + sec;

    return (long)T;
}

/**
 * @brief       ★ BMS 追加(D4, 2026-09-27): 公历时间 → Unix 秒
 * @param       t : 时间点(通常传 &calendar)
 * @retval      从 1970-01-01 00:00:00 起的秒数
 * @note        只是把上面那个官方 static 函数开一个口子, ⛔ 不在应用层再写一份年月日算法 ——
 *              "两套换算"必然对不上(与 `AGENTS.md` §2.2 的"两套量纲"同类, 审查 C-22)。
 */
uint32_t rtc_time2unix(const _calendar_obj *t)
{
    return (uint32_t)rtc_date2sec(t->year, t->month, t->date, t->hour, t->min, t->sec);
}

/**
 * @brief       ★ BMS 追加(D4): Unix 秒 → 公历时间(事件日志回显用)
 * @param       ts : Unix 秒
 * @param       t  : 输出(week 不计算, 固定 0)
 * @note        纯整数逐日/逐月递减(本工程年份落在 1970~2105, 最多 136 次循环, 无浮点)。
 *              ★ 反向算法与上面的正向算法共用同一个 `rtc_is_leap_year()` ⇒ 两个方向不可能互相不认。
 */
void rtc_unix2time(uint32_t ts, _calendar_obj *t)
{
    const uint8_t mon_tab[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    uint32_t days = ts / 86400u;
    uint32_t rest = ts % 86400u;
    uint16_t year = 1970u;
    uint8_t  mon  = 0u;
    uint32_t ydays;

    while (year < 2106u)
    {
        ydays = (rtc_is_leap_year(year) != 0u) ? 366u : 365u;

        if (days < ydays)
        {
            break;
        }

        days -= ydays;
        year++;
    }

    while (mon < 12u)
    {
        ydays = ((mon == 1u) && (rtc_is_leap_year(year) != 0u)) ? 29u : mon_tab[mon];

        if (days < ydays)
        {
            break;
        }

        days -= ydays;
        mon++;
    }

    t->year  = year;
    t->month = (uint8_t)(mon + 1u);
    t->date  = (uint8_t)(days + 1u);
    t->hour  = (uint8_t)(rest / 3600u);
    t->min   = (uint8_t)((rest / 60u) % 60u);
    t->sec   = (uint8_t)(rest % 60u);
    t->week  = 0u;                          /* 界面不显示星期; 需要时用 rtc_get_week() 现算 */
}
