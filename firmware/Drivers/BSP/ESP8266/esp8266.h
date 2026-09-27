/**
 ****************************************************************************************************
 * @file        esp8266.h
 * @brief       ESP8266 (AT 固件) WiFi 模块驱动 —— USART3 (PB10=TX, PB11=RX)
 * @note        模块按"透传模式"使用: 建好 TCP 连接后 STM32 直接写原始字节,
 *              因此对 ESP8266 的 AT 固件版本没有要求(不依赖 AT+MQTTxxx 指令)。
 ****************************************************************************************************
 */

#ifndef __ESP8266_H
#define __ESP8266_H

#include "./SYSTEM/sys/sys.h"

#define ESP8266_RX_BUF_SIZE     768         /* 接收环形缓冲大小 */
#define ESP8266_RESP_BUF_SIZE   384         /* AT 应答快照大小 */
#define ESP8266_POLL_PERIOD_MS  20          /* 等待应答期间回调上层的间隔(ms) */
#define ESP8266_RESET_SETTLE_MS 2000        /* AT+RST 后等模块自行重启的时间(ms, D5 起带泵) */

#define ESP8266_STAT_OK         0           /* 成功 */
#define ESP8266_STAT_ERROR      1           /* 模块返回 ERROR / FAIL */
#define ESP8266_STAT_TIMEOUT    2           /* 超时 */

void        esp8266_init(void);                                     /* 启动 USART3 接收中断 */
void        esp8266_clear(void);                                    /* 清空接收缓冲 */
int         esp8266_getchar(void);                                  /* 取一字节, 无数据返回 -1 */
uint16_t    esp8266_available(void);                                /* 缓冲内可读字节数 */

uint8_t     esp8266_at(void);                                       /* 发送 AT, 测试通信 */
uint8_t     esp8266_echo_off(void);                                 /* ATE0 关回显 */
uint8_t     esp8266_reset(void);                                    /* AT+RST 软复位 */
uint8_t     esp8266_set_mode(uint8_t mode);                         /* AT+CWMODE=<mode> */
uint8_t     esp8266_join_ap(const char *ssid, const char *pwd);     /* AT+CWJAP 连接热点 */
uint8_t     esp8266_open_tcp(const char *host, uint16_t port);      /* AT+CIPSTART 建立 TCP */
uint8_t     esp8266_enter_transparent(void);                        /* AT+CIPMODE=1 + AT+CIPSEND */
uint8_t     esp8266_exit_transparent(void);                         /* +++ 退出透传 */
uint8_t     esp8266_send(const uint8_t *buf, uint16_t len);         /* 透传发送原始数据 */
uint8_t     esp8266_cmd(const char *cmd, const char *ack, uint32_t timeout_ms);  /* 发命令等应答 */
uint8_t     esp8266_wait_ack(const char *ack, uint32_t timeout_ms); /* 只等待应答 */
const char *esp8266_get_resp(void);                                 /* 最近一次应答快照 */

/* --- 等待应答期间的"服务回调": 让报警/消音/显示不被网络阻塞 ---
 * AT+CWJAP 最长等 20s、AT+CIPSTART 最长等 15s, 这段时间主循环是停住的。
 * 应用层把"报警任务 + 报警行刷新 + 喂狗"注册进来, 驱动在等待循环里每 20ms
 * 回调一次, 保证蜂鸣器节拍、KEY0 消音、超时停响在网络忙时依然正常。
 * 回调运行在主循环上下文(不是中断), 不会与刷屏产生重入。 */
void        esp8266_set_poll_hook(void (*hook)(void));              /* 应用层注册回调(传 NULL 取消) */
void        esp8266_service(void);                                  /* 驱动内部等待循环调用 */

/* --- 单总线(DHT11/DS18B20)期间的接收暂停 ---
 * DHT11 判"0/1"靠 delay_us(40) 之后读引脚电平, 而 "0" 的高电平只有 26~28us;
 * 115200 下 USART3 每约 87us 进一次中断, 会把采样点推后 -> 偶发 CRC 校验失败。
 * 读单总线器件前暂停 USART3 中断(只关 USART3, 不动 SysTick, 时间基准不受影响),
 * 读完恢复; 这段时间模块送来的字节会因溢出丢失, 但都是 MQTT QoS0 回执, 可丢。 */
void        esp8266_rx_pause(void);                                 /* 暂停 USART3 接收中断 */
void        esp8266_rx_resume(void);                                /* 恢复 USART3 接收中断 */

/* --- 排错支持: 波特率自适应 + 接收字节取证 --- */
uint8_t     esp8266_set_baud(uint32_t baud);                        /* 改 USART3 波特率(不动接收中断) */
uint32_t    esp8266_get_baud(void);                                 /* 当前波特率 */
uint16_t    esp8266_rx_total(void);                                 /* 上次 clear 后收到的字节数 */
const uint8_t *esp8266_rx_raw(void);                                /* 收到的前 8 个原始字节 */
uint32_t    esp8266_scan_baud(uint16_t *p_rx_bytes);                /* 逐个波特率试探 AT; 返回可用值, 0=全失败 */

#endif
