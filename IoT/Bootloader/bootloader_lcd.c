/**
 ****************************************************************************************************
 * @file        bootloader_lcd.c
 * @author      zp492
 * @brief       Bootloader LCD 显示模块实现 (800x480 TFT, 正点原子 SSD1963 驱动)
 * @note        裸机运行, 使用 g_bl_tick 做超时, 不依赖 FreeRTOS
 ****************************************************************************************************
 */

#include "bootloader_lcd.h"
#include "bootloader.h"
#include "lcd.h"
#include "key.h"
#include <stdio.h>

/* ---- 屏幕布局 (800x480) ---- */
#define LCD_W           800
#define LCD_H           480
#define TITLE_Y         40
#define INFO_Y          120
#define PROGRESS_Y      280
#define PROGRESS_H      36
#define PROGRESS_X      80
#define PROGRESS_W      (LCD_W - 160)
#define HINT_Y          400
#define CENTER_X(x,w)   ((LCD_W - (w)) / 2 + (x))  /* 居中辅助 */

/* ---- 颜色方案 ---- */
#define C_BG            WHITE
#define C_TITLE         BLUE
#define C_INFO          BLACK
#define C_PROGRESS_BG   GRAY
#define C_PROGRESS_BAR  GREEN
#define C_ERROR         RED
#define C_WARN          YELLOW
#define C_HINT          GRAY

/* ---- 全局 tick (与 bootloader.c 共享) ---- */
extern volatile uint32_t g_bl_tick;

/* ================================================================================
 * 内部函数: 在屏幕中央区域绘制标题和清除信息区
 * ================================================================================ */

static void bl_lcd_draw_title(const char *title, uint16_t color)
{
    lcd_clear(C_BG);
    lcd_show_string(0, TITLE_Y, LCD_W, 32, 24, (char *)title, color);
}

static uint8_t bl_lcd_poll_keys(void)
{
    uint8_t k = key_scan(1);  /* mode=1: 连续扫描 */
    if (k == KEY0_PRES) return BL_KEY_CONFIRM;
    if (k == KEY1_PRES) return BL_KEY_CANCEL;
    if (k == WKUP_PRES) return BL_KEY_ANY;
    return BL_KEY_NONE;
}

/* ================================================================================
 * 公共 API
 * ================================================================================ */

void bl_lcd_init(void)
{
    lcd_init();
    lcd_clear(C_BG);
    lcd_display_on();
    key_init();
}

/* ---- 发现新固件 ---- */

void bl_lcd_show_new_firmware(const char *cur_ver, const char *new_ver)
{
    char buf[64];

    bl_lcd_draw_title("OTA Firmware Upgrade", BLUE);

    /* 当前版本 */
    lcd_show_string(60, INFO_Y, LCD_W - 120, 28, 24, (char *)"Current:", BLACK);
    snprintf(buf, sizeof(buf), "v%s", cur_ver);
    lcd_show_string(260, INFO_Y, 200, 28, 24, buf, GRAY);

    /* 新版本 */
    lcd_show_string(60, INFO_Y + 50, LCD_W - 120, 28, 24, (char *)"New:", BLACK);
    snprintf(buf, sizeof(buf), "v%s", new_ver);
    lcd_show_string(260, INFO_Y + 50, 200, 28, 24, buf, GREEN);

    /* 分隔线 */
    lcd_draw_hline(60, INFO_Y + 110, LCD_W - 120, GRAY);

    /* 操作提示 */
    lcd_show_string(0, HINT_Y - 20, LCD_W, 24, 16,
                    (char *)"KEY0 = Confirm Upgrade", BLUE);
    lcd_show_string(0, HINT_Y + 20, LCD_W, 24, 16,
                    (char *)"KEY1 = Skip (boot old firmware)", GRAY);
}

/* ---- 确认/取消 (阻塞等待) ---- */

bl_confirm_t bl_lcd_confirm_upgrade(uint32_t timeout_ms)
{
    uint32_t start = g_bl_tick;
    uint32_t elapsed;
    char buf[32];
    uint8_t key;

    while (1) {
        elapsed = g_bl_tick - start;

        /* 超时 → 自动升级 */
        if (elapsed >= timeout_ms) {
            return BL_CONFIRM_TIMEOUT;
        }

        /* 倒计时显示 */
        {
            uint32_t remain = (timeout_ms - elapsed) / 1000;
            if (remain < 60) {
                snprintf(buf, sizeof(buf), "Auto-upgrade in %lu s...", (unsigned long)remain);
                lcd_fill(0, HINT_Y - 50, LCD_W, HINT_Y - 30, C_BG);
                lcd_show_string(0, HINT_Y - 50, LCD_W, 24, 16, buf, C_HINT);
            }
        }

        /* 按键检测 */
        key = bl_lcd_poll_keys();
        if (key == BL_KEY_CONFIRM) {
            return BL_CONFIRM_UPGRADE;
        }
        if (key == BL_KEY_CANCEL) {
            return BL_CONFIRM_SKIP;
        }

        /* 每 50ms 检查一次 (g_bl_tick 由 SysTick 1ms 中断递增) */
        {
            uint32_t target = g_bl_tick + 50;
            while (g_bl_tick < target) {
                /* 忙等待, SysTick 中断驱动 */
            }
        }
    }
}

/* ---- 升级进度 ---- */

void bl_lcd_show_upgrading_start(void)
{
    bl_lcd_draw_title("Upgrading Firmware...", BLUE);

    lcd_show_string(0, INFO_Y, LCD_W, 28, 24,
                    (char *)"DO NOT POWER OFF!", RED);
    lcd_show_string(0, INFO_Y + 40, LCD_W, 24, 16,
                    (char *)"Erasing & flashing App area...", BLACK);

    /* 进度条边框 */
    lcd_draw_rectangle(PROGRESS_X - 2, PROGRESS_Y - 2,
                       PROGRESS_X + PROGRESS_W + 2, PROGRESS_Y + PROGRESS_H + 2,
                       GRAY);
}

void bl_lcd_update_progress(uint32_t page, uint32_t total)
{
    uint32_t pct;
    uint32_t bar_w;
    char buf[32];

    if (total == 0) return;

    pct = (page * 100) / total;
    if (pct > 100) pct = 100;

    bar_w = (PROGRESS_W * pct) / 100;

    /* 进度条填充 */
    lcd_fill(PROGRESS_X, PROGRESS_Y,
             PROGRESS_X + (uint16_t)bar_w, PROGRESS_Y + PROGRESS_H,
             (pct == 100) ? GREEN : BLUE);

    /* 未完成部分保持背景色 */
    if (bar_w < PROGRESS_W) {
        lcd_fill(PROGRESS_X + (uint16_t)bar_w, PROGRESS_Y,
                 PROGRESS_X + PROGRESS_W, PROGRESS_Y + PROGRESS_H,
                 C_BG);
    }

    /* 百分比文字 */
    snprintf(buf, sizeof(buf), "%lu%%  (%lu/%lu)", (unsigned long)pct,
             (unsigned long)page, (unsigned long)total);
    lcd_fill(PROGRESS_X, PROGRESS_Y + PROGRESS_H + 10,
             PROGRESS_X + PROGRESS_W, PROGRESS_Y + PROGRESS_H + 28, C_BG);
    lcd_show_string(PROGRESS_X, PROGRESS_Y + PROGRESS_H + 10,
                    PROGRESS_W, 24, 16, buf, BLACK);
}

/* ---- 结果 ---- */

void bl_lcd_show_upgrade_done(void)
{
    bl_lcd_draw_title("Upgrade Complete!", GREEN);

    lcd_show_string(0, INFO_Y, LCD_W, 28, 24,
                    (char *)"Firmware installed successfully.", BLACK);
    lcd_show_string(0, INFO_Y + 50, LCD_W, 24, 16,
                    (char *)"Rebooting to new firmware...", GRAY);

    lcd_show_string(0, HINT_Y, LCD_W, 24, 16,
                    (char *)"Please wait, device will restart.", C_HINT);
}

void bl_lcd_show_upgrade_error(int err_code)
{
    char buf[48];

    bl_lcd_draw_title("Upgrade Failed!", RED);

    lcd_show_string(0, INFO_Y, LCD_W, 28, 24,
                    (char *)"An error occurred during upgrade.", BLACK);

    snprintf(buf, sizeof(buf), "Error code: 0x%02X", err_code);
    lcd_show_string(0, INFO_Y + 50, LCD_W, 28, 24, buf, RED);

    lcd_show_string(0, INFO_Y + 110, LCD_W, 24, 16,
                    (char *)"The old firmware may be corrupted.", C_HINT);
    lcd_show_string(0, INFO_Y + 140, LCD_W, 24, 16,
                    (char *)"Bootloader will retry on next power-on.", C_HINT);

    lcd_show_string(0, HINT_Y, LCD_W, 24, 16,
                    (char *)"Press WKUP to reboot and retry...", BLACK);
}

void bl_lcd_show_upgrade_cancelled(void)
{
    bl_lcd_draw_title("Upgrade Cancelled", GRAY);

    lcd_show_string(0, INFO_Y, LCD_W, 28, 24,
                    (char *)"User cancelled the upgrade.", BLACK);
    lcd_show_string(0, INFO_Y + 50, LCD_W, 24, 16,
                    (char *)"Booting current firmware...", GRAY);
}

/* ================================================================================
 * 等待任意键 (错误页面使用)
 * ================================================================================ */

void bl_lcd_wait_any_key(void)
{
    while (1) {
        uint8_t k = bl_lcd_poll_keys();
        if (k != BL_KEY_NONE) {
            break;
        }
        /* 50ms 轮询 */
        {
            uint32_t target = g_bl_tick + 50;
            while (g_bl_tick < target) {}
        }
    }
}
