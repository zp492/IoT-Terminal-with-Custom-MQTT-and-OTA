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
#include "tcp_client.h"

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
        printf("[W5500] IP: 192.168.1.200, Port: please check socket configuration\r\n");
    }
    else if (w5500_ret == 1)
    {
        printf("[W5500] chip init success, but network cable is not connected (Link Down)\r\n");
        printf("[W5500] please check if the network cable is inserted to the router/switch\r\n");
        /* 仍然可以进入FreeRTOS, 后续插上网线后W5500会自动Link Up */
    }
    else
    {
        printf("[W5500] init failed! please check hardware connection:\r\n");
        printf("        PB12(CS) / PB13(SCK) / PB14(MISO) / PB15(MOSI) / PD6(RST)\r\n");
        while (1)
            ; /* 初始化失败, 停止运行 */
    }

    /* ---- 配置 TCP 服务器地址 (可根据需要修改) ---- */
    {
        uint8_t server_ip[4] = {192, 168, 1, 4};  /* 目标服务器 IP (用户PC) */
        tcp_client_set_server(server_ip, 8080);    /* 端口 8080 */
    }

    freertos_demo(); /* 运行FreeRTOS例程 (内含TCP客户端任务) */
}
