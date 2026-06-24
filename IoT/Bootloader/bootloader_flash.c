/**
 ****************************************************************************************************
 * @file        bootloader_flash.c
 * @author      zp492
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

    erase_init.TypeErase   = FLASH_TYPEERASE_PAGES;//页擦除
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
 * 字编程（单字节写入flash）
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
 * 逐页原子搬运 (核心: 每页独立完成 读→擦→写→验 四步)
 *
 * 原子性保证:
 *   每页操作是独立的原子单元. 如果在某页中途断电:
 *   - Flag 页完好 → Bootloader 下次上电重新执行整个搬运
 *   - 已完成的页: 数据已写入并验证, 重复写入幂等
 *   - 当前页:    擦除态 (0xFF) 或部分写入, 重复写入补全
 *   - 未开始的页: 仍是旧固件 (但旧固件擦除不影响, 因为后续会被覆盖)
 *
 *   断电窗口极小化: 擦除一页 40ms, 写入一页 10ms, 验证一页 <1ms.
 *   总共每页 ~50ms, 任意时刻断电只影响当前一页.
 * ================================================================================ */

/* 页缓冲区: 2KB (512 个 uint32_t), 静态分配避免栈溢出 */
#define BL_PAGE_BUF_WORDS  (FLASH_PAGE_SIZE / sizeof(uint32_t))  /* 512 */
static uint32_t g_page_buf[BL_PAGE_BUF_WORDS];

int bl_flash_copy_region(uint32_t src_addr, uint32_t dst_addr, uint32_t size_bytes,
                         void (*progress_cb)(uint32_t page, uint32_t total))
{
    uint32_t num_pages;
    uint32_t page;
    uint32_t offset;
    int ret;

    if (size_bytes == 0) return 0;

    num_pages = (size_bytes + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;

    BL_LOG("COPY: 0x%08X -> 0x%08X, %u bytes (%u page(s))",
           (unsigned)src_addr, (unsigned)dst_addr,
           (unsigned)size_bytes, (unsigned)num_pages);

    /* ---- 逐页搬运: 每页完整走 读→擦→写→验 ---- */
    for (page = 0, offset = 0; page < num_pages; page++) {
        uint32_t page_src  = src_addr + offset;
        uint32_t page_dst  = dst_addr + offset;
        uint32_t page_size;
        uint32_t page_words;
        uint32_t w;

        /* 计算当前页实际字节数 (最后一页可能不足 2KB) */
        if (page == num_pages - 1 && (size_bytes % FLASH_PAGE_SIZE) != 0) {
            page_size = size_bytes % FLASH_PAGE_SIZE;
        } else {
            page_size = FLASH_PAGE_SIZE;
        }
        page_words = (page_size + 3) / 4;  /* 向上取整到 32-bit 字 */

        BL_LOG("  Page %u/%u: src=0x%08X dst=0x%08X size=%u",
               (unsigned)(page + 1), (unsigned)num_pages,
               (unsigned)page_src, (unsigned)page_dst, (unsigned)page_size);

        /* ----------------------------------------------------------------
         * Step A — 读取暂存区: 将 Download 区当前页拷贝到 RAM 缓冲区
         * ---------------------------------------------------------------- */
        {
            const uint32_t *src = (const uint32_t *)(uintptr_t)page_src;
            for (w = 0; w < page_words; w++) {
                g_page_buf[w] = src[w];
            }
            /* 如果最后一页不足整字, 高位保持擦除态 0xFF (缓冲区未初始化的部分) */
        }

        /* ----------------------------------------------------------------
         * Step B — 擦除 App 区: 擦除目标页 (一页 = 2KB, ~40ms)
         * ---------------------------------------------------------------- */
        ret = bl_flash_erase_pages(page_dst, 1);
        if (ret != 0) {
            BL_LOG("  ERASE FAILED at page %u", (unsigned)page);
            return ret;
        }

        /* ----------------------------------------------------------------
         * Step C — 写入 App 区: 将 RAM 缓冲区逐字编程到目标页
         * ---------------------------------------------------------------- */
        for (w = 0; w < page_words; w++) {
            ret = bl_flash_program_word(page_dst + w * 4, g_page_buf[w]);
            if (ret != 0) {
                BL_LOG("  WRITE FAILED at page %u word %u", (unsigned)page, (unsigned)w);
                return ret;
            }
        }

        /* ----------------------------------------------------------------
         * Step D — 回读校验: 逐字节比对目标页与源页
         * ---------------------------------------------------------------- */
        {
            uint32_t b;
            ret = 0;
            for (b = 0; b < page_size; b++) {
                uint8_t src_byte = *(volatile uint8_t *)(page_src + b);
                uint8_t dst_byte = *(volatile uint8_t *)(page_dst + b);
                if (src_byte != dst_byte) {
                    BL_LOG("  VERIFY FAIL: page %u offset %u: src=0x%02X dst=0x%02X",
                           (unsigned)page, (unsigned)b, src_byte, dst_byte);
                    ret = BL_ERR_FLASH_VERIFY;
                    break;
                }
            }
            if (ret != 0) {
                return ret;
            }
        }

        offset += page_size;

        /* 进度回调 (LCD 更新等) */
        if (progress_cb) {
            progress_cb(page + 1, num_pages);
        }

        /* 日志: 每 8 页或最后一页 */
        if ((page & 0x07) == 0 || page == num_pages - 1) {
            BL_LOG("  Page %u/%u OK (%u%%)",
                   (unsigned)(page + 1), (unsigned)num_pages,
                   (unsigned)((offset * 100UL) / size_bytes));
        }
    }

    BL_LOG("COPY: all %u page(s) verified OK (%u bytes)", (unsigned)num_pages, (unsigned)size_bytes);
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
