/**
 ****************************************************************************************************
 * @file        app_onenet.h
 * @brief       OneNET「MQTT 物联网套件」数据点上报封装
 ****************************************************************************************************
 */

#ifndef __APP_ONENET_H
#define __APP_ONENET_H

#include "./SYSTEM/sys/sys.h"

uint8_t onenet_connect(void);                                               /* 建立 MQTT 连接 */
/* 上报 3 个数据流; 温度/湿度以 0.1 为单位传入(例: 265 表示 26.5) */
uint8_t onenet_publish(uint16_t msg_id, int16_t temp_x10, uint16_t humi_x10, uint8_t lux);
const char *onenet_get_topic(void);                                         /* 取上报主题 */
const char *onenet_get_payload(void);                                       /* 取最近一次上报载荷 */

#endif
