#include "freertos_task.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "led.h"
#include "key.h"
#include "./MALLOC/malloc.h"
#include "tcp_client.h"
/*FreeRTOS*********************************************************************************************/
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/*FreeRTOS配置*/

/* START_TASK 任务 配置
 * 包括: 堆栈大小 任务优先级 创建任务 任务句柄
 */
#define START_STACK_SIZE 128
#define START_TSAK_PROI 1
void start_task(void *pvParameters);
TaskHandle_t start_task_handler;

/* TCP_CLIENT 任务 配置
 * 堆栈 512 words (2KB) — TCP socket 收发需要较大栈空间
 */
#define TCP_CLIENT_STACK_SIZE 512
#define TCP_CLIENT_TSAK_PROI  2
TaskHandle_t tcp_client_task_handler;

/**
 * @brief       FreeRTOS例程入口函数
 * @param       无
 * @retval      无
 */
void freertos_demo(void)
{
    xTaskCreate(start_task,
                "start_task",
                START_STACK_SIZE,
                NULL,
                START_TSAK_PROI,
                &start_task_handler);

    vTaskStartScheduler(); /*开启任务调度器*/
}

void start_task(void *pvParameters)
{
    taskENTER_CRITICAL(); // 进入临界区
    xTaskCreate(tcp_client_task,
                "tcp_client",
                TCP_CLIENT_STACK_SIZE,
                NULL,
                TCP_CLIENT_TSAK_PROI,
                &tcp_client_task_handler);
    vTaskDelete(NULL);
    taskEXIT_CRITICAL(); // 退出临界区
}

/**
 * @brief       TCP 客户端任务 (FreeRTOS 任务入口)
 * @note        薄包装, 实际逻辑在 tcp_client_run() 中
 */
void tcp_client_task(void *pvParameters)
{
    tcp_client_run(pvParameters);
}
