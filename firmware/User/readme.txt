    该文件夹下面存放用户层代码，包括：
    1，main.c / stm32f1xx_it.c / stm32f1xx_it.h / stm32f1xx_hal_conf.h / stm32f1xx_hal_msp.c
       CubeMX 生成的应用入口、中断向量与 HAL 配置
    2，adc.c / gpio.c / usart.c (+ .h)
       外设初始化（MX_*_Init），由 CubeMX 生成、按需手改
    3，FreeRTOSConfig.h
       FreeRTOS 配置（静态分配 / 无软件定时器 / 中断优先级上限 5）
    4，App：本工程自写的 BMS 业务逻辑
       app_config.h        全局配置：编译期断言 + BMS 三级门限(§12.7) + Flash 分区(§13)
       app_config_local.h  本机凭据 —— 交付或上传前删除本文件，见附录C · C-37
       app_tasks.c/.h      FreeRTOS 五任务：静态内存池 + 心跳 + 空闲任务回调
       app_nvic.c/.h       NVIC 分组与优先级（GROUP_4；UART=6 / AFE ALERT=5；4 条编译期断言）
       app_main.c          应用主入口（⚠️ 目前仍是前序工程的单线程超循环，待按 5 任务迁移，见 Build.md §8.4）
       app_alarm.c/.h      三级告警判定
       app_log.c/.h        事件日志（内部 Flash + CRC + 队列异步落盘）
       app_mqtt.c/.h       极简 MQTT 3.1.1 报文组装与解析
       app_onenet.c/.h     OneNET 上报主题与 JSON 载荷

    注意：应用层（App/）内不得出现 AFE 寄存器名与执行器引脚宏（AGENTS.md S15 / G7-1）。
