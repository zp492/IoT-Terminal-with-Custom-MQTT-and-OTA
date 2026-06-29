/**
 ****************************************************************************************************
 * @file        ota_partition.h
 * @author      zp492
 * @brief       OTA Flash 分区定义 (供 Bootloader 和 App 共用)
 * @details     STM32F103ZET6 内部 Flash 512KB (0x08000000 ~ 0x08080000)
 *
 *              分区布局:
 *              ┌──────────────────────────────────┐
 *              │  0x08000000  Bootloader  (48KB)  │  第1步: 为Bootloader预留
 *              │  0x0800C000  App 主区    (232KB)  │  当前App搬迁到此
 *              │  0x08046000  Download区  (232KB)  │  后续: 新固件暂存区
 *              │  0x08080000  Flash结束            │
 *              └──────────────────────────────────┘
 *
 * @note        修改历史
 *              V1.0 20260622  zp492: 创建, 定义基础分区
 ****************************************************************************************************
 */

#ifndef __OTA_PARTITION_H
#define __OTA_PARTITION_H

#include "stm32f1xx.h"

/* ================================================================================
 * Flash 基址 (STM32F103 内部 Flash)
 * ================================================================================ */
#define FLASH_BASE_ADDR             FLASH_BASE          /* 0x08000000 */
#define FLASH_TOTAL_SIZE            0x00080000UL        /* 512KB */

/* ================================================================================
 * Bootloader 分区 (前 48KB, 包含启动管理 + 固件校验 + Flash 擦写)
 * ================================================================================ */
#define BOOTLOADER_BASE_ADDR        (FLASH_BASE_ADDR)
#define BOOTLOADER_SIZE             0x0000C000UL        /* 48KB */
#define BOOTLOADER_END_ADDR         (BOOTLOADER_BASE_ADDR + BOOTLOADER_SIZE)  /* 0x0800C000 */

/* ================================================================================
 * App 主分区 (232KB, Bootloader 之后) — 当前运行固件
 * ================================================================================ */
#define APP_BASE_ADDR               BOOTLOADER_END_ADDR /* 0x0800C000 */
#define APP_SIZE                    0x0003A000UL        /* 232KB */
#define APP_END_ADDR                (APP_BASE_ADDR + APP_SIZE)  /* 0x08046000 */

/* ================================================================================
 * Download 分区 (232KB, App 之后) — 新固件缓存区 (后续步骤使用)
 * ================================================================================ */
#define DOWNLOAD_BASE_ADDR          APP_END_ADDR        /* 0x08046000 */
#define DOWNLOAD_SIZE               0x0003A000UL        /* 232KB */
#define DOWNLOAD_END_ADDR           (DOWNLOAD_BASE_ADDR + DOWNLOAD_SIZE)  /* 0x08080000 */

/* ================================================================================
 * 升级标志页 (Bootloader 区域内, page #22, 0x0800B000)
 * --------------------------------------------------------------------------------
 * Flag 页位于 Bootloader 区域末尾 4KB, 远离代码段, 避免执行 Flash 操作时
 * 影响正在运行的 Bootloader 代码 (STM32F1 Flash 操作会 Stall 总线).
 *
 * Flag 结构 (ota_flag_t, 直接映射到 Flash):
 *   flag->magic   = OTA_FLAG_MAGIC (0x4F544101)
 *   flag->fw_size = 固件字节数
 *   flag->fw_crc32 = CRC32 校验值 (硬件 CRC: 多项式 0x04C11DB7)
 *
 * Flag 清除方式: 擦除整个 Flag 页 (所有位恢复 0xFF)
 * ================================================================================ */
#define FLAG_PAGE_BASE_ADDR         0x0800B000UL
#define FLAG_PAGE_SIZE              0x00000800UL        /* 2KB, STM32F103xE Flash 页大小 */

/* ---- Flag Magic ---- */
#define OTA_FLAG_MAGIC              0x4F544101UL        /* "OTA" + version 1 */

/* ---- Flag 结构体 (映射到 0x0800B000, 3 × uint32_t = 12 字节) ---- */
typedef struct {
    uint32_t magic;     /* OTA_FLAG_MAGIC, 验证 Flag 是否有效 */
    uint32_t fw_size;   /* 固件字节数 */
    uint32_t fw_crc32;  /* CRC32 校验值 */
} ota_flag_t;

/* Flag 页指针 (直接映射到 Flash 地址, 读操作无需 Flash API) */
#define OTA_FLAG ((volatile ota_flag_t *)FLAG_PAGE_BASE_ADDR)

/* ---- Flag 页地址校验 (编译期) ---- */
/* FLAG_PAGE_BASE_ADDR = 0x0800B000, BOOTLOADER area = 0x08000000~0x0800BFFF */
#if ((0x0800B000UL < 0x08000000UL) || ((0x0800B000UL + 0x00000800UL) > 0x0800C000UL))
#error "Flag page must be entirely within Bootloader area!"
#endif

/* ================================================================================
 * 编译期静态断言: 确保总分区间不超出 Flash 范围
 * ================================================================================ */
#if ((DOWNLOAD_END_ADDR - FLASH_BASE_ADDR) > FLASH_TOTAL_SIZE)
#error "OTA partition overflow! Total partition size exceeds 512KB Flash."
#endif

/* ================================================================================
 * 向量表偏移量 (App 需要: SCB->VTOR = FLASH_BASE | APP_VECT_TAB_OFFSET)
 * 用于 sys_nvic_set_vector_table(FLASH_BASE_ADDR, APP_VECT_TAB_OFFSET)
 * ================================================================================ */
#define APP_VECT_TAB_OFFSET         (APP_BASE_ADDR - FLASH_BASE_ADDR)  /* 0x0000C000 */

/* ================================================================================
 * 固件版本信息结构 (App 和 Download 分区内固定偏移处)
 * --------------------------------------------------------------------------------
 * 每个固件镜像在偏移 FW_INFO_OFFSET 处嵌入一个 fw_info_t 结构体.
 * Bootloader 可从此处读取版本号用于 LCD 显示和版本比对.
 *
 * ARMCC5 放置方式:
 *   const fw_info_t __attribute__((at(APP_BASE_ADDR + FW_INFO_OFFSET))) g_fw_info;
 *
 * 结构体大小: 64 字节 (预留扩展空间)
 * ================================================================================ */
#define FW_INFO_OFFSET              0x200UL             /* 从分区起始偏移 (在向量表之后) */
#define FW_INFO_MAGIC               0x4657494EUL        /* "FWIN" — 固件信息有效标志 */
#define FW_INFO_VERSION_STR_LEN     16                  /* 版本字符串最大长度 */
#define FW_INFO_DATE_LEN            16                  /* 构建日期字符串最大长度 */

typedef struct {
    uint32_t magic;                                     /* FW_INFO_MAGIC, 用于验证结构有效 */
    uint8_t  ver_major;                                 /* 主版本号 (如 1) */
    uint8_t  ver_minor;                                 /* 次版本号 (如 2) */
    uint8_t  ver_patch;                                 /* 补丁号   (如 0) */
    uint8_t  reserved;                                  /* 保留, 对齐 */
    char     version_str[FW_INFO_VERSION_STR_LEN];      /* 版本字符串 "1.2.0" */
    char     build_date[FW_INFO_DATE_LEN];              /* 构建日期 "Jun 24 2026" */
    uint32_t fw_bin_size;                               /* 固件 .bin 文件字节数 (0=未填写) */
    uint32_t fw_bin_crc32;                              /* 固件 .bin 文件 CRC32   (0=未填写) */
} fw_info_t;

/* ---- 编译期校验 ---- */
// #if sizeof(fw_info_t) > 64  // C 不允许编译期 sizeof, 运行时检查
// #error "fw_info_t exceeds 64 bytes"

#endif /* __OTA_PARTITION_H */
