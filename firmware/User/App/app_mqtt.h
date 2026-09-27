/**
 ****************************************************************************************************
 * @file        app_mqtt.h
 * @brief       极简 MQTT 3.1.1 客户端(仅实现 CONNECT / PUBLISH QoS0 / PINGREQ / DISCONNECT)
 * @note        报文通过 ESP8266 透传通道直接发出, 二进制安全(含 0x00)。
 ****************************************************************************************************
 */

#ifndef __APP_MQTT_H
#define __APP_MQTT_H

#include "./SYSTEM/sys/sys.h"

#define MQTT_STAT_OK        0
#define MQTT_STAT_ERROR     1
#define MQTT_STAT_TIMEOUT   2

uint8_t mqtt_connect(const char *clientid, const char *username, const char *password, uint16_t keepalive);
uint8_t mqtt_publish(const char *topic, const uint8_t *payload, uint16_t len);
uint8_t mqtt_ping(void);                    /* 发 PINGREQ 并等 PINGRESP, 是唯一的链路活性检测 */
uint8_t mqtt_disconnect(void);

/* --- 下行监视: 上线(进透传)以后必须持续排空接收缓冲 ---
 * 不排空的后果: 768 字节环形缓冲只涨不落, 满了以后模块发来的 CLOSED / PINGRESP
 * 会被静默丢弃 -> 云端断了、模块掉线了, 本地却一直显示 "Cloud : online"。
 * 这类"静默失效"对监护类设备最危险, 所以主循环每轮都要调一次 mqtt_poll_rx()。
 * 注意: 只在透传(已上线)状态下调用 —— AT 命令期间由 esp8266_wait_ack() 自己收数。 */
void     mqtt_poll_rx(void);                /* 排空 USART3 缓冲, 更新链路状态 */

uint8_t  mqtt_link_closed(void);            /* 1 = 收到 CLOSED / WIFI DISCONNECT */
void     mqtt_link_clear_closed(void);      /* 清除上述标志(处理完后调用) */
uint32_t mqtt_rx_bytes(void);               /* 累计收到的字节数(排错用) */
uint32_t mqtt_rx_last_tick(void);           /* 最近一次收到字节的时刻 */

#endif
