/**
 ****************************************************************************************************
 * @file        bootloader_crc.c
 * @brief       硬件 CRC32 封装实现 (HAL CRC 库, ARMCC5/C89 兼容)
 * @note        STM32F1 CRC 外设:
 *              - 多项式: 0x04C11DB7 (CRC-32/MPEG2, 不可配置)
 *              - 初始值: 0xFFFFFFFF (硬件默认)
 *              - 输入/输出无位反转, 无最终 XOR
 *              - 仅 32-bit 字输入 (小端序), 复位位 = CRC_CR_RESET
 *
 *              步骤: HAL_CRC_Calculate() 处理对齐主体 →
 *                    尾部不足 4 字节 0x00 填充 LSB → 直接写 CRC->DR
 ****************************************************************************************************
 */

#include "bootloader_crc.h"

/* ---- 全局 CRC HAL 句柄 ---- */
static CRC_HandleTypeDef g_crc_handle;

/**
 * @brief       初始化 CRC 外设 (使能时钟 + 关联句柄)
 * @retval      0 成功
 */
int bl_crc32_init(void)
{
    __HAL_RCC_CRC_CLK_ENABLE();

    /* 关联 CRC 外设实例 */
    g_crc_handle.Instance = CRC;

    return 0;
}

/**
 * @brief       计算 Flash 范围内数据的 CRC32
 * @param       start_addr: Flash 起始地址 (必须 4 字节对齐)
 * @param       size_bytes: 数据字节数
 * @retval      CRC32 校验值
 *
 * @note        算法:
 *              1. 复位 CRC 单元 (初始值 → 0xFFFFFFFF)
 *              2. 用 HAL_CRC_Calculate() 输入所有完整 32-bit 字
 *                 (HAL_CRC_Calculate 内部: 先复位, 逐字写 DR, 返回 DR)
 *              3. 尾部 1~3 字节用 0x00 填充 LSB, 直接写入 CRC->DR
 *              4. 读 CRC->DR 返回
 *
 *              注意: HAL_CRC_Calculate() 会自动调用 __HAL_CRC_DR_RESET(),
 *              因此无需手动复位. 但如果 word_count == 0, 则需要手动复位.
 */
uint32_t bl_crc32_calculate(uint32_t start_addr, uint32_t size_bytes)
{
    uint32_t word_count;
    uint32_t remainder;
    uint32_t i;
    uint32_t result;

    if (size_bytes == 0) {
        return 0xFFFFFFFFUL;
    }

    word_count = size_bytes / 4;
    remainder  = size_bytes % 4;

    if (word_count > 0) {
        /* HAL_CRC_Calculate: 内部复位 → 逐字 feed → 返回 CRC 值 */
        result = HAL_CRC_Calculate(&g_crc_handle,
                                   (uint32_t *)(uintptr_t)start_addr,
                                   word_count);
        (void)result;  /* 此时 CRC->DR 已是最新值, 后续若有 remainder 会继续 feed */
    } else {
        /* 没有完整 32-bit 字, 需手动复位 CRC 单元 */
        __HAL_CRC_DR_RESET(&g_crc_handle);
    }

    /* ---- 处理尾部不足 4 字节 ---- */
    if (remainder != 0) {
        uint32_t tail_word;
        const uint8_t *p;

        tail_word = 0;
        p = (const uint8_t *)(uintptr_t)(start_addr + word_count * 4);

        /* 逐字节拼入 32-bit 字 (低位在前, 小端序) */
        for (i = 0; i < remainder; i++) {
            tail_word |= ((uint32_t)p[i]) << (i * 8);
        }

        /* 直接写 DR, 追加到当前 CRC 计算中 */
        CRC->DR = tail_word;
    }

    /* 读取最终 CRC 值 */
    return CRC->DR;
}
