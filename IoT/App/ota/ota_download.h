/**
 ****************************************************************************************************
 * @file        ota_download.h
 * @author      zp492
 * @brief       OTA 固件下载模块 — MQTT 指令分派 + Flash 写入 + CRC 校验 + Flag 写入
 * @note        协议:
 *              启动:  {"cmd":"ota_start","size":N,"crc32":N}  (JSON)
 *              数据:  OTAD + seq(2B BE) + data                (二进制, magic=0x4F544144)
 *              结束:  {"cmd":"ota_end"}                       (JSON)
 *              取消:  {"cmd":"ota_cancel"}                    (JSON)
 *
 *              流程: ota_start → N×ota_data → ota_end → 写Flag → sys_soft_reset()
 *
 *              Flash 操作在 MQTT 任务上下文中执行, OTA 期间 MQTT 阻塞是预期的。
 ****************************************************************************************************
 */

#ifndef __OTA_DOWNLOAD_H
#define __OTA_DOWNLOAD_H

#include <stdint.h>

/* ================================================================================
 * OTA 下载状态
 * ================================================================================ */
typedef enum {
    OTA_IDLE,               /* 空闲 */
    OTA_ERASING,            /* 擦除 Download 区 */
    OTA_RECEIVING,          /* 接收固件数据块 */
    OTA_VERIFYING,          /* CRC32 校验中 */
    OTA_WRITING_FLAG,       /* 写升级标志 */
    OTA_DONE,               /* 下载完成, 等待复位 */
    OTA_ERROR,              /* 出错 */
} ota_state_t;

/* ================================================================================
 * OTA 错误码
 * ================================================================================ */
typedef enum {
    OTA_ERR_NONE            = 0,
    OTA_ERR_FLASH_ERASE     = 1,
    OTA_ERR_FLASH_WRITE     = 2,
    OTA_ERR_SIZE_MISMATCH   = 3,
    OTA_ERR_CRC_MISMATCH    = 4,
    OTA_ERR_SEQ_GAP         = 5,
    OTA_ERR_INVALID_CMD     = 6,
    OTA_ERR_FLAG_WRITE      = 7,
    OTA_ERR_BUSY            = 8,  /* 上一次 OTA 未结束 */
} ota_err_t;

/* ================================================================================
 * OTA 进度回调 (用于更新 LED / LCD / 串口)
 * ================================================================================ */
typedef void (*ota_progress_cb_t)(ota_state_t state, uint32_t received, uint32_t total);

/* ================================================================================
 * 公共 API
 * ================================================================================ */

/**
 * @brief       处理 MQTT 下发的 OTA 指令/数据
 * @param       payload: MQTT PUBLISH 消息体
 * @param       len:     消息体长度
 * @note        由 mqtt_on_cmd 回调调用
 *              - JSON 指令 → {"cmd":"ota_start", ...} / {"cmd":"ota_end"}
 *              - 二进制数据 → OTAD 魔数识别
 *
 *              返回后由调用方 (mqtt_task) 继续运行, 如需复位则调用
 *              ota_trigger_reset() 最后一步。
 */
void ota_handle_packet(const uint8_t *payload, uint16_t len);

/**
 * @brief       获取当前 OTA 状态
 */
ota_state_t ota_get_state(void);

/**
 * @brief       获取最近一次 OTA 错误码
 */
ota_err_t ota_get_error(void);

/**
 * @brief       获取错误描述字符串
 */
const char *ota_errstr(ota_err_t err);

/**
 * @brief       注册进度回调 (可选, 用于 LED/LCD 显示进度)
 */
void ota_set_progress_callback(ota_progress_cb_t cb);

/**
 * @brief       获取下载进度百分比 (0~100)
 */
uint8_t ota_get_progress(void);

#endif /* __OTA_DOWNLOAD_H */
