/**
 ****************************************************************************************************
 * @file        ota_download.c
 * @author      zp492
 * @brief       OTA 固件下载模块实现
 * @note        OTA 协议:
 *              ┌─────────────────────────────────────────────────┐
 *              │ 启动: {"cmd":"ota_start","size":N,"crc32":N}   │
 *              │ 数据: [OTAD][seq:2B BE][data...]  每包 ~2KB   │
 *              │ 结束: {"cmd":"ota_end"}                         │
 *              │ 取消: {"cmd":"ota_cancel"}                      │
 *              └─────────────────────────────────────────────────┘
 *
 *              Flash 写入在 MQTT 任务上下文中, 每个字写入 ~20μs,
 *              每包 2KB 约需 10ms 编程时间, 对 MQTT 保活无影响。
 ****************************************************************************************************
 */

#include "ota_download.h"
#include "ota_partition.h"
#include "stm32f1xx_hal_flash.h"
#include "stm32f1xx_hal_flash_ex.h"
#include "stm32f1xx_hal_crc.h"
#include "lcd.h"
#include <string.h>
#include <stdio.h>

/* ---- FreeRTOS ---- */
#include "FreeRTOS.h"
#include "task.h"

/* ---- 调试输出 ---- */
#define OTA_LOG(fmt, ...)  printf("[OTA] " fmt "\r\n", ##__VA_ARGS__)

/* ================================================================================
 * OTA 二进制数据帧格式
 *
 *   Offset  Size  Field
 *   ------  ----  -----
 *   0       4     Magic:  "OTAD" (0x4F 0x54 0x41 0x44)
 *   4       2     Sequence number (big-endian, uint16_t)
 *   6       N     Firmware data chunk
 *
 *   最小帧长: 6 字节 (无数据的空包, 用于最后一块或心跳)
 * ================================================================================ */
#define OTA_MAGIC_0     0x4FU  /* 'O' */
#define OTA_MAGIC_1     0x54U  /* 'T' */
#define OTA_MAGIC_2     0x41U  /* 'A' */
#define OTA_MAGIC_3     0x44U  /* 'D' */
#define OTA_HEADER_LEN  6U

/* ---- 状态 ---- */
static ota_state_t       g_ota_state      = OTA_IDLE;
static ota_err_t         g_ota_err        = OTA_ERR_NONE;
static ota_progress_cb_t g_ota_progress_cb = NULL;

/* ---- 下载参数 ---- */
static uint32_t g_fw_total_size  = 0;      /* 期望总字节数 */
static uint32_t g_fw_expected_crc = 0;     /* 期望 CRC32 */
static uint32_t g_fw_received    = 0;      /* 已接收字节数 */
static uint16_t g_next_seq       = 0;      /* 下一个期望序号 */

/* ---- CRC 句柄 ---- */
static CRC_HandleTypeDef g_crc_handle;

/* --------------------------------------------------------------------------------
 * 内部函数声明
 * -------------------------------------------------------------------------------- */

static void     ota_start(const uint8_t *payload, uint16_t len);
static void     ota_data_chunk(const uint8_t *payload, uint16_t len);
static void     ota_end(void);
static void     ota_cancel(void);
static void     ota_set_error(ota_err_t err);
static int      ota_erase_download_area(void);
static int      ota_write_chunk_to_flash(const uint8_t *data, uint16_t len);
static void     ota_verify_and_finish(void);
static void     ota_write_flag(void);
static void     ota_notify_progress(void);

/* ================================================================================
 * 公共 API
 * ================================================================================ */

/**
 * @brief       处理来自 MQTT 的 OTA 指令/数据包
 * @note        由 mqtt_on_cmd 调用, 在 MQTT 任务上下文中执行
 */
void ota_handle_packet(const uint8_t *payload, uint16_t len)
{
    /* ---- 检测 OTA 二进制数据帧 (magic = "OTAD") ---- */
    if (len >= OTA_HEADER_LEN &&
        payload[0] == OTA_MAGIC_0 && payload[1] == OTA_MAGIC_1 &&
        payload[2] == OTA_MAGIC_2 && payload[3] == OTA_MAGIC_3) {

        ota_data_chunk(payload, len);
        return;
    }

    /* ---- 检测 JSON 指令 (以 '{' 开头) ---- */
    if (len > 0 && payload[0] == '{') {
        /* 转为以 '\0' 结尾的字符串便于 strstr 解析 */
        char cmd_buf[64];
        uint16_t copy_len = (len < sizeof(cmd_buf) - 1) ? len : (sizeof(cmd_buf) - 1);
        memcpy(cmd_buf, payload, copy_len);
        cmd_buf[copy_len] = '\0';

        if (strstr(cmd_buf, "\"ota_start\"")) {
            ota_start(payload, len);
        } else if (strstr(cmd_buf, "\"ota_end\"")) {
            ota_end();
        } else if (strstr(cmd_buf, "\"ota_cancel\"")) {
            ota_cancel();
        }
        /* 其他 JSON 指令不是 OTA 命令, 由上层自己处理 (如 LED 控制) */
        return;
    }

    /* 既不是 JSON 也不是 OTA 二进制帧, 忽略 */
}

ota_state_t ota_get_state(void)
{
    return g_ota_state;
}

ota_err_t ota_get_error(void)
{
    return g_ota_err;
}

const char *ota_errstr(ota_err_t err)
{
    switch (err) {
    case OTA_ERR_NONE:          return "OK";
    case OTA_ERR_FLASH_ERASE:   return "Flash erase failed";
    case OTA_ERR_FLASH_WRITE:   return "Flash write failed";
    case OTA_ERR_SIZE_MISMATCH: return "Size mismatch";
    case OTA_ERR_CRC_MISMATCH:  return "CRC mismatch";
    case OTA_ERR_SEQ_GAP:       return "Sequence gap";
    case OTA_ERR_INVALID_CMD:   return "Invalid command";
    case OTA_ERR_FLAG_WRITE:    return "Flag write failed";
    case OTA_ERR_BUSY:          return "OTA busy";
    default:                    return "Unknown";
    }
}

void ota_set_progress_callback(ota_progress_cb_t cb)
{
    g_ota_progress_cb = cb;
}

uint8_t ota_get_progress(void)
{
    if (g_fw_total_size == 0) return 0;
    return (uint8_t)((g_fw_received * 100UL) / g_fw_total_size);
}

/* ================================================================================
 * 内部函数: OTA 启动
 * ================================================================================ */

static void ota_start(const uint8_t *payload, uint16_t len)
{
    const char *p;
    unsigned long parsed_size;
    unsigned long parsed_crc;
    int ret;

    if (g_ota_state != OTA_IDLE) {
        OTA_LOG("start ignored: state=%d (not idle)", (int)g_ota_state);
        if (g_ota_state == OTA_RECEIVING) {
            /* 已经在下载中, 可能是重发包, 忽略 */
        }
        return;
    }

    /* ---- 解析 JSON: {"cmd":"ota_start","size":237568,"crc32":3735928559} ---- */
    p = (const char *)payload;

    /* 提取 "size":N */
    {
        const char *s = strstr(p, "\"size\":");
        if (s == NULL) {
            OTA_LOG("start: missing 'size' field");
            ota_set_error(OTA_ERR_INVALID_CMD);
            return;
        }
        parsed_size = 0;
        s += 7; /* 跳过 \"size\": */
        while (*s >= '0' && *s <= '9') {
            parsed_size = parsed_size * 10 + (unsigned long)(*s - '0');
            s++;
        }
    }

    /* 提取 "crc32":N */
    {
        const char *s = strstr(p, "\"crc32\":");
        if (s == NULL) {
            OTA_LOG("start: missing 'crc32' field");
            ota_set_error(OTA_ERR_INVALID_CMD);
            return;
        }
        parsed_crc = 0;
        s += 8; /* 跳过 \"crc32\": */
        while (*s >= '0' && *s <= '9') {
            parsed_crc = parsed_crc * 10 + (unsigned long)(*s - '0');
            s++;
        }
    }

    /* 校验大小 */
    if (parsed_size == 0 || parsed_size > DOWNLOAD_SIZE) {
        OTA_LOG("start: invalid size %lu (max %lu)",
                parsed_size, (unsigned long)DOWNLOAD_SIZE);
        ota_set_error(OTA_ERR_SIZE_MISMATCH);
        return;
    }

    /* ---- 记录参数 ---- */
    g_fw_total_size    = (uint32_t)parsed_size;
    g_fw_expected_crc  = (uint32_t)parsed_crc;
    g_fw_received      = 0;
    g_next_seq         = 0;
    g_ota_err          = OTA_ERR_NONE;

    OTA_LOG("start: size=%lu CRC32=0x%08lX", parsed_size, parsed_crc);

    /* ---- 初始化 CRC 外设 (用于接收完成后校验) ---- */
    __HAL_RCC_CRC_CLK_ENABLE();
    g_crc_handle.Instance = CRC;

    /* ---- 擦除 Download 区 ---- */
    g_ota_state = OTA_ERASING;
    ota_notify_progress();

    OTA_LOG("erasing Download area...");
    ret = ota_erase_download_area();
    if (ret != 0) {
        OTA_LOG("erase FAILED!");
        ota_set_error(OTA_ERR_FLASH_ERASE);
        return;
    }

    OTA_LOG("erase done, ready for data (max %lu bytes/chunk)",
            (unsigned long)(2048 - OTA_HEADER_LEN));

    /* LCD: 准备下载 */
    lcd_clear(WHITE);
    lcd_show_string(0, 40, 800, 32, 24, (char *)"Downloading Firmware...", BLUE);
    lcd_show_string(0, 100, 800, 24, 16, (char *)"DO NOT POWER OFF!", RED);

    g_ota_state = OTA_RECEIVING;
    ota_notify_progress();
}

/* ================================================================================
 * 内部函数: OTA 数据块接收
 * ================================================================================ */

static void ota_data_chunk(const uint8_t *payload, uint16_t len)
{
    uint16_t seq;
    uint16_t data_len;
    int ret;

    if (g_ota_state != OTA_RECEIVING) {
        OTA_LOG("data ignored: state=%d (not receiving)", (int)g_ota_state);
        return;
    }

    /* ---- 解析 OTA 帧头 ---- */
    /* payload[0..3] = magic (已由调用方验证) */
    seq = ((uint16_t)payload[4] << 8) | payload[5];
    data_len = len - OTA_HEADER_LEN;

    /* ---- 序号连续性检查 ---- */
    if (seq != g_next_seq) {
        OTA_LOG("seq gap: expected %u got %u (offset=%lu)",
                g_next_seq, seq, (unsigned long)g_fw_received);
        ota_set_error(OTA_ERR_SEQ_GAP);
        return;
    }

    /* ---- 边界检查 ---- */
    if (g_fw_received + data_len > g_fw_total_size) {
        OTA_LOG("overflow: received=%lu + chunk=%u > total=%lu",
                (unsigned long)g_fw_received, data_len, (unsigned long)g_fw_total_size);
        ota_set_error(OTA_ERR_SIZE_MISMATCH);
        return;
    }

    /* ---- 写入 Flash ---- */
    if (data_len > 0) {
        ret = ota_write_chunk_to_flash(payload + OTA_HEADER_LEN, data_len);
        if (ret != 0) {
            OTA_LOG("flash write failed at offset=%lu", (unsigned long)g_fw_received);
            ota_set_error(OTA_ERR_FLASH_WRITE);
            return;
        }
        g_fw_received += data_len;
    }

    g_next_seq++;

    /* ---- 进度: 串口 + LCD 进度条 ---- */
    {
        uint8_t pct = ota_get_progress();
        uint32_t bar_w = (700 * (uint32_t)pct) / 100;

        if ((g_fw_received & 0xFFF) == 0 || g_fw_received >= g_fw_total_size) {
            OTA_LOG("progress: %lu/%lu (%u%%)",
                    (unsigned long)g_fw_received, (unsigned long)g_fw_total_size, pct);
        }

        /* LCD 进度条 */
        lcd_draw_rectangle(48, 198, 752, 236, GRAY);
        if (bar_w > 0) {
            lcd_fill(50, 200, 50 + (uint16_t)bar_w, 234,
                     (pct == 100) ? GREEN : BLUE);
        }
        /* 百分比文字 */
        {
            char pbuf[16];
            snprintf(pbuf, sizeof(pbuf), "%u%%", pct);
            lcd_fill(350, 250, 450, 270, WHITE);
            lcd_show_string(0, 250, 800, 24, 16, pbuf, BLACK);
        }
    }

    ota_notify_progress();
}

/* ================================================================================
 * 内部函数: OTA 结束
 * ================================================================================ */

static void ota_end(void)
{
    if (g_ota_state != OTA_RECEIVING) {
        OTA_LOG("end ignored: state=%d (not receiving)", (int)g_ota_state);
        return;
    }

    OTA_LOG("end: received %lu/%lu bytes",
            (unsigned long)g_fw_received, (unsigned long)g_fw_total_size);

    /* 大小校验 */
    if (g_fw_received != g_fw_total_size) {
        OTA_LOG("size mismatch!");
        ota_set_error(OTA_ERR_SIZE_MISMATCH);
        return;
    }

    /* CRC 校验 + 写 Flag */
    ota_verify_and_finish();
}

/* ================================================================================
 * 内部函数: OTA 取消
 * ================================================================================ */

static void ota_cancel(void)
{
    OTA_LOG("cancelled by platform");
    g_ota_state = OTA_IDLE;
    g_ota_err   = OTA_ERR_NONE;
    ota_notify_progress();
}

/* ================================================================================
 * 内部函数: 擦除 Download 区 (232KB, 116 页)
 * ================================================================================ */

static int ota_erase_download_area(void)
{
    FLASH_EraseInitTypeDef erase_init;
    uint32_t page_error;
    uint32_t num_pages;
    HAL_StatusTypeDef status;

    num_pages = (g_fw_total_size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;

    OTA_LOG("erase: %lu pages from 0x%08lX",
            (unsigned long)num_pages, (unsigned long)DOWNLOAD_BASE_ADDR);

    erase_init.TypeErase   = FLASH_TYPEERASE_PAGES;
    erase_init.Banks       = FLASH_BANK_1;
    erase_init.PageAddress = DOWNLOAD_BASE_ADDR;
    erase_init.NbPages     = num_pages;

    HAL_FLASH_Unlock();
    status = HAL_FLASHEx_Erase(&erase_init, &page_error);
    HAL_FLASH_Lock();

    if (status != HAL_OK) {
        OTA_LOG("erase FAILED at page 0x%08lX", (unsigned long)page_error);
        return -1;
    }

    return 0;
}

/* ================================================================================
 * 内部函数: 将数据块逐字写入 Download 区
 * ================================================================================ */

static int ota_write_chunk_to_flash(const uint8_t *data, uint16_t len)
{
    uint32_t target_addr;
    uint32_t word;
    uint16_t i;
    uint16_t aligned_len;
    HAL_StatusTypeDef status;

    target_addr = DOWNLOAD_BASE_ADDR + g_fw_received;

    HAL_FLASH_Unlock();

    /* 逐 32-bit 字编程 */
    aligned_len = len & 0xFFFCU;  /* 4 字节对齐部分 */
    for (i = 0; i < aligned_len; i += 4) {
        word = ((uint32_t)data[i])
             | ((uint32_t)data[i + 1] << 8)
             | ((uint32_t)data[i + 2] << 16)
             | ((uint32_t)data[i + 3] << 24);

        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                   target_addr + i, word);
        if (status != HAL_OK) {
            HAL_FLASH_Lock();
            OTA_LOG("program FAILED at 0x%08lX", (unsigned long)(target_addr + i));
            return -1;
        }
    }

    /* 尾部 1~3 字节: 用 0xFF 填充高位 (Flash 擦除态) */
    if ((len & 3) != 0) {
        word = 0xFFFFFFFFUL;
        for (i = aligned_len; i < len; i++) {
            /* 清零对应字节 → 填入实际数据 */
            ((uint8_t *)&word)[i & 3] = data[i];
        }
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                   target_addr + aligned_len, word);
        if (status != HAL_OK) {
            HAL_FLASH_Lock();
            OTA_LOG("tail program FAILED");
            return -1;
        }
    }

    HAL_FLASH_Lock();
    return 0;
}

/* ================================================================================
 * 内部函数: CRC32 校验 + 写标志
 * ================================================================================ */

static void ota_verify_and_finish(void)
{
    uint32_t computed_crc;
    uint32_t word_count;
    uint32_t i;

    g_ota_state = OTA_VERIFYING;
    ota_notify_progress();

    OTA_LOG("verifying CRC32...");

    /* 使用硬件 CRC 外设计算 Download 区 CRC32
     * 需要注意: 与 Bootloader 使用相同的算法 (字对齐, 尾部填充 0x00) */
    __HAL_CRC_DR_RESET(&g_crc_handle);

    word_count = g_fw_total_size / 4;
    for (i = 0; i < word_count; i++) {
        uint32_t w = *(volatile uint32_t *)(DOWNLOAD_BASE_ADDR + i * 4);
        CRC->DR = w;
    }

    /* 尾部填充 0x00 */
    if (g_fw_total_size % 4 != 0) {
        uint32_t tail = 0;
        uint32_t rem = g_fw_total_size % 4;
        const uint8_t *p = (const uint8_t *)(DOWNLOAD_BASE_ADDR + word_count * 4);
        for (i = 0; i < rem; i++) {
            ((uint8_t *)&tail)[i] = p[i];
        }
        CRC->DR = tail;
    }

    computed_crc = CRC->DR;

    OTA_LOG("expected CRC: 0x%08lX", (unsigned long)g_fw_expected_crc);
    OTA_LOG("computed CRC: 0x%08lX", (unsigned long)computed_crc);

    if (computed_crc != g_fw_expected_crc) {
        OTA_LOG("CRC MISMATCH!");
        ota_set_error(OTA_ERR_CRC_MISMATCH);
        return;
    }

    OTA_LOG("CRC OK. Writing upgrade flag...");

    g_ota_state = OTA_WRITING_FLAG;
    ota_notify_progress();

    ota_write_flag();
}

/* ================================================================================
 * 内部函数: 写升级标志页
 *
 * 标志页位于 0x0800B000 (Bootloader 区域内, 页 #22).
 * 写入 4 个 32-bit word: magic | fw_size | fw_crc32 | status
 *
 * STM32F1 Flash 编程要求: 必须先擦除页, 再编程各 word.
 * 写入 status = PENDING (0x00000000) 因为 Flash 可编程 0x00 位
 * (从擦除态 0xFF→0x00 是合法操作).
 * ================================================================================ */

static void ota_write_flag(void)
{
    uint32_t words[FLAG_TOTAL_WORDS];
    uint32_t i;
    HAL_StatusTypeDef status;

    /* ---- Step 1: 擦除 Flag 页 ---- */
    {
        FLASH_EraseInitTypeDef erase_init;
        uint32_t page_error;

        erase_init.TypeErase   = FLASH_TYPEERASE_PAGES;
        erase_init.Banks       = FLASH_BANK_1;
        erase_init.PageAddress = FLAG_PAGE_BASE_ADDR;
        erase_init.NbPages     = 1;

        HAL_FLASH_Unlock();
        status = HAL_FLASHEx_Erase(&erase_init, &page_error);
        if (status != HAL_OK) {
            HAL_FLASH_Lock();
            OTA_LOG("flag erase FAILED at 0x%08lX", (unsigned long)page_error);
            ota_set_error(OTA_ERR_FLAG_WRITE);
            return;
        }
    }

    /* ---- Step 2: 逐字编程 Flag ---- */
    words[FLAG_WORD_MAGIC]    = OTA_FLAG_MAGIC;
    words[FLAG_WORD_FW_SIZE]  = g_fw_total_size;
    words[FLAG_WORD_FW_CRC32] = g_fw_expected_crc;
    words[FLAG_WORD_STATUS]   = FLAG_STATUS_PENDING;

    for (i = 0; i < FLAG_TOTAL_WORDS; i++) {
        status = HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                   FLAG_PAGE_BASE_ADDR + i * 4,
                                   words[i]);
        if (status != HAL_OK) {
            HAL_FLASH_Lock();
            OTA_LOG("flag write FAILED at word %lu", (unsigned long)i);
            ota_set_error(OTA_ERR_FLAG_WRITE);
            return;
        }
    }

    HAL_FLASH_Lock();

    OTA_LOG("upgrade flag written. Magic=0x%08lX Size=%lu CRC=0x%08lX",
            (unsigned long)OTA_FLAG_MAGIC,
            (unsigned long)g_fw_total_size,
            (unsigned long)g_fw_expected_crc);

    g_ota_state = OTA_DONE;
    ota_notify_progress();

    /* LCD: 下载完成 */
    lcd_clear(WHITE);
    lcd_show_string(0, 40, 800, 32, 24, (char *)"Download Complete!", GREEN);
    lcd_show_string(0, 120, 800, 24, 16, (char *)"Firmware downloaded & verified.", BLACK);
    lcd_show_string(0, 160, 800, 24, 16, (char *)"Device will reboot to upgrade...", GRAY);

    OTA_LOG("OTA download complete. Rebooting in 3 seconds...");
    vTaskDelay(pdMS_TO_TICKS(3000));

    /* 软复位 → Bootloader 接手升级 */
    NVIC_SystemReset();

    /* never reaches here */
    while (1);
}

/* ================================================================================
 * 内部函数: 设置错误状态
 * ================================================================================ */

static void ota_set_error(ota_err_t err)
{
    char buf[48];
    g_ota_state = OTA_ERROR;
    g_ota_err   = err;
    OTA_LOG("ERROR: %s (code=%d)", ota_errstr(err), (int)err);

    /* LCD: 下载失败 */
    lcd_clear(WHITE);
    lcd_show_string(0, 40, 800, 32, 24, (char *)"Download Failed!", RED);
    lcd_show_string(0, 120, 800, 24, 16, (char *)"Firmware download error:", BLACK);
    snprintf(buf, sizeof(buf), "Error: 0x%02X  %s", (unsigned)err, ota_errstr(err));
    lcd_show_string(0, 160, 800, 24, 16, buf, RED);
    lcd_show_string(0, 220, 800, 24, 16,
                    (char *)"Platform will retry automatically.", GRAY);

    ota_notify_progress();
}

/* ================================================================================
 * 内部函数: 通知进度回调
 * ================================================================================ */

static void ota_notify_progress(void)
{
    if (g_ota_progress_cb) {
        g_ota_progress_cb(g_ota_state, g_fw_received, g_fw_total_size);
    }
}
