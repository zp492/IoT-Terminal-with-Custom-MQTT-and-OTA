/**
 ****************************************************************************************************
 * @file        bootloader_flash.h
 * @author      zp492
 * @brief       Flash 操作封装 — 页擦除、字编程、区域拷贝、字节比对、Flag 页管理
 * @note        所有操作均加超时保护, 失败时返回负值错误码
 *              STM32F103ZE: Flash 页大小 2KB, 仅支持 16-bit 半字硬件编程
 *              HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD) 内部循环两次半字写
 ****************************************************************************************************
 */

#ifndef __BOOTLOADER_FLASH_H
#define __BOOTLOADER_FLASH_H

#include "stm32f1xx.h"
#include "stm32f1xx_hal_flash.h"
#include "stm32f1xx_hal_flash_ex.h"

/* ---- Flash 操作超时 (ms) ---- */
#define BL_FLASH_TIMEOUT        50000UL     /* 50 秒, 足够擦除 116 页 */

/**
 * @brief       擦除指定范围的 Flash 页
 * @param       start_addr: 起始地址 (必须页对齐, 即 2KB 对齐)
 * @param       num_pages: 擦除页数
 * @retval      0 成功, <0 失败码
 */
int bl_flash_erase_pages(uint32_t start_addr, uint32_t num_pages);

/**
 * @brief       编程一个 32-bit 字到 Flash
 * @param       addr: 目标地址 (必须 4 字节对齐)
 * @param       data: 32-bit 数据
 * @retval      0 成功, <0 失败
 */
int bl_flash_program_word(uint32_t addr, uint32_t data);

/**
 * @brief       从源区域拷贝数据到目标区域 (逐页: 读→擦→写→验)
 * @param       src_addr: 源地址 (Flash 中, 4 字节对齐)
 * @param       dst_addr: 目标地址 (Flash 中, 页对齐)
 * @param       size_bytes: 拷贝字节数
 * @param       progress_cb: 进度回调 (page, total), 可为 NULL
 * @retval      0 成功, <0 失败码
 */
int bl_flash_copy_region(uint32_t src_addr, uint32_t dst_addr, uint32_t size_bytes,
                         void (*progress_cb)(uint32_t page, uint32_t total));

/**
 * @brief       逐字节比对两个 Flash 区域
 * @param       src_addr: 期望数据地址
 * @param       dst_addr: 实际数据地址
 * @param       size_bytes: 比较字节数
 * @retval      0 匹配, >0 首个不匹配的字节偏移, <0 参数错误
 */
int bl_flash_verify_region(uint32_t src_addr, uint32_t dst_addr, uint32_t size_bytes);

/**
 * @brief       擦除升级标志页 (将 flag 页全部恢复为 0xFF)
 * @retval      0 成功, <0 失败
 */
int bl_flash_erase_flag_page(void);

#endif /* __BOOTLOADER_FLASH_H */
