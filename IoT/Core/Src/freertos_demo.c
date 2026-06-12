#include "freertos_demo.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "led.h"
#include "key.h"
#include "./MALLOC/malloc.h"
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

/* TASK1 任务 配置
 * 包括: 堆栈大小 任务优先级 创建任务 任务句柄
 */
#define TASK1_STACK_SIZE 128
#define TASK1_TSAK_PROI 2
void task1(void *pvParameters);
TaskHandle_t task1_handler;

///* TASK2 任务 配置
// * 包括: 堆栈大小 任务优先级 创建任务 任务句柄
// */
// #define TASK2_STACK_SIZE 128
// #define TASK2_TSAK_PROI 3
// void task2(void *pvParameters);
// TaskHandle_t task2_handler;

///* TASK3 任务 配置
// * 包括: 堆栈大小 任务优先级 创建任务 任务句柄
// */
// #define TASK3_STACK_SIZE 128
// #define TASK3_TSAK_PROI 4
// void task3(void *pvParameters);
// TaskHandle_t task3_handler;

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
    xTaskCreate(task1,
                "task1",
                TASK1_STACK_SIZE,
                NULL,
                TASK1_TSAK_PROI,
                &task1_handler);
    vTaskDelete(NULL);
    taskEXIT_CRITICAL(); // 退出临界区
}

void task1(void *pvParameters)
{
 
  
}
