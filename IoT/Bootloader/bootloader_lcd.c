/**
 ****************************************************************************************************
 * @file        bootloader_lcd.c
 * @author      zp492
 * @brief       Bootloader LCD 显示模块实现 (800x480 TFT, 正点原子 ILI9341 驱动)
 * @note        裸机运行, 使用 g_bl_tick 做超时, 不依赖 FreeRTOS
 ****************************************************************************************************
 */

#include "bootloader_lcd.h"
#include "bootloader.h"
#include "lcd.h"
#include "key.h"
#include <stdio.h>

/* ---- 屏幕布局 (2.8寸 320x240) ---- */
#define LCD_W 320
#define LCD_H 240
#define TITLE_Y 10
#define INFO_Y 55
#define PROGRESS_Y 130
#define PROGRESS_H 20
#define PROGRESS_X 20
#define PROGRESS_W (LCD_W - 40)
#define HINT_Y 200
#define CENTER_X(x, w) ((LCD_W - (w)) / 2 + (x)) /* 居中辅助 */

/* ---- 颜色方案 ---- */
#define C_BG WHITE
#define C_TITLE BLUE
#define C_INFO BLACK
#define C_PROGRESS_BG MAGENTA
#define C_PROGRESS_BAR GREEN
#define C_ERROR RED
#define C_WARN GREEN
#define C_HINT MAGENTA

/* ---- 全局 tick (与 bootloader.c 共享) ---- */
extern volatile uint32_t g_bl_tick;

/* ================================================================================
 * 内部函数: 在屏幕中央区域绘制标题和清除信息区
 * ================================================================================ */

static void bl_lcd_draw_title(const char *title, uint16_t color)
{
    lcd_clear(C_BG);
    lcd_show_string(0, TITLE_Y, LCD_W, 24, 16, (char *)title, color);
}

static uint8_t bl_lcd_poll_keys(void)
{
    uint8_t k = key_scan(1); /* mode=1: 连续扫描 */
    if (k == KEY0_PRES)
        return BL_KEY_CONFIRM;
    if (k == KEY1_PRES)
        return BL_KEY_CANCEL;
    if (k == WKUP_PRES)
        return BL_KEY_ANY;
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
    const char *cur_disp, *new_disp;

    /* 版本号为空或 "?" 时显示 "Unknown" */
    cur_disp = (cur_ver && cur_ver[0] && cur_ver[0] != '?') ? cur_ver : "Unknown";
    new_disp = (new_ver && new_ver[0] && new_ver[0] != '?') ? new_ver : "Unknown";

    /* 串口诊断 */
    printf("[BL] LCD show: cur=%s new=%s\r\n", cur_disp, new_disp);

    bl_lcd_draw_title("OTA Firmware Upgrade", BLUE);

    /* 当前版本 */
    lcd_show_string(10, INFO_Y, 80, 24, 16, (char *)"Cur:", BLACK);
    lcd_show_string(90, INFO_Y, 220, 24, 16, (char *)cur_disp, BLUE);

    /* 新版本 */
    lcd_show_string(10, INFO_Y + 30, 80, 24, 16, (char *)"New:", BLACK);
    lcd_show_string(90, INFO_Y + 30, 220, 24, 16, (char *)new_disp, BLUE);

    /* 分隔线 */
    lcd_draw_hline(10, INFO_Y + 65, LCD_W - 20, CYAN);

    /* 操作提示 */
    lcd_show_string(0, HINT_Y - 10, LCD_W, 20, 12,
                    (char *)"KEY0=Upgrade  KEY1=Skip", BLACK);
}

/* ---- 确认/取消 (阻塞等待) ---- */

bl_confirm_t bl_lcd_confirm_upgrade(uint32_t timeout_ms)
{
    uint32_t start = g_bl_tick;
    uint32_t elapsed;
    char buf[32];
    uint8_t key;

    while (1)
    {
        elapsed = g_bl_tick - start;

        /* 超时 → 自动升级 */
        if (elapsed >= timeout_ms)
        {
            return BL_CONFIRM_TIMEOUT;
        }

        /* 倒计时显示 */
        {
            uint32_t remain = (timeout_ms - elapsed) / 1000;
            if (remain < 60)
            {
                snprintf(buf, sizeof(buf), "Auto in %lu s...", (unsigned long)remain);
                lcd_fill(0, PROGRESS_Y - 20, LCD_W, PROGRESS_Y, C_BG);
                lcd_show_string(0, PROGRESS_Y - 20, LCD_W, 20, 12, buf, C_HINT);
            }
        }

        /* 按键检测 */
        key = bl_lcd_poll_keys();
        if (key == BL_KEY_CONFIRM)
        {
            return BL_CONFIRM_UPGRADE;
        }
        if (key == BL_KEY_CANCEL)
        {
            return BL_CONFIRM_SKIP;
        }

        /* 每 50ms 检查一次 (g_bl_tick 由 SysTick 1ms 中断递增) */
        {
            uint32_t target = g_bl_tick + 50;
            while (g_bl_tick < target)
            {
                /* 忙等待, SysTick 中断驱动 */
            }
        }
    }
}

/* ---- 升级进度 ---- */

void bl_lcd_show_upgrading_start(void)
{
    bl_lcd_draw_title("Upgrading Firmware...", BLUE);

    lcd_show_string(0, INFO_Y, LCD_W, 24, 16,
                    (char *)"DO NOT POWER OFF!", RED);
    lcd_show_string(0, INFO_Y + 25, LCD_W, 20, 12,
                    (char *)"Erasing & flashing...", BLACK);

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

    if (total == 0)
        return;

    pct = (page * 100) / total;
    if (pct > 100)
        pct = 100;

    bar_w = (PROGRESS_W * pct) / 100;

    /* 进度条填充 */
    lcd_fill(PROGRESS_X, PROGRESS_Y,
             PROGRESS_X + (uint16_t)bar_w, PROGRESS_Y + PROGRESS_H,
             (pct == 100) ? GREEN : BLUE);

    /* 未完成部分保持背景色 */
    if (bar_w < PROGRESS_W)
    {
        lcd_fill(PROGRESS_X + (uint16_t)bar_w, PROGRESS_Y,
                 PROGRESS_X + PROGRESS_W, PROGRESS_Y + PROGRESS_H,
                 C_BG);
    }

    /* 百分比文字 */
    snprintf(buf, sizeof(buf), "%lu%% (%lu/%lu)", (unsigned long)pct,
             (unsigned long)page, (unsigned long)total);
    lcd_fill(PROGRESS_X, PROGRESS_Y + PROGRESS_H + 2,
             PROGRESS_X + PROGRESS_W, PROGRESS_Y + PROGRESS_H + 20, C_BG);
    lcd_show_string(PROGRESS_X, PROGRESS_Y + PROGRESS_H + 2,
                    PROGRESS_W, 20, 12, buf, BLACK);
}

/* ---- 结果 ---- */

void bl_lcd_show_upgrade_done(void)
{
    bl_lcd_draw_title("Upgrade Complete!", GREEN);

    lcd_show_string(0, INFO_Y, LCD_W, 24, 16,
                    (char *)"Installed successfully.", BLACK);
    lcd_show_string(0, INFO_Y + 30, LCD_W, 20, 12,
                    (char *)"Rebooting...", GRAY);
    lcd_show_string(0, HINT_Y, LCD_W, 20, 12,
                    (char *)"Wait, device will restart.", C_HINT);
}

void bl_lcd_show_upgrade_error(int err_code)
{
    char buf[48];

    bl_lcd_draw_title("Upgrade Failed!", RED);

    lcd_show_string(0, INFO_Y, LCD_W, 24, 16,
                    (char *)"Error during upgrade.", BLACK);

    snprintf(buf, sizeof(buf), "Code: 0x%02X", err_code);
    lcd_show_string(0, INFO_Y + 30, LCD_W, 24, 16, buf, RED);

    lcd_show_string(0, INFO_Y + 65, LCD_W, 20, 12,
                    (char *)"Old firmware may be corrupted.", C_HINT);

    lcd_show_string(0, HINT_Y, LCD_W, 20, 12,
                    (char *)"Press WKUP to retry...", BLACK);
}

void bl_lcd_show_upgrade_cancelled(void)
{
    bl_lcd_draw_title("Upgrade Cancelled", GRAY);

    lcd_show_string(0, INFO_Y, LCD_W, 24, 16,
                    (char *)"User cancelled.", BLACK);
    lcd_show_string(0, INFO_Y + 30, LCD_W, 20, 12,
                    (char *)"Booting current firmware...", GRAY);
}

/* ================================================================================
 * 等待任意键 (错误页面使用)
 * ================================================================================ */

void bl_lcd_wait_any_key(void)
{
    while (1)
    {
        uint8_t k = bl_lcd_poll_keys();
        if (k != BL_KEY_NONE)
        {
            break;
        }
        /* 50ms 轮询 */
        {
            uint32_t target = g_bl_tick + 50;
            while (g_bl_tick < target)
            {
            }
        }
    }
}
