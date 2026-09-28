/**
 ****************************************************************************************************
 * @file        rs485.h
 * @brief       板载 RS485 物理层（USART2 + PD7 方向脚）：IDLE 中断 + DMA 收帧 / DMA 发送 + TC 中断释放 DE
 * @note        1, ★ 引脚口径唯一来源 = `Tech_Design.md` §2：**PA2 = USART2_TX / PA3 = USART2_RX /
 *                  PD7 = DE·RE**，⚠️ **P5 跳线必须插上**（§2 跳线表："用板载 RS485 收发器必须插上"）。
 *                  本模块是 `Tech_Design.md` §4.3 时间预算注② 的代码落点：
 *                  "**USART IDLE 中断 + DMA 收帧 / DMA 发送**（`T3.5` 判帧用定时器，
 *                    DE/RE 用 **TC 中断**释放，**不得用固定延时** —— 早释放会截断最后一字节）"。
 *              2, ★ **本模块自己持有 `huart2`**，⛔ 不写进 CubeMX 生成的 `User/usart.c`
 *                  （那份文件只声明 USART1/USART3；`HAL_UART_MspInit()` 对 USART2 什么都不做，
 *                    所以时钟/GPIO/DMA 全部在本文件里配，见 `rs485_init()`）。
 *                  ⛔ 不要"顺手"往 `HAL_UART_MspInit()` 里加 USART2 分支 —— `app_nvic.c` 头注
 *                  已经定过这条口径：CubeMX 生成物只当**初值**，本工程的外设初始化归自己的模块。
 *              3, ★★ **DMA 通道号是一个"错了不报错、只是永远收不到"的数**，本仓库**没有 RM0008**，
 *                  所以它有**两个来源**（`AGENTS.md` §3.1 ①/②）：
 *                  a) 手册 Table 78「Summary of DMA1 requests for each channel」：
 *                     **USART2_RX = DMA1_Channel6 / USART2_TX = DMA1_Channel7**
 *                     （同一张表里 USART1_TX = Channel4 已由一份第三方实现独立印证；
 *                       ⚠️ 本轮**没有取到 RM0008 本地原文** ⇒ 另一条下注的通道号都只是"未见反例"）；
 *                  b) ★ **真机判据 `rs485_selftest()`**：挂错通道 = USART 的 DMA 请求永远不触发
 *                     = `HAL_UART_Transmit_DMA()` 永不完成 ⇒ **自检当场报超时**。
 *                     ⇒ 这条判据不需要任何外部设备（自检把 DE 留在**接收**态，收发器驱动关闭，
 *                        ⛔ 不会在 A/B 总线上产生任何波形），是本模块唯一能自己证明自己的那一格。
 *              4, ⛔ **中断里不调任何 FreeRTOS API**（`FromISR` 一族一次都不出现）——
 *                 回调只写 `volatile` 标志与寄存器。这条口径同时消掉了"中断优先级必须 ≥5"
 *                 那一整套约束（`app_nvic.c` 的注释里那句"D5 一旦加队列发送就会踩坑"因此不适用）。
 *              5, ⛔ 本模块不做协议：CRC16、功能码、寄存器映射一律在 `User/App/app_modbus.c`。
 *                  这里只回答"收到一帧字节"与"把一帧字节发出去"。
 *              6, ⚠️ ★ **本模块故意不定义 `HAL_UART_ErrorCallback`** —— 那个符号已经被
 *                  `Drivers/BSP/ESP8266/esp8266.c:164` 定义了（两个定义 = 直接链接冲突），
 *                  而它那一份是**按实例守卫**的（只处理 USART3）。
 *                  ⇒ USART2 的错误恢复由本模块在**任务上下文**里做（`rs485.c` 的 `rs485_recover()`，
 *                    每拍随 `rs485_rx_take()` / `rs485_tx_busy()` 跑一次），它同时管两件事：
 *                    **a)** 发送卡死（TC 丢一次 ⇒ DE 永久停在发送态 ⇒ "从机再也不应答"）；
 *                    **b)** 接收掉挂（HAL 在错误路径里把 `RxState` 置回 READY 却不会自动重挂
 *                            ⇒ "一次噪声 = 永久收不到"）。
 *                  ★ 这条安排本身是一个口径：**错误恢复要读的标志位与状态机，在 ISR 里反而看不清**，
 *                    所以驱动只负责"记下发生了"，恢复留给唯一能看清全局的那一层。
 * @note        移植来源：**本项目自写**（`firmware/Drivers/BSP/` 与旧工程 `atk_f103/` 均无 RS485 驱动；
 *                  官方例程的 RS485 是纯寄存器版且无 DMA）。改动点：不适用（新建）。
 *              验证方式（**本轮只有编译级 + 门禁级 + 一条自检通路，实物台架未做**）：
 *                  ① `rs485_selftest()` 上电真机跑 —— 见上面第 3 条 b)；
 *                  ② D9 台架：USB-RS485 转换器 + 第三方 Modbus 主机（Modbus Poll / QModMaster）
 *                     读 0x0000~0x0032 全部 51 个寄存器，响应 ≤100ms（§4.3 注①）；
 *                  ③ ⚠️ **背靠背连续帧**必须单独测：IDLE 的静默门限（1 字符 ≈521µs）**小于**
 *                     Modbus 的 T3.5（1823µs）⇒ 帧中若有 >521µs 的间隙会被切成两帧（`app_config.h` §12.13.1）。
 ****************************************************************************************************
 */

#ifndef __RS485_H
#define __RS485_H

#include "./SYSTEM/sys/sys.h"

/* ===================== 物理层常量（★ 出处见文件头；⛔ 不要在这里改数值，改 `app_config.h` §12.13） ===================== */
#define RS485_UART_INSTANCE     USART2
#define RS485_TX_GPIO_PORT      GPIOA
#define RS485_TX_PIN            GPIO_PIN_2
#define RS485_RX_GPIO_PORT      GPIOA
#define RS485_RX_PIN            GPIO_PIN_3
#define RS485_DE_GPIO_PORT      GPIOD
#define RS485_DE_PIN            GPIO_PIN_7

/* DMA 通道（★ 出处 + 真机判据见文件头第 3 条） */
#define RS485_DMA_RX_CHANNEL    DMA1_Channel6
#define RS485_DMA_TX_CHANNEL    DMA1_Channel7
#define RS485_DMA_RX_IRQn       DMA1_Channel6_IRQn
#define RS485_DMA_TX_IRQn       DMA1_Channel7_IRQn

/* RX DMA 缓冲（**必须 ≥ `APP_MB_ADU_MAX`**：装不下时 IDLE 事件给出的长度会超过协议上限）。
 * 取 2 倍 ADU 是刻意的：`HAL_UARTEx_ReceiveToIdle_DMA()` 在**正常模式**下，缓冲被写满
 * （Transfer Complete）也会触发一次 RxEvent —— 那种事件的 `RxEventType` 是 TC 而不是 IDLE，
 * 本模块会把它当作"过长的帧"计数并重新武装，而不会把它当成一帧合法的请求。
 * ⚠️ 它**不是**"支持 512 字节的帧"：协议上限仍是 256（`app_config.h` §12.13）。 */
#define RS485_RX_DMA_SIZE       (2u * 256u)

/* ===================== 返回码 ===================== */
#define RS485_OK                0
#define RS485_ERR_PARAM         1   /* 入参非法（空指针 / 长度 0 / 超过 ADU 上限） */
#define RS485_ERR_BUSY          2   /* 上一次发送还没结束（TC 未到）—— ⛔ 调用方必须等，不许重入 */
#define RS485_ERR_HAL           3   /* `HAL_UART_Transmit_DMA()` 本身拒绝（状态机不在 READY） */

/**
 * @brief       初始化：时钟 / PA2·PA3 / PD7 / 两个 DMA 通道 / `HAL_UART_Init` / 武装 RX
 * @param       baud  波特率（`APP_MB_BAUD_19200` 等，⛔ 不要直接写字面量）
 * @retval      RS485_OK / RS485_ERR_HAL
 * @note        ⚠️ 必须在 `app_nvic_init()` **之后**调用（那一步把 NVIC 分组钉成 GROUP_4；
 *              分组没钉之前设的优先级数字含义不同）。落点 = `app_main()` 的启动序列。
 * @note        ⛔ 本函数里没有任何 `delay_ms`（S12 / 启动序列里那个 tick 还没走，
 *              见 `app_protect.h` 的 `app_protect_step()` 参数说明）。
 */
uint8_t  rs485_init(uint32_t baud);

/**
 * @brief       运行期改波特率（§6.1 的 0x0303 寄存器用它）
 * @param       baud  新波特率
 * @retval      RS485_OK / RS485_ERR_HAL
 * @note        ⛔ **不得在发送中途调用**：调用方（协议层）必须在"回帧已经发完、TC 已到"
 *              之后才调它，否则会把正在发送的那一帧切成两半（现象 = 主机 CRC 错）。
 */
uint8_t  rs485_set_baud(uint32_t baud);

/**
 * @brief       把收到的一帧取走（**非阻塞**，由 TaskComm 每拍调用）
 * @param       dst / dstsz  输出缓冲与容量
 * @retval      0 = 当前没有待取帧；>0 = 帧长（已拷进 `dst`）
 * @note        ★ 一帧 = **一次 IDLE 事件**（见 `rs485.h` 文件头第 3 条与 `app_config.h` §12.13.1）。
 *              取帧时若 `dstsz` 不够，帧被**丢弃**并计入 `rs485_rx_overrun()`（⛔ 不截断：
 *              截断的帧会通过 CRC 的概率极低但**不为零**，而"半个合法帧"是最坏的一种输入）。
 */
uint16_t rs485_rx_take(uint8_t *dst, uint16_t dstsz);

/**
 * @brief       启动一次异步发送（DMA + TC 中断释放 DE）
 * @param       src / len  待发字节
 * @retval      RS485_OK / RS485_ERR_PARAM / RS485_ERR_BUSY / RS485_ERR_HAL
 * @note        ★ **DE 的释放点只有一处 = `HAL_UART_TxCpltCallback()`（= TC 中断）**，
 *              ⛔ 不得改成"发完 DMA 就放"（`HAL_UART_Transmit_DMA()` 返回时最后一个字节
 *              还在移位寄存器里，早放 = 把最后一字节的停止位切掉 ⇒ 主机偶发 CRC 错）。
 */
uint8_t  rs485_tx_start(const uint8_t *src, uint16_t len);

/**
 * @brief       上一次发送是否还在进行
 * @retval      1 = 忙（DE 仍为高、总线还没放开）
 */
uint8_t  rs485_tx_busy(void);

/**
 * @brief       ★ 真机自检：证明 **TX 的 DMA 请求映射**真的通（见文件头第 3 条 b)）
 * @retval      RS485_OK    发送完成（DMA 通道映射 + USART2 的 DMA 请求 + TC 中断**三者同时成立**）
 *              RS485_ERR_HAL 启动失败
 *              RS485_ERR_BUSY 超时（≈`RS485_SELFTEST_WAIT_MS`）—— 通道挂错时就是这个结果
 * @note        ⚠️ 自检期间 **DE 保持在接收态**（收发器驱动关闭）⇒ 测试字节只走到 MCU 的
 *              TX 引脚，**A/B 总线上不会有任何波形**（⛔ 不会打扰可能挂在上面的主机）。
 *              代价 = 这一格只证明 TX 通路，**不证明** PD7 真的能驱动收发器 ——
 *              后者只能靠台架（把 P5 插上、用转换器看到波形）。
 * @note        计时用**不依赖中断**的 `delay_ms(1)`（C-107：本函数可能跑在调度器之前）。
 */
uint8_t  rs485_selftest(void);

/* ===================== 取证出口（⛔ 不参与任何判据） ===================== */
uint32_t rs485_rx_frames(void);     /* 收到的 IDLE 帧数（含 CRC 错、地址不匹配的） */
uint32_t rs485_rx_overrun(void);    /* 过短/过长/未及时取走而被丢掉的帧数 */
uint32_t rs485_tx_errors(void);     /* 发送启动失败 + 通信错误回调次数 */
uint16_t rs485_last_rx_len(void);   /* 最近一帧的长度（0 = 还没收到过） */

#endif
