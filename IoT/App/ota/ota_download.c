/**
 ****************************************************************************************************
 * @file        ota_download.c
 * @author      zp492
 * @brief       OTA 下载 — 独立 FreeRTOS 任务, Flash 擦写在任务上下文中执行
 * @note        mqtt_on_cmd 只入队 (ota_handle_packet), OTA 任务负责所有重操作
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
#include "cJSON/cjson_port.h"

/* FreeRTOS */
#include "task.h"

/* ---- MQTT 任务句柄 (OTA 激活时挂起, 完成时恢复) ---- */
extern TaskHandle_t mqtt_task_handler;
static TaskHandle_t ota_self = NULL;
static unsigned portBASE_TYPE ota_orig_prio = 2;

/* OTA 激活: 挂起 MQTT + OTA 升最高 (用于长时间擦除, 期间无数据接收) */
static void ota_takeover(void)
{
    if (mqtt_task_handler) vTaskSuspend(mqtt_task_handler);
    if (ota_self) vTaskPrioritySet(ota_self, configMAX_PRIORITIES - 1);
}

/* OTA 半释放: 恢复 MQTT (数据接收需要), OTA 保持最高优先级处理队列 */
static void ota_resume_mqtt(void)
{
    if (mqtt_task_handler) vTaskResume(mqtt_task_handler);
    /* OTA 保持最高优先级: 从队列取到数据后立即处理, 不会被 MQTT 抢占 */
}

/* OTA 休眠: 降回原优先级 + 恢复 MQTT (出错或取消时) */
static void ota_release(void)
{
    if (ota_self) vTaskPrioritySet(ota_self, ota_orig_prio);
    if (mqtt_task_handler) vTaskResume(mqtt_task_handler);
}

#define OTA_LOG(fmt, ...)  printf("[OTA] " fmt "\r\n", ##__VA_ARGS__)

/* ---- OTA 二进制帧: [OTAD][seq:2B BE][data] ---- */
#define OTA_MAGIC_0     0x4FU
#define OTA_MAGIC_1     0x54U
#define OTA_MAGIC_2     0x41U
#define OTA_MAGIC_3     0x44U
#define OTA_HEADER_LEN  6U

/* ---- 静态状态 ---- */
static ota_state_t       g_state      = OTA_IDLE;
static ota_err_t         g_err        = OTA_ERR_NONE;
static ota_progress_cb_t g_progress_cb = NULL;

/* ---- 下载参数 ---- */
static uint32_t g_fw_total_size   = 0;
static uint32_t g_fw_expected_crc = 0;
static uint32_t g_fw_received     = 0;
static uint16_t g_next_seq        = 0;

/* ---- CRC ---- */
static CRC_HandleTypeDef g_crc_handle;

/* ---- 内置函数 ---- */
static void ota_process_item(const ota_queue_item_t *item);
static void ota_process_start_json(cJSON *root);
static void ota_process_data_chunk(const uint8_t *payload, uint16_t len);
static void ota_process_end(void);
static void ota_process_cancel(void);

static int  ota_erase_download_area(void);
static int  ota_write_chunk_to_flash(const uint8_t *data, uint16_t len);
static void ota_verify_and_finish(void);
static void ota_write_flag(void);
static void ota_set_error(ota_err_t err);
static void ota_notify_progress(void);

/* ================================================================================
 * 公共 API
 * ================================================================================ */

/**
 * @brief       入队 OTA 数据包 (mqtt_on_cmd 调用, 立即返回)
 */
void ota_handle_packet(const uint8_t *payload, uint16_t len, QueueHandle_t ota_queue)
{
    ota_queue_item_t item;

    if (len > OTA_QUEUE_ITEM_DATA_MAX) {
        len = OTA_QUEUE_ITEM_DATA_MAX;
    }

    memset(&item, 0, sizeof(item));
    item.len = len;
    memcpy(item.data, payload, len);// 复制数据到队列项

    /* 非阻塞发送: 队列满则丢弃 (MQTT 继续运行, 不会卡住) */
    if (xQueueSendToBack(ota_queue, &item, 0) != pdTRUE) {
        OTA_LOG("WARN: queue full, dropping packet");
    }
}

/**
 * @brief       OTA 任务主循环 — 独立 FreeRTOS 任务入口
 * @note        队列阻塞等待, 收到包后处理; Flash 操作在此上下文执行
 */
void ota_task_run(QueueHandle_t ota_queue)
{
    ota_queue_item_t item;

    ota_self = xTaskGetCurrentTaskHandle();
    ota_orig_prio = uxTaskPriorityGet(ota_self);
    OTA_LOG("OTA task started (pri=%u), waiting for commands...", ota_orig_prio);

    while (1) {
        /* 阻塞等待 OTA 数据包 */
        if (xQueueReceive(ota_queue, &item, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        /* 处理数据包 */
        ota_process_item(&item);

        /* 下载完成 → 延时后复位 */
        if (g_state == OTA_DONE) {
            OTA_LOG("Rebooting in 3 seconds...");
            vTaskDelay(pdMS_TO_TICKS(3000));
            NVIC_SystemReset();
            /* NOTREACHED */
        }
    }
}
/*查询函数*/
ota_state_t ota_get_state(void)   { return g_state; }
ota_err_t   ota_get_error(void)   { return g_err; }

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

void ota_set_progress_callback(ota_progress_cb_t cb) { g_progress_cb = cb; }

uint8_t ota_get_progress(void)
{
    if (g_fw_total_size == 0) return 0;
    return (uint8_t)((g_fw_received * 100UL) / g_fw_total_size);
}

/* ================================================================================
 * 包分派: 根据类型转到对应处理函数
 * ================================================================================ */

static void ota_process_item(const ota_queue_item_t *item)
{
    const uint8_t *p = item->data;
    uint16_t len = item->len;

    /* ---- OTAD 二进制帧 ---- */
    if (len >= OTA_HEADER_LEN &&
        p[0] == OTA_MAGIC_0 && p[1] == OTA_MAGIC_1 &&
        p[2] == OTA_MAGIC_2 && p[3] == OTA_MAGIC_3) {
        ota_process_data_chunk(p, len);
        return;
    }

    /* ---- JSON 指令 ---- */
    if (len > 0 && p[0] == '{') {
        cJSON *root = cJSON_ParseWithLength((const char *)p, len);
        if (root == NULL) {
            OTA_LOG("JSON parse error");
            return;
        }

        if (cJSON_GetObjectItem(root, "ota_start")) {
            ota_process_start_json(root);
        } else if (cJSON_GetObjectItem(root, "ota_end")) {
            ota_process_end();
        } else if (cJSON_GetObjectItem(root, "ota_cancel")) {
            ota_process_cancel();
        }
        cJSON_Delete(root);
        return;
    }
}

/* ================================================================================
 * ota_start 处理 — 解析 JSON + 擦 Download 区
 * ================================================================================ */

static void ota_process_start_json(cJSON *root)
{
    cJSON *item;
    unsigned long parsed_size = 0;
    unsigned long parsed_crc  = 0;

    if (g_state != OTA_IDLE) {
        OTA_LOG("start ignored: already in state %d", (int)g_state);
        return;
    }

    /* 解析 "size":N */
    item = cJSON_GetObjectItem(root, "size");
    if (!cJSON_IsNumber(item) || item->valuedouble <= 0) {
        OTA_LOG("start: invalid or missing size");
        ota_set_error(OTA_ERR_INVALID_CMD);
        return;
    }
    parsed_size = (unsigned long)item->valuedouble;

    if (parsed_size > DOWNLOAD_SIZE) {
        OTA_LOG("start: size %lu exceeds DOWNLOAD_SIZE %lu", parsed_size, (unsigned long)DOWNLOAD_SIZE);
        ota_set_error(OTA_ERR_INVALID_CMD);
        return;
    }

    /* 解析 "crc32":N (可选, 不存在时默认为 0) */
    item = cJSON_GetObjectItem(root, "crc32");
    if (cJSON_IsNumber(item)) {
        parsed_crc = (unsigned long)item->valuedouble;
    }

    g_fw_total_size   = (uint32_t)parsed_size;
    g_fw_expected_crc = (uint32_t)parsed_crc;
    g_fw_received     = 0;
    g_next_seq        = 0;
    g_err             = OTA_ERR_NONE;

    OTA_LOG("start: size=%lu CRC=0x%08lX", parsed_size, parsed_crc);

    /* 接管: 挂起 MQTT + OTA 升最高 (擦除期间无数据接收, 无需 MQTT) */
    ota_takeover();

    /* 初始化 CRC 外设 */
    __HAL_RCC_CRC_CLK_ENABLE();
    g_crc_handle.Instance = CRC;

    /* ---- 擦除 Download 区 (耗时 4~5 秒, MQTT 已挂起) ---- */
    g_state = OTA_ERASING;
    ota_notify_progress();

    OTA_LOG("erasing Download area...");
    if (ota_erase_download_area() != 0) {
        OTA_LOG("erase FAILED!");
        ota_set_error(OTA_ERR_FLASH_ERASE);
        return;
    }

    /* LCD 提示 */
    lcd_clear(WHITE);
    lcd_show_string(0, 40, 800, 32, 24, (char *)"Downloading Firmware...", BLUE);
    lcd_show_string(0, 100, 800, 24, 16, (char *)"DO NOT POWER OFF!", RED);

    g_state = OTA_RECEIVING;
    ota_resume_mqtt();  /* 恢复 MQTT (数据到来需要它), OTA 保持最高优先级处理队列 */
    ota_notify_progress();
}

/* ================================================================================
 * ota_data 处理 — 写 Flash
 * ================================================================================ */

static void ota_process_data_chunk(const uint8_t *payload, uint16_t len)
{
    uint16_t seq;
    uint16_t data_len;
    uint8_t pct;
    uint32_t bar_w;

    if (g_state != OTA_RECEIVING) {
        OTA_LOG("data ignored: state=%d", (int)g_state);
        return;
    }

    seq      = ((uint16_t)payload[4] << 8) | payload[5];
    data_len = len - OTA_HEADER_LEN;

    /* 序号检查 */
    if (seq != g_next_seq) {
        OTA_LOG("seq gap: expected %u got %u", g_next_seq, seq);
        ota_set_error(OTA_ERR_SEQ_GAP);
        return;
    }

    /* 边界检查 */
    if (g_fw_received + data_len > g_fw_total_size) {
        OTA_LOG("overflow: %lu + %u > %lu",
                (unsigned long)g_fw_received, data_len, (unsigned long)g_fw_total_size);
        ota_set_error(OTA_ERR_SIZE_MISMATCH);
        return;
    }

    /* ---- 重操作: 写 Flash (~10ms) ---- */
    if (data_len > 0) {
        if (ota_write_chunk_to_flash(payload + OTA_HEADER_LEN, data_len) != 0) {
            OTA_LOG("flash write failed at offset=%lu", (unsigned long)g_fw_received);
            ota_set_error(OTA_ERR_FLASH_WRITE);
            return;
        }
        g_fw_received += data_len;
    }

    g_next_seq++;

    /* ---- 进度 (LCD) ---- */
    pct = ota_get_progress();
    if ((g_fw_received & 0xFFF) == 0 || g_fw_received >= g_fw_total_size) {
        OTA_LOG("progress: %lu/%lu (%u%%)",
                (unsigned long)g_fw_received, (unsigned long)g_fw_total_size, pct);
    }

    bar_w = (700UL * (uint32_t)pct) / 100;
    lcd_draw_rectangle(48, 198, 752, 236, GRAY);
    if (bar_w > 0) { lcd_fill(50, 200, 50 + (uint16_t)bar_w, 234, (pct == 100) ? GREEN : BLUE); }
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u%%", pct);
        lcd_fill(350, 250, 450, 270, WHITE);
        lcd_show_string(0, 250, 800, 24, 16, buf, BLACK);
    }

    ota_notify_progress();
}

/* ================================================================================
 * ota_end / ota_cancel
 * ================================================================================ */

static void ota_process_end(void)
{
    if (g_state != OTA_RECEIVING) {
        OTA_LOG("end ignored: state=%d", (int)g_state);
        return;
    }

    OTA_LOG("end: %lu/%lu bytes", (unsigned long)g_fw_received, (unsigned long)g_fw_total_size);

    if (g_fw_received != g_fw_total_size) {
        ota_set_error(OTA_ERR_SIZE_MISMATCH);
        return;
    }

    /* ---- 重操作: CRC32 校验 + 写 Flag ---- */
    ota_verify_and_finish();
}

static void ota_process_cancel(void)
{
    OTA_LOG("cancelled by platform");
    ota_release();  /* 恢复 MQTT, OTA 降回原优先级 */
    g_state = OTA_IDLE;
    g_err   = OTA_ERR_NONE;
    ota_notify_progress();
}

/* ================================================================================
 * Flash 操作
 * ================================================================================ */

static int ota_erase_download_area(void)
{
    FLASH_EraseInitTypeDef ei;
    uint32_t pe;
    uint32_t np = (g_fw_total_size + FLASH_PAGE_SIZE - 1) / FLASH_PAGE_SIZE;

    ei.TypeErase   = FLASH_TYPEERASE_PAGES;
    ei.Banks       = FLASH_BANK_1;
    ei.PageAddress = DOWNLOAD_BASE_ADDR;
    ei.NbPages     = np;

    HAL_FLASH_Unlock();
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&ei, &pe);
    HAL_FLASH_Lock();
    return (st == HAL_OK) ? 0 : -1;
}

static int ota_write_chunk_to_flash(const uint8_t *data, uint16_t len)
{
    uint32_t addr = DOWNLOAD_BASE_ADDR + g_fw_received;
    uint16_t aligned = len & 0xFFFCU;
    uint16_t i;

    HAL_FLASH_Unlock();

    for (i = 0; i < aligned; i += 4) {
        uint32_t w = ((uint32_t)data[i]) | ((uint32_t)data[i+1] << 8)
                   | ((uint32_t)data[i+2] << 16) | ((uint32_t)data[i+3] << 24);
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + i, w) != HAL_OK) {
            HAL_FLASH_Lock();
            return -1;
        }
    }

    if ((len & 3) != 0) {
        uint32_t w = 0xFFFFFFFFUL;
        for (i = aligned; i < len; i++) { ((uint8_t *)&w)[i & 3] = data[i]; }
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD, addr + aligned, w) != HAL_OK) {
            HAL_FLASH_Lock();
            return -1;
        }
    }

    HAL_FLASH_Lock();
    return 0;
}

/* ================================================================================
 * CRC32 校验 + 写 Flag
 * ================================================================================ */

static void ota_verify_and_finish(void)
{
    uint32_t computed;
    uint32_t wc = g_fw_total_size / 4;
    uint32_t i;

    g_state = OTA_VERIFYING;
    ota_notify_progress();

    __HAL_CRC_DR_RESET(&g_crc_handle);
    for (i = 0; i < wc; i++) {
        CRC->DR = *(volatile uint32_t *)(DOWNLOAD_BASE_ADDR + i * 4);
    }//逐字计算

    if (g_fw_total_size % 4) {
        uint32_t tail = 0;
        uint32_t rem = g_fw_total_size % 4;
        const uint8_t *p = (const uint8_t *)(DOWNLOAD_BASE_ADDR + wc * 4);
        for (i = 0; i < rem; i++) { ((uint8_t *)&tail)[i] = p[i]; }
        CRC->DR = tail;
    }//逐字节计算

    computed = CRC->DR;
    OTA_LOG("CRC expected=0x%08lX computed=0x%08lX",
            (unsigned long)g_fw_expected_crc, (unsigned long)computed);

    if (computed != g_fw_expected_crc) {
        ota_set_error(OTA_ERR_CRC_MISMATCH);
        return;
    }

    g_state = OTA_WRITING_FLAG;
    ota_notify_progress();

    ota_write_flag();
}

static void ota_write_flag(void)
{
    FLASH_EraseInitTypeDef ei;
    uint32_t pe;
    ota_flag_t flag_buf;

    /* 擦 Flag 页 */
    ei.TypeErase   = FLASH_TYPEERASE_PAGES;
    ei.Banks       = FLASH_BANK_1;
    ei.PageAddress = FLAG_PAGE_BASE_ADDR;
    ei.NbPages     = 1;

    HAL_FLASH_Unlock();
    if (HAL_FLASHEx_Erase(&ei, &pe) != HAL_OK) {
        HAL_FLASH_Lock();
        ota_set_error(OTA_ERR_FLAG_WRITE);
        return;
    }

    /* 准备 Flag 结构体 → 逐字段写入 Flash */
    flag_buf.magic    = OTA_FLAG_MAGIC;
    flag_buf.fw_size  = g_fw_total_size;
    flag_buf.fw_crc32 = g_fw_expected_crc;

    {
        uint32_t *pw = (uint32_t *)&flag_buf;
        uint32_t i;
        for (i = 0; i < sizeof(flag_buf) / 4; i++) {
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_WORD,
                                  FLAG_PAGE_BASE_ADDR + i * 4, pw[i]) != HAL_OK) {
                HAL_FLASH_Lock();
                ota_set_error(OTA_ERR_FLAG_WRITE);
                return;
            }
        }
    }
    HAL_FLASH_Lock();

    /* LCD 完成 */
    lcd_clear(WHITE);
    lcd_show_string(0, 40, 800, 32, 24, (char *)"Download Complete!", GREEN);
    lcd_show_string(0, 120, 800, 24, 16, (char *)"Firmware verified.", BLACK);
    lcd_show_string(0, 160, 800, 24, 16, (char *)"Rebooting to upgrade...", GRAY);

    g_state = OTA_DONE;
    ota_notify_progress();
}

/* ================================================================================
 * 错误处理 + 进度通知
 * ================================================================================ */

static void ota_set_error(ota_err_t err)
{
    char buf[48];
    ota_release();  /* 恢复 MQTT, OTA 降回原优先级 */
    g_state = OTA_ERROR;
    g_err   = err;
    OTA_LOG("ERROR: %s (code=%d)", ota_errstr(err), (int)err);

    lcd_clear(WHITE);
    lcd_show_string(0, 40, 800, 32, 24, (char *)"Download Failed!", RED);
    snprintf(buf, sizeof(buf), "Error: 0x%02X  %s", (unsigned)err, ota_errstr(err));
    lcd_show_string(0, 160, 800, 24, 16, buf, RED);

    ota_notify_progress();
}

static void ota_notify_progress(void)
{
    if (g_progress_cb) {
        g_progress_cb(g_state, g_fw_received, g_fw_total_size);
    }
}
