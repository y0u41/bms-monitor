/**
 ****************************************************************************************************
 * @file        app_onenet.c
 * @brief       OneNET 数据上报
 * @note        接入点      : mqtts.heclouds.com:1883 (非加密)
 *              clientId    : 设备名称
 *              username    : 产品 ID
 *              password    : token(由设备密钥计算, 真值在 app_config_local.h)
 *              上报主题/载荷 : 由 APP_ONENET_MODE 决定, 见 app_config.h
 *                              mode 0 = $sys/{pid}/{dev}/dp/post/json      数据流-数据点
 *                              mode 1 = $sys/{pid}/{dev}/thing/property/post 物模型属性
 *              载荷不带时间戳 -> 平台按到达时间入库, 因此不需要 RTC/SNTP。
 ****************************************************************************************************
 */

#include "./App/app_onenet.h"
#include "./App/app_config.h"
#include "./App/app_mqtt.h"
#include <string.h>
#include <stdio.h>

static char g_topic[96];
static char g_payload[224];

/**
 * @brief       建立 MQTT 连接
 * @param       无
 * @retval      MQTT_STAT_OK / _ERROR / _TIMEOUT
 */
uint8_t onenet_connect(void)
{
    return mqtt_connect(ONENET_DEVICE, ONENET_PID, ONENET_TOKEN, MQTT_KEEPALIVE_S);
}

/**
 * @brief       上报温度/湿度/光照
 * @param       msg_id: 消息 ID(递增, 用于平台回执匹配)
 * @param       temp  : 温度 ℃
 * @param       humi  : 湿度 %RH
 * @param       lux   : 光照 0~100
 * @retval      MQTT_STAT_OK / _ERROR
 */
uint8_t onenet_publish(uint16_t msg_id, int16_t temp_x10, uint16_t humi_x10, uint8_t lux)
{
    char v_t[12];
    char v_h[12];

    /* 把 0.1 单位的值写成 JSON 数字, 例如 26.5 / -12.3 */
    if (temp_x10 < 0)
    {
        snprintf(v_t, sizeof(v_t), "-%u.%u", (unsigned)((-temp_x10) / 10), (unsigned)((-temp_x10) % 10));
    }
    else
    {
        snprintf(v_t, sizeof(v_t), "%u.%u", (unsigned)(temp_x10 / 10), (unsigned)(temp_x10 % 10));
    }

    snprintf(v_h, sizeof(v_h), "%u.%u", (unsigned)(humi_x10 / 10), (unsigned)(humi_x10 % 10));

#if (APP_ONENET_MODE == 1)
    /* --- 物模型属性上报(OneNET Studio): 属性标识符必须与平台物模型一致 --- */
    snprintf(g_topic, sizeof(g_topic), "$sys/%s/%s/thing/property/post",
             ONENET_PID, ONENET_DEVICE);

    snprintf(g_payload, sizeof(g_payload),
             "{\"id\":\"%u\",\"version\":\"1.0\",\"params\":{"
             "\"%s\":{\"value\":%s},\"%s\":{\"value\":%s},\"%s\":{\"value\":%u}}}",
             (unsigned)msg_id,
             ONENET_DP_TEMP, v_t,
             ONENET_DP_HUMI, v_h,
             ONENET_DP_LUX,  (unsigned)lux);
#else
    /* --- 数据流-数据点上报(MQTT物联网套件): 不需要预先定义物模型 --- */
    snprintf(g_topic, sizeof(g_topic), "$sys/%s/%s/dp/post/json",
             ONENET_PID, ONENET_DEVICE);

    snprintf(g_payload, sizeof(g_payload),
             "{\"id\":%u,\"dp\":{\"%s\":[{\"v\":%s}],\"%s\":[{\"v\":%s}],\"%s\":[{\"v\":%u}]}}",
             (unsigned)msg_id,
             ONENET_DP_TEMP, v_t,
             ONENET_DP_HUMI, v_h,
             ONENET_DP_LUX,  (unsigned)lux);
#endif

    printf("ONENET >> %s\r\n", g_topic);
    printf("ONENET >> %s\r\n", g_payload);

    return mqtt_publish(g_topic, (const uint8_t *)g_payload, (uint16_t)strlen(g_payload));
}

/**
 * @brief       取上报主题
 */
const char *onenet_get_topic(void)
{
    return g_topic;
}

/**
 * @brief       取最近一次上报载荷
 */
const char *onenet_get_payload(void)
{
    return g_payload;
}
