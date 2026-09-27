/**
 ****************************************************************************************************
 * @file        rtc.h
 * @brief       RTC 实时时钟驱动(带后备电池时掉电继续走)
 * @note        移植自正点原子官方 HAL 例程「实验15 RTC实验」, 时钟换算/年月日算法与官方一致,
 *              做了四处适配:
 *                1, 去掉公司信息页脚、闹钟设置(rtc_set_alarm)与仅用于演示的秒中断
 *                   —— 本工程只需要"读时间给报警记录打时间戳", 不需要秒中断和闹钟;
 *                2, 首次上电(后备域从没被配置过)不再硬编码 2020-04-25, 而是取
 *                   **固件编译时间**(__DATE__/__TIME__)作为起点, 这样日志时间戳至少有参考价值;
 *                3, 新增 rtc_is_set(): 区分"时间被真正设置过(有电池)"和"每次上电从编译时间重来";
 *                4, 官方头文件里声明为 static 的 rtc_is_leap_year/rtc_date2sec 改为 .c 内部 static。
 *
 * @note        硬件: PC14/PC15 上的 32.768kHz 晶振 + VBAT 纽扣电池座(CR1220)。
 *              不装电池也能跑, 只是每次掉电后时间从编译时间重新开始。
 *              晶振起振失败时官方做法是自动切到内部 LSI(约 40kHz, 精度差但能走)。
 *
 * @note        ★ BMS 追加(D4, 2026-09-27, AGENTS §3 G5 要求写清改动点): 新增 `rtc_time2unix()`
 *              与 `rtc_unix2time()` —— 事件日志的 Unix 秒时间戳与回显要用, 而换算算法**只允许**
 *              有这一份(正向复用官方 static 的 `rtc_date2sec`, 反向与它共用同一个闰年判据)。
 *              验证方式: D4 上机 —— 页 3 的时间列与串口 dump 必须与 RTC 当前时间一致;
 *              无 VBAT 电池时两者都应从固件编译时刻起算(`rtc_is_set()` == 0)。
 ****************************************************************************************************
 */

#ifndef __RTC_H
#define __RTC_H

#include "./SYSTEM/sys/sys.h"

/* 时间结构体, 用来保存年月日时分秒等信息 */
typedef struct
{
    uint8_t  hour;      /* 时 */
    uint8_t  min;       /* 分 */
    uint8_t  sec;       /* 秒 */
    uint16_t year;      /* 年 */
    uint8_t  month;     /* 月 */
    uint8_t  date;      /* 日 */
    uint8_t  week;      /* 星期: 0=星期日, 1~6=星期一~六 */
} _calendar_obj;

extern _calendar_obj calendar;                      /* 最近一次读取到的时间 */

uint8_t  rtc_init(void);                            /* 初始化 RTC; 0=成功, 1=失败 */
void     rtc_get_time(void);                        /* 刷新 calendar */
uint8_t  rtc_set_time(uint16_t syear, uint8_t smon, uint8_t sday,
                      uint8_t hour, uint8_t min, uint8_t sec);  /* 设置时间; 0=成功 */
uint16_t rtc_read_bkr(uint32_t bkrx);               /* 读后备寄存器(bkrx: 0~41) */
void     rtc_write_bkr(uint32_t bkrx, uint16_t data);  /* 写后备寄存器(bkrx: 0~41) */
uint8_t  rtc_get_week(uint16_t year, uint8_t month, uint8_t day);  /* 0=星期日 */
uint8_t  rtc_is_set(void);                          /* 1 = 时间被真正设置过(有后备电池) */
uint32_t rtc_time2unix(const _calendar_obj *t);     /* ★ BMS: 公历 → Unix 秒(1970 起) */
void     rtc_unix2time(uint32_t ts, _calendar_obj *t); /* ★ BMS: Unix 秒 → 公历(week 恒 0) */

#endif
