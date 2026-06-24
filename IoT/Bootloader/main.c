/**
 ****************************************************************************************************
 * @file        main.c
 * @author      zp492
 * @brief       Bootloader 入口 — 裸机, 无 FreeRTOS
 * @note        上电从 Reset_Handler → SystemInit() → __main → main()
 *              调用 bootloader_run() (永不返回)
 *
 *              编译: Keil MDK 独立 Target "Bootloader"
 *              ROM: 0x08000000 Size: 0xC000 (48KB)
 *              RAM: 0x20000000 Size: 0x10000 (64KB)
 ****************************************************************************************************
 */

#include "bootloader.h"

/**
 * @brief       程序入口
 * @note        bootloader_run() 负责所有初始化, 永不返回
 */
int main(void)
{
    bootloader_run();

    /* Never reached */
    while (1);
}
