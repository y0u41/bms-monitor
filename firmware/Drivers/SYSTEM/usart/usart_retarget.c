/**
 ****************************************************************************************************
 * @file        usart.c
 * @brief       printf 重定向到 USART1（调试串口）
 * @note        USART1 的初始化由 CubeMX 生成的 Src/usart.c 完成(MX_USART1_UART_Init)。
 *              本文件只负责把 printf 的输出送到 USART1 的数据寄存器。
 *              同时兼容 AC5 / AC6 两种编译器, 并关闭半主机模式。
 ****************************************************************************************************
 */

#include "./SYSTEM/usart/usart.h"
#include "usart.h"      /* CubeMX 生成: 提供 huart1 句柄 */

/* 判断编译器类型, 如果是 AC6 编译器则执行 __use_no_semihosting */
#if defined ( __ARMCC_VERSION ) && ( __ARMCC_VERSION >= 6010050 )
__asm(".global __use_no_semihosting\n\t");   /* 声明不使用半主机模式 */
__asm(".global __ARM_use_no_argv \n\t");     /* AC6 下声明 main 为无参数格式 */
#else
#pragma import(__use_no_semihosting)

struct __FILE
{
    int handle;
};

#endif

/* 不使用半主机模式, 需要重定义以下函数(同时兼容 AC5 / AC6) */
int _ttywrch(int ch)
{
    (void)ch;
    return ch;
}

void _sys_exit(int x)
{
    (void)x;
}

char *_sys_command_string(char *cmd, int len)
{
    (void)cmd;
    (void)len;
    return NULL;
}

FILE __stdout;

/**
 * @brief       printf 底层输出函数
 * @param       ch: 待发送字符
 * @param       f : 文件句柄(未使用)
 * @retval      ch
 * @note        直接写 USART1 的 SR/DR 寄存器, 不依赖 HAL 中断状态, 可在任何时刻调用。
 */
int fputc(int ch, FILE *f)
{
    (void)f;

    while ((USART1->SR & 0X40U) == 0U)      /* 等待上一个字符发送完成(TC 置位) */
    {
        ;
    }

    USART1->DR = (uint8_t)ch;               /* 将要发送的字符写入 DR */

    return ch;
}
