BMS 监测终端 · 4S 磷酸铁锂 · 固件工程（STM32F103ZET6 精英版 V2）
=========================================================================

本工程目录按【正点原子 精英版 V2 HAL 例程】的格式整理。

目录说明
--------
  User/          应用层：main.c / 中断 / HAL 配置 + App/（BMS 业务代码）
  Drivers/BSP/   板级驱动（AFE / SOFTI2C / LCD / SHT30 / RTC / STMFLASH ...）
  Drivers/SYSTEM/  官方 SYSTEM 层（sys / delay / usart）
  Drivers/CMSIS/   CMSIS 内核 + 器件头 + 启动文件 + system_stm32f1xx.c
  Drivers/STM32F1xx_HAL_Driver/   ST 官方 HAL 驱动
  Middlewares/   FreeRTOS V10.4.6（Kernel + portable/GCC/ARM_CM3）
  Projects/MDK-ARM/   MDK 工程 bms.uvprojx（唯一工程文件）
  Output/        编译产物 —— 用根目录的 keilkill.bat 清理
  Tools/         gate_check.sh（G7 机械门禁，G7-1~G7-4 四条检查，见 AGENTS.md §3）

构建
----
  在 Projects/MDK-ARM 下用 MDK 打开 bms.uvprojx，目标 bms，IROM1 = 0x70000（448KB）。
  命令行： UV4.exe -r bms.uvprojx -j0 -o b.log     （日志为 GBK）

注意
----
  1) 工程内不含 CubeMX 的 BMS.ioc / .mxproject（已删除）。
     引脚唯一来源是 Tech_Design.md §2，应用层唯一来源是 User/。
     ⚠️ 日后若要用 CubeMX，请在**本工程目录之外**另建工程；不要往回生成——
        它会重建 Core/ 并直接覆盖 Projects/MDK-ARM/bms.uvprojx。
  2) 尾部 64KB Flash 归数据区（参数 / 累计量 / 标定表 / 事件日志），
     详见 app_config.h §13 与 Tech_Design.md §8。
  3) DHT11 与 LSENS 是前序环境监测工程留下的模块，暂留（app_main.c 仍在调用），
     待 app_main.c 按 5 任务迁移完成后一并删除。
