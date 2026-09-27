    该文件夹下面存放工程文件，包括：
    1，MDK-ARM：Keil MDK 工程（bms.uvprojx，Arm Compiler 6 / ARMCLANG V6.24，目标名 bms）

    编译产物不在这里，而是输出到工程根目录的 Output\ 下（与正点原子官方例程一致）。
    清理编译产物请用工程根目录的 keilkill.bat。

    注意：
    1) 工程上限 IROM1 = 0x70000（448KB）—— 尾部 64KB 归数据区，不得占用。
       已设好并验证：bms.map 中 LR_IROM1 / ER_IROM1 的 Max = 0x00070000；
       镜像超出 448KB 时链接器会直接报错（当前镜像仅 0x12478 ≈ 73KB）。
       ⚠️ 该值落在 uvprojx 的 <OCR_RVCT4> 元素里，改 <Cpu> 或 <OnChipMemories><IROM>
          对生成的散转文件无效（已实测）。用 GUI 改：Options for Target → Target →
          Read/Only Memory Areas → IROM1 Size。
    2) 以下三项若被 CubeMX 重新生成会被按默认路径重写，必须重做：
       - IROM1 大小（默认 0x80000，要改回 0x70000）
       - User\stm32f1xx_it.c 里屏蔽 SVC_Handler / PendSV_Handler（否则与 FreeRTOS port.c 撞 L6200E）
       - User\stm32f1xx_it.c 的 SysTick_Handler 中调用 xPortSysTickHandler()
       完整补丁表见 AGENTS.md §3。
    3) 工程内不含 BMS.ioc / .mxproject（已删除，避免误点 Generate 覆盖本工程）；
       本工程应用层的唯一来源是 User\。
