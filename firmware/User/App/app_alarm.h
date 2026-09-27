/**
 ****************************************************************************************************
 * @file        app_alarm.h
 * @brief       两级超限判断: 危险区(蜂鸣器 + 整屏闪) / 舒适区(只黄字提示)
 ****************************************************************************************************
 */

#ifndef __APP_ALARM_H
#define __APP_ALARM_H

#include "./SYSTEM/sys/sys.h"

/* 通道标志位, 危险区与舒适区共用同一组位, 供界面给数值行上色 */
#define ALARM_FAULT_TEMP    0x01
#define ALARM_FAULT_HUMI    0x02
#define ALARM_FAULT_LUX     0x04

void        alarm_init(void);                                   /* 初始化蜂鸣器与 KEY0 */
void        alarm_feed(int16_t temp_x10, uint16_t humi_x10,
                       uint32_t lux_cdm2, uint8_t th_valid);    /* 每采样周期喂一次数据;
                                                                 * th_valid=0 表示温湿度读数无效,
                                                                 * 此时不据此判断(保持原行为) */
void        alarm_task(void);                                   /* 每 20ms 调一次: 计时停响 + 驱动蜂鸣器 */
uint8_t     alarm_key_mute(void);                               /* KEY0 消音; 1 = 真的消音了 */
uint8_t     alarm_get_faults(void);                             /* 危险区标志(会响) */
uint8_t     alarm_get_warns(void);                              /* 舒适区标志(只黄字, 不响) */
uint8_t     alarm_is_beeping(void);                             /* 蜂鸣器是否正在响 */
const char *alarm_reason(void);                                 /* 显示用字符串, 例如 "TEMP+warnH" */

/* 报警开始时的回调(交给 app_log 存历史)。app_alarm 不知道历史怎么存, 保持解耦;
 * 参数: 危险区标志(ALARM_FAULT_xxx) + 当时的温度/湿度/光照。传 NULL = 不记录。 */
void        alarm_set_log_hook(void (*hook)(uint8_t faults, int16_t temp_x10,
                                           uint16_t humi_x10, uint32_t lux_cdm2));

#endif
