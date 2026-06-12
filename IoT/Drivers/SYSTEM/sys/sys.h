/**
 ****************************************************************************************************
 * @file        sys.c
 * @author      朱杰
 * @version     V2.0
 * @date        2026-06-12
 * @brief       系统底层驱动代码(包括时钟配置/中断管理/低功耗/复位等)
 * @details     该文件提供了系统底层的初始化和管理函数, 包括:
 *              - 系统时钟配置 (HSE + PLL, 带HSI降级保护)
 *              - 中断向量表偏移设置 (用于IAP/Bootloader)
 *              - 低功耗模式 (WFI / 待机)
 *              - 系统软复位
 *              - 全局中断开关
 *              - 栈顶地址设置 (用于RTOS任务切换)
 *
 * @note        硬件平台: 正点原子 STM32F103ZET6 开发板
 *              - MCU:    STM32F103ZET6 (Cortex-M3, 72MHz主频, 512KB Flash, 64KB SRAM)
 *              - HSE:    8MHz 外部晶振 (定义于 stm32f1xx_hal_conf.h 中的 HSE_VALUE = 8000000U)
 *              - HSI:    8MHz 内部RC振荡器 (精度较低 ±1%, 用于HSE故障时的降级时钟源)
 *              - LSE:    32.768KHz 外部低速晶振 (用于RTC)
 *
 * @note        时钟树概览 (F103):
 *              HSE(8MHz) ──→ PLL(×9) ──→ PLLCLK(72MHz) ──→ SYSCLK ──→ HCLK(AHB总线,72MHz)
 *                                               │                        ├──→ PCLK1(APB1,≤36MHz)
 *                                               │                        └──→ PCLK2(APB2,≤72MHz)
 *                                               │
 *              降级路径:  HSI(8MHz) ──→ 直通 ──→ SYSCLK(8MHz)
 *
 * @note        修改历史
 *              V1.0 20211103  正点原子团队: 第一次发布, 原始版本
 *              V2.0 20260612  朱杰: 重写时钟初始化, 新增HSI降级保护/自适应分频/Flash等待周期
 ****************************************************************************************************
 */

#ifndef __SYS_H
#define __SYS_H

#include "stm32f1xx.h"

/**
 * SYS_SUPPORT_OS用于定义系统文件夹是否支持OS
 * 0,不支持OS
 * 1,支持OS
 */
#define SYS_SUPPORT_OS 1

/*函数申明*******************************************************************************************/

void sys_nvic_set_vector_table(uint32_t baseaddr, uint32_t offset); /* 设置中断偏移量 */
void sys_standby(void);                                             /* 进入待机模式 */
void sys_soft_reset(void);                                          /* 系统软复位 */
uint8_t sys_clock_set(uint32_t plln);                               /* 时钟设置函数 */
uint8_t sys_stm32_clock_init(uint32_t plln);                        /* 系统时钟初始化函数(返回0成功/1降级HSI/2失败) */

/* 以下为汇编函数 */
void sys_wfi_set(void);          /* 执行WFI指令 */
void sys_intx_disable(void);     /* 关闭所有中断 */
void sys_intx_enable(void);      /* 开启所有中断 */
void sys_msr_msp(uint32_t addr); /* 设置栈顶地址 */

#endif
