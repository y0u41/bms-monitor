/**
 ****************************************************************************************************
 * @file        stmflash.h
 * @brief       STM32 内部 FLASH 读写(用来模拟 EEPROM, 掉电不丢)
 * @note        移植自正点原子官方 HAL 例程「实验31 FLASH模拟EEPROM」, 读写逻辑与官方逐行一致,
 *              仅做了三处适配:
 *                1, 去掉公司信息页脚与仅用于演示的 test_write();
 *                2, FLASH 总容量按本板(STM32F103ZET6, 512KB)直接定死, 不用手改;
 *                3, 官方头文件里那 6 个只声明、从未定义的 static 函数原型一并删除
 *                   (它们是早期版本残留, 留着会干扰阅读)。
 * @note        写一个数据要点:
 *               F103 的擦除单位是"页"(本板 2KB), 所以 stmflash_write() 只要发现目标位置
 *               不是 0xFFFF, 就会把**整页**读出来、擦掉、改完再整页写回。
 *               因此一次写入的代价是"页擦除(~40ms) + 整页编程", 不要放在高频路径里。
 * @note        ★ BMS 追加(D4, 2026-09-27): 新增 `stmflash_page_is_blank()` / `stmflash_erase_page()`
 *               两个**追加式存储**原语(事件日志用), 移植来源/改动点/验证方式见 stmflash.c 文件头。
 ****************************************************************************************************
 */

#ifndef __STMFLASH_H
#define __STMFLASH_H

#include "./SYSTEM/sys/sys.h"

/* FLASH 起始地址与总容量(本板 STM32F103ZET6 = 512KB) */
#define STM32_FLASH_SIZE        0x80000         /* STM32 FLASH 总大小 */
#define STM32_FLASH_BASE        0x08000000      /* STM32 FLASH 起始地址 */

/* STM32F103 扇区(页)大小: 容量 < 256KB 的型号为 1KB, >= 256KB 的为 2KB */
#if STM32_FLASH_SIZE < 256 * 1024
#define STM32_SECTOR_SIZE       1024
#else
#define STM32_SECTOR_SIZE       2048
#endif

/* FLASH 解锁密钥 */
#define STM32_FLASH_KEY1        0x45670123
#define STM32_FLASH_KEY2        0xCDEF89AB

/* 接口函数 */
uint16_t stmflash_read_halfword(uint32_t faddr);                        /* 读一个半字(16 位) */
void     stmflash_read(uint32_t raddr, uint16_t *pbuf, uint16_t length); /* 读指定长度的半字数组 */
void     stmflash_write(uint32_t waddr, uint16_t *pbuf, uint16_t length);/* 写(自动擦除整页) */

/* ★ BMS 追加(D4): 追加式存储用的两个原语(见文件头 BMS 改动点) */
uint8_t  stmflash_page_is_blank(uint32_t page_addr);                      /* 1 = 整页全 0xFFFF, 可直接追加 */
uint8_t  stmflash_erase_page(uint32_t page_addr);                        /* 只擦一页; 0 = 成功 */

#endif
