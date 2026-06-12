/**
 ****************************************************************************************************
 * @file        main.c
 * @brief       FreeRTOS + W5500 以太网实验
 * @note        启动流程:
 *              HAL_Init → 时钟72MHz → 延时 → 串口 → LED → 按键 → W5500 → FreeRTOS
 *              其中W5500初始化包含: SPI2 → 注册回调 → 硬件复位 → 芯片初始化 →
 *              网络配置 → PHY自动协商 → SPI调速 → 等待Link Up
 ****************************************************************************************************
 */

#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "led.h"
#include "key.h"
#include "./MALLOC/malloc.h"
#include "freertos_demo.h"
#include "w5500_port.h"

int main(void)
{
    uint8_t w5500_ret;

    HAL_Init();                             /* 初始化HAL库 */
    sys_stm32_clock_init(RCC_PLL_MUL9);     /* 设置时钟, 72MHz */
    delay_init(72);                         /* 延时初始化 */
    usart_init(115200);                     /* 串口初始化为115200 */
    led_init();                             /* 初始化LED */
    key_init();                             /* 初始化按键 */

    /* ---- W5500 初始化 ---- */
    w5500_ret = w5500_init();

    if (w5500_ret == 0)
    {
        printf("[W5500] 初始化成功, 网线已连接 (Link Up)\r\n");
        printf("[W5500] IP: 192.168.1.100, Port: 请看socket配置\r\n");
    }
    else if (w5500_ret == 1)
    {
        printf("[W5500] 芯片初始化成功, 但网线未连接 (Link Down)\r\n");
        printf("[W5500] 请检查网线是否插入到路由器/交换机\r\n");
        /* 仍然可以进入FreeRTOS, 后续插上网线后W5500会自动Link Up */
    }
    else
    {
        printf("[W5500] 初始化失败! 请检查硬件连接:\r\n");
        printf("        PB12(CS) / PB13(SCK) / PB14(MISO) / PB15(MOSI) / PD6(RST)\r\n");
        while (1);  /* 初始化失败, 停止运行 */
    }

    freertos_demo(); /* 运行FreeRTOS例程 */
}
