/**
 ****************************************************************************************************
 * @file        fw_info.c
 * @brief       固件版本信息 — 通过 ARMCC5 __at__ 放置在 Flash 固定偏移处
 * @note        位于 APP_BASE + FW_INFO_OFFSET (0x0800C200)
 *              Bootloader 读取此处获取版本号用于 LCD 显示
 *
 *              ARMCC5 语法: __attribute__((at(addr))) 将变量放在绝对地址
 *              GCC 语法:    __attribute__((section(".fw_info"))) + 链接脚本
 ****************************************************************************************************
 */

#include "ota_partition.h"
#include "fw_version.h"

/* ---- 构建版本字符串 "X.Y.Z" ---- */
#define FW_STR_(maj, min, pat)  #maj "." #min "." #pat
#define FW_STR(maj, min, pat)   FW_STR_(maj, min, pat)

/*
 * 固件信息块 — 放置在 APP_BASE + 0x200 = 0x0800C200
 *
 * ARMCC5 的 __attribute__((at())) 直接将变量放在指定绝对 Flash 地址.
 * 由于 App 编译时不知道 APP_BASE_ADDR (链接器宏), 这里使用硬编码的
 * Flash 绝对地址: FLASH_BASE + BOOTLOADER_SIZE + FW_INFO_OFFSET
 *             = 0x08000000 + 0xC000 + 0x200 = 0x0800C200
 *
 * 偏移计算: 0x08000000 + 0xC000 + 0x200 = 0x0800C200
 */
#define FW_INFO_FLASH_ADDR  (0x08000000UL + 0x0000C000UL + FW_INFO_OFFSET)

const fw_info_t g_firmware_info
    __attribute__((at(FW_INFO_FLASH_ADDR)))
    __attribute__((used)) = {
    .magic         = FW_INFO_MAGIC,
    .ver_major     = FW_VER_MAJOR,
    .ver_minor     = FW_VER_MINOR,
    .ver_patch     = FW_VER_PATCH,
    .reserved      = 0,
    .version_str   = FW_STR(FW_VER_MAJOR, FW_VER_MINOR, FW_VER_PATCH),
    .build_date    = __DATE__,
    .fw_bin_size   = 0,       /* 由构建后脚本填写, 或保持 0 */
    .fw_bin_crc32  = 0,       /* 由构建后脚本填写, 或保持 0 */
};
