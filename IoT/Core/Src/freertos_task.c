#include "freertos_task.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "led.h"
#include "key.h"
#include "./MALLOC/malloc.h"
#include "tcp_client.h"
#include "dht11.h"
#include "adc.h"
#include "lcd.h"
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

/* SENSOR 任务 配置
 * 堆栈 256 words (1KB) — LCD 字符绘制 + DHT11/ADC 读取
 */
#define SENSOR_STACK_SIZE 256
#define SENSOR_TSAK_PROI  3
TaskHandle_t sensor_task_handler;

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
    xTaskCreate(sensor_task,
                "sensor",
                SENSOR_STACK_SIZE,
                NULL,
                SENSOR_TSAK_PROI,
                &sensor_task_handler);
    vTaskDelete(NULL);
    taskEXIT_CRITICAL(); // 退出临界区
}

/**
 * @brief       传感器采集 + LCD 显示任务
 * @note        每秒读取 DHT11 温湿度 + ADC 电压, 刷新 LCD
 */
void sensor_task(void *pvParameters)
{
    uint8_t temp, humi;
    uint16_t adc_raw, adc_mv;
    char buf[32];

    (void)pvParameters;

    /* ---- 初始化传感器外设 ---- */
    lcd_init();                             /* LCD 屏初始化 (FSMC) */
    lcd_clear(WHITE);                       /* 清屏白色背景 */
    lcd_show_string(10, 10, 240, 24, 24, "Sensor Init...", BLUE);

    printf("[SENSOR] DHT11 init...\r\n");
    if (dht11_init() == 0)
    {
        printf("[SENSOR] DHT11 OK\r\n");
    }
    else
    {
        printf("[SENSOR] DHT11 not found! (check PG11)\r\n");
    }

    printf("[SENSOR] ADC3 CH6 init (PF8)...\r\n");
    adc_init();
    printf("[SENSOR] ADC3 DMA2_Ch4 started\r\n");

    /* 等待传感器稳定 */
    vTaskDelay(pdMS_TO_TICKS(2000));

    lcd_clear(WHITE);

    /* ---- 主循环: 每秒刷新 ---- */
    while (1)
    {
        /* 读取 DHT11 (失败则显示 --) */
        if (dht11_read_data(&temp, &humi) == 0)
        {
            snprintf(buf, sizeof(buf), "Temp: %d C    ", temp);
            lcd_show_string(10, 30, 240, 24, 24, buf, RED);

            snprintf(buf, sizeof(buf), "Humi: %d %%    ", humi);
            lcd_show_string(10, 70, 240, 24, 24, buf, BLUE);
        }
        else
        {
            lcd_show_string(10, 30, 240, 24, 24, "Temp: -- C    ", RED);
            lcd_show_string(10, 70, 240, 24, 24, "Humi: -- %    ", BLUE);
        }

        /* 读取 ADC */
        adc_raw = adc_get_value();
        adc_mv  = adc_get_mv();

        snprintf(buf, sizeof(buf), "ADC3CH6 Raw : %4d   ", adc_raw);
        lcd_show_string(10, 120, 240, 24, 24, buf, BLACK);

        snprintf(buf, sizeof(buf), "ADC3CH6 Volt: %4dmV", adc_mv);
        lcd_show_string(10, 160, 240, 24, 24, buf, BLACK);

        /* 串口同步输出 */
        printf("[SENSOR] Temp=%dC  Humi=%d%%  ADC=%d(%dmV)\r\n",
               temp, humi, adc_raw, adc_mv);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief       TCP 客户端任务 (FreeRTOS 任务入口)
 * @note        薄包装, 实际逻辑在 tcp_client_run() 中
 */
void tcp_client_task(void *pvParameters)
{
    tcp_client_run(pvParameters);
}
