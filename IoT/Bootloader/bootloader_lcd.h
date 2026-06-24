/**
 ****************************************************************************************************
 * @file        bootloader_lcd.h
 * @brief       Bootloader LCD 显示模块 — 固件检测 / 确认升级 / 进度 / 结果
 * @note        依赖 BSP lcd.c + key.c (FSMC 800x480 TFT)
 *              KEY0 = 确认, KEY1 = 取消, WKUP = 任意键返回
 ****************************************************************************************************
 */

#ifndef __BOOTLOADER_LCD_H
#define __BOOTLOADER_LCD_H

#include <stdint.h>

/* ================================================================================
 * 按键返回值
 * ================================================================================ */
#define BL_KEY_NONE     0
#define BL_KEY_CONFIRM  1   /* KEY0 — 确认升级 */
#define BL_KEY_CANCEL   2   /* KEY1 — 取消升级 */
#define BL_KEY_ANY      3   /* WKUP  — 任意键 */

/* ================================================================================
 * 确认升级结果
 * ================================================================================ */
typedef enum {
    BL_CONFIRM_UPGRADE,     /* 用户确认升级 */
    BL_CONFIRM_SKIP,        /* 用户取消升级 */
    BL_CONFIRM_TIMEOUT,     /* 超时自动升级 */
} bl_confirm_t;

/* ================================================================================
 * 公共 API
 * ================================================================================ */

void bl_lcd_init(void);

/* ---- 发现新固件 ---- */
void bl_lcd_show_new_firmware(const char *cur_ver, const char *new_ver);

/* ---- 确认/取消 (阻塞等待按键或超时) ---- */
bl_confirm_t bl_lcd_confirm_upgrade(uint32_t timeout_ms);

/* ---- 升级进度 ---- */
void bl_lcd_show_upgrading_start(void);
void bl_lcd_update_progress(uint32_t page, uint32_t total);

/* ---- 结果 ---- */
void bl_lcd_show_upgrade_done(void);
void bl_lcd_show_upgrade_error(int err_code);
void bl_lcd_show_upgrade_cancelled(void);

/* ---- 等待按键 (阻塞) ---- */
void bl_lcd_wait_any_key(void);

#endif /* __BOOTLOADER_LCD_H */
