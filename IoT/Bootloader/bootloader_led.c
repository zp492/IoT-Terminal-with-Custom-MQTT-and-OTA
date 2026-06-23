/**
 ****************************************************************************************************
 * @file        bootloader_led.c
 * @brief       非阻塞 LED 状态机实现
 * @note        基于 g_bl_tick (SysTick 1ms) 驱动, 不阻塞 Bootloader 主流程
 *              LED0(0)=ON, LED0(1)=OFF (活低)
 *              SOS 时序: 点 200ms/空 200ms/划 600ms/空 200ms/字间 600ms/词间 2000ms
 ****************************************************************************************************
 */

#include "bootloader_led.h"
#include "led.h"

/* ---- SOS 模式时序常量 (ms) ---- */
#define SOS_DOT_ON      200
#define SOS_DOT_OFF     200
#define SOS_DASH_ON     600
#define SOS_DASH_OFF    200
#define SOS_LETTER_GAP  600     /* 额外 400ms 加在最后一个元素 off 后 */
#define SOS_WORD_GAP    2000    /* 循环间隔 */

/* ---- SOS 序列: 0=点, 1=划 ---- */
static const uint8_t sos_sequence[] = {0, 0, 0, 1, 1, 1, 0, 0, 0};  /* ... --- ... */
#define SOS_SEQ_LEN  9

/* ---- 内部状态 ---- */
static bl_state_t  g_led_state    = BL_STATE_INIT;
static int8_t      g_led_err_code = 0;
static uint32_t    g_led_last_tick;
static uint8_t     g_led_sos_idx;
static uint8_t     g_led_sos_on;          /* 当前 SOS 元素是否在 ON 阶段 */

/**
 * @brief       初始化 LED (复用 BSP led.c)
 */
void bl_led_init(void)
{
    led_init();                     /* GPIOB/GPIOE 时钟 + PB5/PE5 推挽输出 */
    LED0(1); LED1(1);               /* 初始全灭 */
    g_led_last_tick = g_bl_tick;
}

/**
 * @brief       设置 LED 状态
 */
void bl_led_set_state(bl_state_t state)
{
    g_led_state = state;
    g_led_sos_idx = 0;
    g_led_sos_on  = 1;              /* SOS 从 ON 阶段开始 */
    g_led_last_tick = g_bl_tick;
    LED0(1); LED1(1);               /* 切换状态时先灭 */
}

/**
 * @brief       设置错误码 (SOS 模式下记录)
 */
void bl_led_set_error(int8_t err_code)
{
    g_led_err_code = err_code;
}

/**
 * @brief       在 BL_STATE_UPGRADING 状态下显示升级进度
 * @param       progress: 0~100 百分比
 * @note        每 10% 额外快速闪烁一次作为心理反馈
 */
void bl_led_show_progress(uint8_t progress)
{
    (void)progress;
    /* 简化实现: 仅依赖原有的快闪模式 */
}

/**
 * @brief       LED 滴答更新 (在主循环或 SysTick ISR 中调用)
 * @note        非阻塞: 比较 g_bl_tick 决定是否翻转 GPIO
 */
void bl_led_update(void)
{
    uint32_t elapsed = g_bl_tick - g_led_last_tick;
    uint32_t on_time, off_time;

    switch (g_led_state) {

    case BL_STATE_CHECK_FLAG:
        /* 慢闪: 250ms ON, 250ms OFF */
        on_time  = 250;
        off_time = 250;
        goto do_blink;

    case BL_STATE_UPGRADING:
        /* 快闪: 100ms ON, 100ms OFF */
        on_time  = 100;
        off_time = 100;
        goto do_blink;

    do_blink:
        if (elapsed >= on_time) {
            LED0(0);  /* ON */
            if (elapsed >= (on_time + off_time)) {
                LED0(1);  /* OFF */
                g_led_last_tick = g_bl_tick;
            }
        }
        LED1(1);  /* LED1 常灭 */
        break;

    case BL_STATE_UPGRADE_DONE:
        /* 常亮 */
        LED0(0);
        LED1(1);
        break;

    case BL_STATE_JUMP_TO_APP:
        /* 全灭 */
        LED0(1);
        LED1(1);
        break;

    case BL_STATE_ERROR:
        /* SOS: ... --- ... 使用 LED1 闪烁, LED0 显示错误码 */
        {
            uint8_t  element = sos_sequence[g_led_sos_idx];
            uint32_t element_on  = element ? SOS_DASH_ON : SOS_DOT_ON;

            if (g_led_sos_on) {
                LED1(0);  /* ON */
                if (elapsed >= element_on) {
                    LED1(1);  /* OFF */
                    g_led_sos_on = 0;
                    g_led_last_tick = g_bl_tick;
                }
            } else {
                /* OFF 阶段: 判断是元素间间隔还是字母间/词间间隔 */
                uint32_t gap;
                gap = SOS_DOT_OFF;
                if (g_led_sos_idx == 2 || g_led_sos_idx == 5) {
                    gap = SOS_LETTER_GAP;   /* S→O 或 O→S 之间 */
                } else if (g_led_sos_idx == 8) {
                    gap = SOS_WORD_GAP;     /* 循环间隔 */
                }
                if (elapsed >= gap) {
                    g_led_sos_idx = (g_led_sos_idx + 1) % SOS_SEQ_LEN;
                    g_led_sos_on  = 1;
                    g_led_last_tick = g_bl_tick;
                }
            }
        }
        /* LED0: 用错误码闪烁 (1=快闪, 其他=慢闪) */
        {
            uint32_t err_period = (g_led_err_code == -4) ? 200 : 500;
            if (elapsed >= err_period) {
                LED0_TOGGLE();
                g_led_last_tick = g_bl_tick;
            }
        }
        break;

    default:
        /* BL_STATE_INIT: 全亮表示初始化 */
        LED0(0); LED1(0);
        break;
    }
}
