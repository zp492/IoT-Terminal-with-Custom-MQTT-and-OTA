/**
 ****************************************************************************************************
 * @file        main.c
 * @brief       FreeRTOS + W5500 以太网实验
 ****************************************************************************************************
 */

#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "led.h"
#include "key.h"
#include "./MALLOC/malloc.h"
#include "freertos_task.h"
#include "w5500_port.h"

int main(void)
{
    uint8_t w5500_ret;

    HAL_Init();                         /* 初始化HAL库 */
    sys_stm32_clock_init(RCC_PLL_MUL9); /* 设置时钟, 72MHz */
    delay_init(72);                     /* 延时初始化 */
    usart_init(115200);                 /* 串口初始化为115200 */
    led_init();                         /* 初始化LED */
    key_init();                         /* 初始化按键 */

    /* ---- W5500 初始化 ---- */
    w5500_ret = w5500_init();

    if (w5500_ret == 0)
    {
        printf("[W5500] init success, link up (Link Up)\r\n");
        {
            wiz_NetInfo ni;
            wizchip_getnetinfo(&ni);
            printf("[W5500] MAC=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
                   ni.mac[0],ni.mac[1],ni.mac[2],
                   ni.mac[3],ni.mac[4],ni.mac[5]);
            printf("[W5500] IP=%d.%d.%d.%d\r\n", ni.ip[0], ni.ip[1], ni.ip[2], ni.ip[3]);
            printf("[W5500] GW=%d.%d.%d.%d\r\n",
                   ni.gw[0],ni.gw[1],ni.gw[2],ni.gw[3]);
        }
    }
    else if (w5500_ret == 1)
    {
        printf("[W5500] chip OK, but no cable (Link Down)\r\n");
        printf("[W5500] please check network cable\r\n");
    }
    else
    {
        printf("[W5500] init failed! check hardware:\r\n");
        printf("        PB12(CS) PB13(SCK) PB14(MISO) PB15(MOSI) PD6(RST)\r\n");
        while (1);
    }

    freertos_demo(); /* 运行FreeRTOS例程 */
}
