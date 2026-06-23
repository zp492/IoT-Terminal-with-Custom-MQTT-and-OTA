/**
 ****************************************************************************************************
 * @file        bootloader_flash.c
 * @brief       Flash 操作封装实现
 * @note        STM32F1 Flash 操作期间会 Stall 总线, 代码必须不在被操作页中执行
 *              Bootloader 代码在 0x08000000~0x0800AFFF, Flag 页在 0x0800B000
 *              被操作的 App 区在 0x0800C000+, 不会影响 Bootloader 本身
 ****************************************************************************************************
 */

#include "bootloader.h"            /* BL_ERR_*, ota_partition.h */
#include "bootloader_flash.h"
#include "bootloader_debug.h"

/* ================================================================================
 * Flash 解锁/上锁
 * ================================================================================ */

static void bl_flash_unlock(void)
{
    HAL_FLASH_Unlock();
}

static void bl_flash_lock(void)
{
    HAL_FLASH_Lock();
}

/* ================================================================================
 * 页擦除
 * ================================================================================ */

int bl_flash_erase_pages(uint32_t start_addr, uint32_t num_pages)
{
    FLASH_EraseInitTypeDef erase_init;
    uint32_t page_error;
    HAL_StatusTypeDef status;

    if (num_pages == 0) return 0;

    /* 参数校验: 地址必须页对齐 */
    if (start_addr % FLASH_PAGE_SIZE != 0) {
        BL_LOG("ERASE: address 0x%08X not page-aligned!", (unsigned)start_addr);
        return BL_ERR_FLASH_ERASE;
    }

    BL_LOG("ERASE: 0x%08X, %u page(s)", (unsigned)start_addr, (unsigned)num_pages);

    erase_init.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase_init.Banks       = FLASH_BANK_1;
    erase_init.PageAddress = start_addr;
    erase_init.NbPages     = num_pages;

    bl_flash_unlock();

    status = HAL_FLASHEx_Erase(&erase_init, &page_error);

    bl_flash_lock();

    if (status != HAL_OK) {
        BL_LOG("ERASE FAILED! page_error=0x%08X", (unsigned)page_error);
        return BL_ERR_FLASH_ERASE;
    }

    return 0;
}

/* ================================================================================
 * 字编程
 * ================================================================================ */

int bl_flash_program_word(uint32_t addr, uint32_t data)
{
    HAL_StatusTypeDef status;

    if (addr % 4 != 0) {
        BL_LOG("PROG: address 0x%08X not word-aligned!", (unsigned)addr);
        return BL_ERR_FLASH_PROGRAM;
    }

    bl_flash_unlock();

    status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr, data);

    bl_flash_lock();

    if (status != HAL_OK) {
        BL_LOG("PROG: FAILED at 0x%08X", (unsigned)addr);
        return BL_ERR_FLASH_PROGRAM;
    }

    return 0;
}

/* ================================================================================
 * 区域拷贝 (先擦除, 再逐字编程)
 * ================================================================================ */

int bl_flash_copy_region(uint32_t src_addr, uint32_t dst_addr, uint32_t size_bytes)
{
    uint32_t num_pages;
    uint32_t num_words;
    uint32_t i;
    int ret;

    if (size_bytes == 0) return 0;

    BL_LOG("COPY: 0x%08X -> 0x%08X, %u bytes",
           (unsigned)src_addr, (unsigned)dst_addr, (unsigned)size_bytes);

    /* ---- 1. 擦除目标区域 ---- */
    num_pages = (size_bytes + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;
    ret = bl_flash_erase_pages(dst_addr, num_pages);
    if (ret != 0) {
        BL_LOG("COPY: erase failed!");
        return ret;
    }

    /* ---- 2. 逐字编程 ---- */
    num_words = size_bytes / 4;
    for (i = 0; i < num_words; i++) {
        uint32_t word = *(volatile uint32_t *)(src_addr + i * 4);
        ret = bl_flash_program_word(dst_addr + i * 4, word);
        if (ret != 0) {
            BL_LOG("COPY: program failed at word %u/%u", (unsigned)i, (unsigned)num_words);
            return ret;
        }

        /* 每 1024 字输出一次进度 (4KB) */
        if ((i & 0x3FF) == 0) {
            BL_LOG("COPY: %u/%u words done", (unsigned)i, (unsigned)num_words);
        }
    }

    /* ---- 3. 处理尾部 ---- */
    if (size_bytes % 4 != 0) {
        /* 从 src 读取剩余 bytes, 用 0xFF 填充高位, 写入 dst */
        uint32_t tail_word = 0xFFFFFFFF;  /* Flash 擦除态 = 0xFF */
        uint32_t remainder = size_bytes % 4;
        const uint8_t *p = (const uint8_t *)(src_addr + num_words * 4);
        for (i = 0; i < remainder; i++) {
            /* 需要改写对应字节: 先清零, 再设值 */
            /* 但 Flash 编程只能 1→0, 所以必须从擦除态 0xFFFFFFFF 开始 */
            /* 这里简单用 memcpy 风格的字节覆盖 */
            ((uint8_t *)&tail_word)[i] = p[i];
        }
        ret = bl_flash_program_word(dst_addr + num_words * 4, tail_word);
        if (ret != 0) {
            BL_LOG("COPY: tail program failed!");
            return ret;
        }
    }

    BL_LOG("COPY: complete (%u bytes)", (unsigned)size_bytes);
    return 0;
}

/* ================================================================================
 * 字节比对验证
 * ================================================================================ */

int bl_flash_verify_region(uint32_t src_addr, uint32_t dst_addr, uint32_t size_bytes)
{
    uint32_t i;

    BL_LOG("VERIFY: comparing 0x%08X vs 0x%08X, %u bytes",
           (unsigned)src_addr, (unsigned)dst_addr, (unsigned)size_bytes);

    for (i = 0; i < size_bytes; i++) {
        uint8_t src_byte = *(volatile uint8_t *)(src_addr + i);
        uint8_t dst_byte = *(volatile uint8_t *)(dst_addr + i);
        if (src_byte != dst_byte) {
            BL_LOG("VERIFY FAIL: offset %u: src=0x%02X dst=0x%02X",
                   (unsigned)i, src_byte, dst_byte);
            return (int)i;  /* 返回不匹配偏移 */
        }
    }

    BL_LOG("VERIFY: OK");
    return 0;
}

/* ================================================================================
 * Flag 页操作
 * ================================================================================ */

/**
 * @brief       擦除升级标志页
 * @note        Flag 页在 0x0800B000 (Bootloader 区域内页 #22)
 *              擦除后所有字节为 0xFF, magic=0xFFFFFFFF → 无效标志
 */
int bl_flash_erase_flag_page(void)
{
    int ret;

    BL_LOG("FLAG: erasing flag page at 0x%08X", (unsigned)FLAG_PAGE_BASE_ADDR);

    ret = bl_flash_erase_pages(FLAG_PAGE_BASE_ADDR, 1);
    if (ret != 0) {
        BL_LOG("FLAG: erase failed!");
    }
    return ret;
}

/**
 * @brief       读取 Flag 页中指定 Word 的值
 * @param       word_index: 0=magic, 1=fw_size, 2=fw_crc32, 3=status
 * @retval      Word 值 (Flash 直接读取)
 */
uint32_t bl_flash_read_flag_word(uint32_t word_index)
{
    uint32_t addr;

    if (word_index >= FLAG_TOTAL_WORDS) {
        return 0xFFFFFFFF;  /* 越界返回擦除态 */
    }

    addr = FLAG_PAGE_BASE_ADDR + word_index * sizeof(uint32_t);
    return *(volatile uint32_t *)addr;
}
