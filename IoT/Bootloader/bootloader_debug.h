/**
 ****************************************************************************************************
 * @file        bootloader_debug.h
 * @brief       串口日志输出模块
 * @note        复用 Drivers/SYSTEM/usart/usart.c 的 printf 重定向 (USART1, 115200 8N1)
 *              通过 BL_DEBUG_ENABLE 宏可在编译期静默所有日志
 ****************************************************************************************************
 */

#ifndef __BOOTLOADER_DEBUG_H
#define __BOOTLOADER_DEBUG_H

#include <stdio.h>

/* ---- 编译开关: 置 0 可关闭所有 Bootloader 串口输出 ---- */
#define BL_DEBUG_ENABLE  1

#if BL_DEBUG_ENABLE
  #define BL_LOG(fmt, ...)   printf("[BL] " fmt "\r\n", ##__VA_ARGS__)
  #define BL_RAW(fmt, ...)   printf(fmt, ##__VA_ARGS__)
#else
  #define BL_LOG(fmt, ...)   ((void)0)
  #define BL_RAW(fmt, ...)   ((void)0)
#endif

void bl_debug_init(void);

#endif /* __BOOTLOADER_DEBUG_H */
