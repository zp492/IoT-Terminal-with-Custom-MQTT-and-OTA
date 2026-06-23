/**
 ****************************************************************************************************
 * @file        bootloader_debug.c
 * @brief       串口日志输出模块实现
 ****************************************************************************************************
 */

#include "bootloader_debug.h"
#include "./SYSTEM/usart/usart.h"

/**
 * @brief       初始化串口调试输出
 * @note        调用 usart_init() 初始化 USART1 115200 8N1
 *              printf 通过 fputc 重定向 (见 usart.c)
 */
void bl_debug_init(void)
{
    usart_init(115200);
    BL_LOG("=== Bootloader Debug Console ===");
}
