该文件夹下存放中间件。

本工程只有一项：
  FreeRTOS/            FreeRTOS Kernel V10.4.6
    ├── *.c            Kernel 源文件（tasks / queue / list / event_groups /
    │                  stream_buffer / croutine）
    ├── include/       Kernel 头文件
    └── portable/
        ├── GCC/ARM_CM3/   ★ Cortex-M3 移植层（port.c / portmacro.h）
        └── MemMang/heap_4.c

要点：
  1) 移植层必须用 GCC/ARM_CM3。原 RVDS/ARM_CM3 是 armcc 专有语法，
     AC6(armclang) 编译不过，已删除。
  2) 配置在 User/FreeRTOSConfig.h（不在本目录）。
  3) configUSE_TIMERS = 0 ⇒ timers.c 不参与编译，已从磁盘删除。
  4) 静态分配：configSUPPORT_STATIC_ALLOCATION = 1，禁止动态内存（AGENTS.md G7-2）。
