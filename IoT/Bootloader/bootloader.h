/**
 ****************************************************************************************************
 * @file        bootloader.h
 * @author      zp492
 * @brief       Bootloader 公共接口 — 状态枚举、常量、主流程入口
 * @note        裸机运行 (无 FreeRTOS), 上电后检查升级标志 → 校验 → 升级 → 跳转 App
 *
 *              架构:
 *              - bootloader.c     核心流程 (bootloader_run)
 *              - bootloader_flash  Flash 擦除/编程/验证
 *              - bootloader_crc   硬件 CRC32 封装
 *              - bootloader_led   非阻塞 LED 状态指示
 *              - bootloader_debug 串口日志输出
 *
 *              Flag 结构 (ota_flag_t 映射到 0x0800B000):
 *              flag->magic   = OTA_FLAG_MAGIC
 *              flag->fw_size = 固件字节数
 *              flag->fw_crc32 = CRC32
 ****************************************************************************************************
 */

#ifndef __BOOTLOADER_H
#define __BOOTLOADER_H

#include "stm32f1xx.h"
#include "ota_partition.h"

/* ================================================================================
 * Bootloader 版本
 * ================================================================================ */
#define BL_VERSION_MAJOR  1
#define BL_VERSION_MINOR  0

/* ================================================================================
 * Bootloader 错误码 (正值 = 状态, 负值 = 错误)
 * ================================================================================ */
typedef enum {
    BL_OK                    =  0,   /* 无升级, 已跳转 App */
    BL_UPGRADING             =  1,   /* 升级进行中 */
    BL_UPGRADE_DONE          =  2,   /* 升级成功, 即将跳转 */
    BL_ERR_FLASH_ERASE       = -1,   /* App 区擦除失败 */
    BL_ERR_FLASH_PROGRAM     = -2,   /* Flash 编程失败 */
    BL_ERR_FLASH_VERIFY      = -3,   /* 拷贝验证不匹配 */
    BL_ERR_CRC_MISMATCH      = -4,   /* 固件 CRC32 校验失败 */
    BL_ERR_INVALID_SIZE      = -5,   /* 固件大小非法 */
    BL_ERR_APP_INVALID       = -6,   /* App 区无有效栈指针/复位向量 */
    BL_ERR_NO_FLAG           = -7,   /* 无有效升级标志 (正常, 直接跳转) */
} bl_status_t;

/* ================================================================================
 * Bootloader 运行状态 (LED 指示灯对应)
 * ================================================================================ */
typedef enum {
    BL_STATE_INIT,              /* 硬件初始化中 */
    BL_STATE_CHECK_FLAG,        /* 检查升级标志 (慢闪) */
    BL_STATE_UPGRADING,         /* 升级进行中 (快闪) */
    BL_STATE_UPGRADE_DONE,      /* 升级完成 (常亮) */
    BL_STATE_ERROR,             /* 出错 (SOS) */
    BL_STATE_JUMP_TO_APP,       /* 即将跳转 App (灭) */
} bl_state_t;

/* ================================================================================
 * 公共变量
 * ================================================================================ */
extern volatile uint32_t g_bl_tick;     /* 1ms 滴答计数器 (SysTick_Handler 递增) */

/* ================================================================================
 * 公共 API
 * ================================================================================ */

/**
 * @brief       Bootloader 主流程入口 (从 main.c 调用)
 * @note        硬件初始化 → 检查 flag → 校验 → 升级 → 跳转
 *              永不返回 (要么跳转 App, 要么错误死循环)
 */
void bootloader_run(void);

/**
 * @brief       设置当前运行状态 (LED 模块读取)
 */
void bootloader_set_state(bl_state_t state);

/**
 * @brief       获取当前运行状态
 */
bl_state_t bootloader_get_state(void);

/**
 * @brief       获取错误码描述字符串
 */
const char *bootloader_errstr(bl_status_t err);

#endif /* __BOOTLOADER_H */
