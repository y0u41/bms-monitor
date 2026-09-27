/**
 ****************************************************************************************************
 * @file        app_mqtt.c
 * @brief       极简 MQTT 3.1.1 客户端
 * @note        1, 只实现 OneNET 数据上报所需的 4 种报文, 代码量小、易读;
 *              2, 报文写入静态缓冲后一次性通过 ESP8266 透传发出;
 *              3, QoS0 发布, 不做重传; 连接成功以 CONNACK(20 02 00 00) 为准。
 ****************************************************************************************************
 */

#include "./App/app_mqtt.h"
#include "./App/app_config.h"
#include "./BSP/ESP8266/esp8266.h"
#include "./SYSTEM/delay/delay.h"
#include <stdio.h>
#include <string.h>

#define MQTT_BUF_SIZE       512
#define MQTT_HDR_RESERVE    5           /* 固定头最大长度(1 + 4 字节变长) */

static uint8_t g_mqtt_buf[MQTT_BUF_SIZE];

/* ============================================================================
 *  下行监视(P0): 上线以后必须持续排空接收缓冲
 * ----------------------------------------------------------------------------
 * 这块是整个联网链路唯一能发现"断线"的地方, 原因见 app_mqtt.h 的说明。
 * 透传模式下 TCP 收到的字节会原样出现在串口上, 里面混着两类东西:
 *   1, MQTT 协议字节  -> 只关心 PINGRESP(0xD0 0x00);
 *   2, 模块自己的 URC  -> CLOSED / WIFI DISCONNECT(纯 ASCII, 用滑动文本窗匹配)。
 * 检测到 CLOSED 后由应用层调用 net_fail() 走完整重连流程。
 * ==========================================================================*/
#define MQTT_URC_WIN        24          /* URC 文本滑动窗长度 */
#define MQTT_DRAIN_MAX      1024        /* 单次最多排空多少字节(防死循环) */

static uint32_t g_rx_bytes    = 0;      /* 累计收到字节数 */
static uint32_t g_rx_last     = 0;      /* 最近一次收到字节的时刻 */
static uint32_t g_rx_pingresp = 0;      /* 最近一次收到 PINGRESP 的时刻 */
static uint8_t  g_rx_closed   = 0;      /* 1 = 收到 CLOSED / WIFI DISCONNECT */
static uint8_t  g_rx_prev     = 0;      /* 上一字节, 用于 PINGRESP 两字节匹配 */
static char     g_urc[MQTT_URC_WIN];    /* URC 文本滑动窗(只收可打印字符) */
static uint8_t  g_urc_len     = 0;

/**
 * @brief       复位下行监视状态(重新建链时调用)
 */
static void mqtt_rx_reset(void)
{
    g_rx_prev   = 0;
    g_rx_closed = 0;
    g_urc_len   = 0;
    g_urc[0]    = '\0';
}

/**
 * @brief       排空 USART3 接收缓冲, 顺便识别 PINGRESP 与 CLOSED
 * @param       无
 * @retval      无
 * @note        只在透传(已上线)状态下调用。AT 命令期间收数由 esp8266_wait_ack() 负责,
 *              两边同时读会互相偷字节。
 */
void mqtt_poll_rx(void)
{
    uint16_t guard = 0;
    int ch;

    while ((guard++ < MQTT_DRAIN_MAX) && ((ch = esp8266_getchar()) >= 0))
    {
        uint8_t b = (uint8_t)ch;

        g_rx_bytes++;
        g_rx_last = HAL_GetTick();

        if ((g_rx_prev == 0xD0) && (b == 0x00))     /* PINGRESP: 0xD0 0x00 */
        {
            g_rx_pingresp = g_rx_last;
        }

        g_rx_prev = b;

        if ((b >= 0x20) && (b < 0x7F))              /* 只让可打印字符进 URC 窗 */
        {
            if (g_urc_len < (MQTT_URC_WIN - 1))
            {
                g_urc[g_urc_len++] = (char)b;
            }
            else
            {
                memmove(g_urc, g_urc + 1, MQTT_URC_WIN - 2);
                g_urc[MQTT_URC_WIN - 2] = (char)b;
            }

            g_urc[MQTT_URC_WIN - 1] = '\0';

            if ((strstr(g_urc, "CLOSED") != NULL) ||
                (strstr(g_urc, "WIFI DISCONNECT") != NULL))
            {
                g_rx_closed = 1;
                break;
            }
        }
        else                                        /* 非可打印: URC 窗断开 */
        {
            g_urc_len = 0;
            g_urc[0]  = '\0';
        }
    }
}

/**
 * @brief       是否收到过 CLOSED / WIFI DISCONNECT
 */
uint8_t mqtt_link_closed(void)
{
    return g_rx_closed;
}

/**
 * @brief       清除 CLOSED 标志
 */
void mqtt_link_clear_closed(void)
{
    g_rx_closed = 0;
}

/**
 * @brief       累计收到的字节数(排错用)
 */
uint32_t mqtt_rx_bytes(void)
{
    return g_rx_bytes;
}

/**
 * @brief       最近一次收到字节的时刻(排错用)
 */
uint32_t mqtt_rx_last_tick(void)
{
    return g_rx_last;
}

/**
 * @brief       把 16 位整数按大端写入
 */
static uint16_t mqtt_put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)(v & 0xFF);
    return 2;
}

/**
 * @brief       写入 MQTT UTF-8 字符串(2 字节长度 + 内容)
 */
static uint16_t mqtt_put_str(uint8_t *p, const char *s)
{
    uint16_t l = (uint16_t)strlen(s);

    mqtt_put_u16(p, l);
    memcpy(p + 2, s, l);

    return (uint16_t)(2 + l);
}

/**
 * @brief       组装完整报文: 计算变长长度并前移数据
 * @param       type: 报文类型字节, 例如 0x10(CONNECT)
 * @param       body_len: 可变头 + 载荷长度
 * @retval      整包长度(字节)
 */
static uint16_t mqtt_assemble(uint8_t type, uint16_t body_len)
{
    uint8_t  rl[4];
    uint8_t  rln = 0;
    uint32_t len = body_len;
    uint16_t i;

    do
    {
        uint8_t b = (uint8_t)(len % 128);
        len /= 128;

        if (len > 0)
        {
            b |= 0x80;
        }

        rl[rln++] = b;
    } while ((len > 0) && (rln < 4));

    /* 把可变头+载荷整体前移, 给固定头腾位置 */
    for (i = 0; i < body_len; i++)
    {
        g_mqtt_buf[1 + rln + i] = g_mqtt_buf[MQTT_HDR_RESERVE + i];
    }

    g_mqtt_buf[0] = type;

    for (i = 0; i < rln; i++)
    {
        g_mqtt_buf[1 + i] = rl[i];
    }

    return (uint16_t)(1 + rln + body_len);
}

/**
 * @brief       等待 CONNACK
 * @param       timeout_ms: 超时(ms)
 * @retval      MQTT_STAT_OK / _ERROR / _TIMEOUT
 */
static uint8_t mqtt_wait_connack(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    uint8_t  state = 0;

    while ((HAL_GetTick() - start) < timeout_ms)
    {
        int ch;

        esp8266_service();                      /* 等 CONNACK 的 5s 里也让报警/消音继续工作 */

        ch = esp8266_getchar();

        if (ch < 0)
        {
            continue;
        }

        switch (state)
        {
            case 0:
                state = (ch == 0x20) ? 1 : 0;
                break;

            case 1:
                state = (ch == 0x02) ? 2 : 0;
                break;

            case 2:
                state = (ch == 0x00) ? 3 : 0;
                break;

            default:
                return (ch == 0x00) ? MQTT_STAT_OK : MQTT_STAT_ERROR;
        }
    }

    return MQTT_STAT_TIMEOUT;
}

/**
 * @brief       MQTT CONNECT
 * @param       clientid : 设备名称
 * @param       username : 产品 ID
 * @param       password : token
 * @param       keepalive: 保活时间(秒)
 * @retval      MQTT_STAT_OK / _ERROR / _TIMEOUT
 */
uint8_t mqtt_connect(const char *clientid, const char *username, const char *password, uint16_t keepalive)
{
    uint8_t *q = g_mqtt_buf + MQTT_HDR_RESERVE;
    uint16_t n = 0;
    uint16_t total;

    n += mqtt_put_str(q + n, "MQTT");                   /* 协议名 */
    q[n++] = 0x04;                                      /* 协议级别 3.1.1 */
    q[n++] = 0x02 | 0x80 | 0x40;                        /* CleanSession | UserName | Password */
    n += mqtt_put_u16(q + n, keepalive);                /* Keep Alive */
    n += mqtt_put_str(q + n, clientid);                 /* Payload: ClientId */
    n += mqtt_put_str(q + n, username);                 /* Payload: UserName */
    n += mqtt_put_str(q + n, password);                 /* Payload: Password */

    total = mqtt_assemble(0x10, n);

    printf("MQTT >> CONNECT (%u bytes, client=%s)\r\n", (unsigned)total, clientid);

    mqtt_rx_reset();                            /* 新的一次连接: 清掉上一轮的链路状态 */
    esp8266_clear();
    g_rx_last = HAL_GetTick();

    if (esp8266_send(g_mqtt_buf, total) != ESP8266_STAT_OK)
    {
        return MQTT_STAT_ERROR;
    }

    return mqtt_wait_connack(5000);
}

/**
 * @brief       MQTT PUBLISH (QoS0)
 * @param       topic  : 主题
 * @param       payload: 载荷
 * @param       len    : 载荷长度
 * @retval      MQTT_STAT_OK / _ERROR
 */
uint8_t mqtt_publish(const char *topic, const uint8_t *payload, uint16_t len)
{
    uint8_t *q = g_mqtt_buf + MQTT_HDR_RESERVE;
    uint16_t n = 0;
    uint16_t total;

    n += mqtt_put_str(q + n, topic);
    memcpy(q + n, payload, len);
    n = (uint16_t)(n + len);

    total = mqtt_assemble(0x30, n);                     /* 0x30: PUBLISH, QoS0 */

    printf("MQTT >> PUBLISH (%u bytes)\r\n", (unsigned)total);

    return esp8266_send(g_mqtt_buf, total);
}

/**
 * @brief       MQTT PINGREQ(保活) —— 发出去并**等 PINGRESP**
 * @param       无
 * @retval      MQTT_STAT_OK / _ERROR / _TIMEOUT
 * @note        ★ 这是整个工程唯一能发现"平台/网络已经断了"的地方。
 *              旧版只判断"串口写成功了没有"(那几乎永远成功), 等于没有保活检测:
 *              云端断了以后本地一路显示 Cloud : online、Sent 还在涨, 实际早就没数据了。
 *              现在发完 PINGREQ 等 PINGRESP(0xD0 0x00), 等不到就当链路故障。
 *              等待期间照常喂狗 + 跑报警(esp8266_service), 不会卡住蜂鸣器和 KEY0。
 */
uint8_t mqtt_ping(void)
{
    uint32_t start;
    uint32_t resp0;

    mqtt_poll_rx();                             /* 先排空, 避免把上一轮的残留当成应答 */
    resp0 = g_rx_pingresp;

    g_mqtt_buf[0] = 0xC0;
    g_mqtt_buf[1] = 0x00;

    printf("MQTT >> PINGREQ\r\n");

    if (esp8266_send(g_mqtt_buf, 2) != ESP8266_STAT_OK)
    {
        return MQTT_STAT_ERROR;
    }

    start = HAL_GetTick();

    while ((HAL_GetTick() - start) < MQTT_PING_TIMEOUT_MS)
    {
        esp8266_service();                      /* 报警 / 消音 / 喂狗 照常 */
        mqtt_poll_rx();

        if (g_rx_pingresp != resp0)
        {
            return MQTT_STAT_OK;                /* 收到新的 PINGRESP: 链路活着 */
        }

        if (g_rx_closed)
        {
            return MQTT_STAT_ERROR;             /* 同时收到 CLOSED: 直接判故障 */
        }

        delay_ms(5);
    }

    printf("MQTT: no PINGRESP in %u ms\r\n", (unsigned)MQTT_PING_TIMEOUT_MS);

    return MQTT_STAT_TIMEOUT;
}

/**
 * @brief       MQTT DISCONNECT
 * @param       无
 * @retval      MQTT_STAT_OK / _ERROR
 */
uint8_t mqtt_disconnect(void)
{
    g_mqtt_buf[0] = 0xE0;
    g_mqtt_buf[1] = 0x00;

    return esp8266_send(g_mqtt_buf, 2);
}
