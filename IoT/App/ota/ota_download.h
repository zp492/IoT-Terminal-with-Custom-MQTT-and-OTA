/**
 ****************************************************************************************************
 * @file        ota_download.h
 * @brief       OTA 固件下载模块 — 独立 FreeRTOS 任务架构
 * @note        MQTT 回调只做入队 (轻量), Flash 擦写在 OTA 任务中执行 (不阻塞 MQTT).
 *
 *              架构:
 *                mqtt_task (pri 4)                  ota_task (pri 2)
 *                  │                                     │
 *                  ├─ mqtt_on_cmd()                      │
 *                  │   └─ ota_handle_packet()            │
 *                  │       └─ xQueueSend(ota_queue)      │
 *                  │         立即返回!                    │
 *                  │                              ┌──────┘
 *                  │                              │ xQueueReceive(ota_queue)
 *                  │                              │ 处理: 擦Flash / 写Flash / CRC / Flag
 *                  │                              └─ NVIC_SystemReset()
 *
 *              协议:
 *                JSON: {"cmd":"ota_start","size":N,"crc32":N}
 *                二进制: [OTAD][seq:2B BE][data]
 *                JSON: {"cmd":"ota_end"}
 *                JSON: {"cmd":"ota_cancel"}
 ****************************************************************************************************
 */

#ifndef __OTA_DOWNLOAD_H
#define __OTA_DOWNLOAD_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"

/* ================================================================================
 * 队列项 — OTA 数据包 (MQTT 任务 → OTA 任务)
 * ================================================================================ */
#define OTA_QUEUE_ITEM_DATA_MAX  2048   /* 单包最大数据字节数 */

typedef struct {
    uint16_t len;                        /* data[] 有效字节数 (0 = 空/占位) */
    uint8_t  data[OTA_QUEUE_ITEM_DATA_MAX]; /* 包内容: JSON 指令或 OTAD 二进制帧 */
} ota_queue_item_t;

#define OTA_QUEUE_LEN  4                /* 队列深度: 脚本延迟6s后数据在擦除完成后到达 */

/* ================================================================================
 * OTA 下载状态
 * ================================================================================ */
typedef enum {
    OTA_IDLE,               /* 空闲, 等待 ota_start */
    OTA_ERASING,            /* 擦除 Download 区 */
    OTA_RECEIVING,          /* 接收固件数据块, 写 Flash */
    OTA_VERIFYING,          /* CRC32 校验中 */
    OTA_WRITING_FLAG,       /* 写升级标志 */
    OTA_DONE,               /* 下载完成, 等待复位 */
    OTA_ERROR,              /* 出错 */
} ota_state_t;

typedef enum {
    OTA_ERR_NONE            = 0,
    OTA_ERR_FLASH_ERASE     = 1,
    OTA_ERR_FLASH_WRITE     = 2,
    OTA_ERR_SIZE_MISMATCH   = 3,
    OTA_ERR_CRC_MISMATCH    = 4,
    OTA_ERR_SEQ_GAP         = 5,
    OTA_ERR_INVALID_CMD     = 6,
    OTA_ERR_FLAG_WRITE      = 7,
    OTA_ERR_BUSY            = 8,
} ota_err_t;

typedef void (*ota_progress_cb_t)(ota_state_t state, uint32_t received, uint32_t total);

/* ================================================================================
 * 公共 API
 * ================================================================================ */

/**
 * @brief       入队 OTA 指令/数据 (mqtt_on_cmd 调用, 立即返回)
 * @param       payload: MQTT PUBLISH 消息体
 * @param       len:     有效字节数
 * @param       ota_queue: OTA 任务的消息队列句柄
 * @note        仅做拷贝入队, 不执行任何 Flash 操作
 */
void ota_handle_packet(const uint8_t *payload, uint16_t len,
                       QueueHandle_t ota_queue);

/**
 * @brief       OTA 任务主循环 (独立 FreeRTOS 任务入口, 永不返回)
 * @param       ota_queue: OTA 消息队列 (由 freertos_task 创建)
 * @note        处理流程: IDLE → ERASING → RECEIVING → VERIFYING → WRITING_FLAG → DONE
 *              完成后调用 NVIC_SystemReset()
 */
void ota_task_run(QueueHandle_t ota_queue);

ota_state_t ota_get_state(void);
ota_err_t   ota_get_error(void);
const char *ota_errstr(ota_err_t err);
void        ota_set_progress_callback(ota_progress_cb_t cb);
uint8_t     ota_get_progress(void);

#endif /* __OTA_DOWNLOAD_H */
