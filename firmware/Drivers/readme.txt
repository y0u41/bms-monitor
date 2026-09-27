    该文件夹下面存放驱动层代码，包括：
    1，BSP：本工程的板级支持包驱动代码
       AFE         bq76920 前端：afe.c/afe.h（统一接口）+ afe_hal.h + afe_hal_bq76920.c（寄存器与 CRC-8）
       SOFTI2C     软件 I2C（单实现 · 双实例：AFE 总线 PD11/PD12，传感总线 PB6/PB7）
       LCD         3.5 寸屏（FSMC，复用官方 lcd.c）
       SHT30       环境温度（I2C 0x44）
       RTC         实时时钟（LSE + VBAT）
       STMFLASH    内部 Flash 读写（参数 / 日志）
       ESP8266     WiFi 上报（加分项）
       LED / BEEP / KEY   板载指示与交互
       DHT11 / LSENS      前序环境监测工程遗留，暂留（app_main.c 仍在调用），
                          待 5 任务迁移完成后删除
    2，SYSTEM：板级基础层（sys 公共头 / delay 微秒级延时 / usart printf 重定向）
    3，CMSIS：ARM CMSIS 内核头 + STM32F1xx 器件头 + 启动文件 + system_stm32f1xx.c
    4，STM32F1xx_HAL_Driver：ST 官方 HAL 驱动

    注意：AFE 的寄存器读写与"写 1 清位"只允许出现在 BSP/AFE 内；
          应用层通过 afe.h 的 afe_hw_fault() 等接口取解码后的结果（AGENTS.md S15）。
