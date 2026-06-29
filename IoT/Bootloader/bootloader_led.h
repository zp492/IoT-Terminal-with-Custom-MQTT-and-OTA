/**
 ****************************************************************************************************
 * @file        bootloader_led.h
 * @author      zp492
 * @brief       非阻塞 LED 状态指示模块
 * @note        LED0 = PB5 (活低), 用 SysTick 滴答驱动状态机
 *
 *              LED 模式:
 *              CHECK_FLAG   → 慢闪 (250ms 周期)
 *              UPGRADING    → 快闪 (100ms 周期)
 *              UPGRADE_DONE → 常亮
 *              ERROR        → SOS (... --- ...)
 *              JUMP_TO_APP  → 灭
 ****************************************************************************************************
 */

#ifndef __BOOTLOADER_LED_H
#define __BOOTLOADER_LED_H

#include "bootloader.h"

void bl_led_init(void);
void bl_led_update(void);
void bl_led_set_state(bl_state_t state);
void bl_led_set_error(int8_t err_code);

#endif /* __BOOTLOADER_LED_H */
