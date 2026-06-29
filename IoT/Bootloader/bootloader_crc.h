/**
 ****************************************************************************************************
 * @file        bootloader_crc.h
 * @author      zp492
 * @brief       硬件 CRC32 封装 (STM32F1 CRC 外设, HAL 库)
 * @note        多项式 0x04C11DB7 (CRC-32/MPEG2), 与 Ethernet CRC 一致
 *              输入: 32-bit 字对齐数据, 尾部不足 4 字节用 0x00 填充 LSB
 *              移植说明: 服务端必须使用相同多项式/初始值/填充规则计算 CRC
 ****************************************************************************************************
 */

#ifndef __BOOTLOADER_CRC_H
#define __BOOTLOADER_CRC_H

#include "stm32f1xx.h"
#include "stm32f1xx_hal_crc.h"

/**
 * @brief       初始化 CRC 外设 (使能时钟 + 初始化 HAL 句柄)
 * @retval      0 成功
 */
int bl_crc32_init(void);

/**
 * @brief       计算 Flash 范围内数据的 CRC32
 * @param       start_addr: Flash 起始地址 (必须 4 字节对齐)
 * @param       size_bytes: 数据字节数
 * @retval      CRC32 校验值 (uint32_t)
 * @note        尾部不足 4 字节部分用 0x00 填充 LSB 位置再送入 CRC
 *              例如: 剩余 2 字节 [0xDE, 0xAD] → 送入 0x0000DEAD
 */
uint32_t bl_crc32_calculate(uint32_t start_addr, uint32_t size_bytes);

#endif /* __BOOTLOADER_CRC_H */
