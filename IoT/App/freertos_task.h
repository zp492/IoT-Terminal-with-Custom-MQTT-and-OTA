/**
 ****************************************************************************************************
 * @file        freertos_task.h
 * @brief       FreeRTOS 任务声明 — 所有任务的入口函数在此声明
 * @note        任务实现位于 freertos_task.c
 ****************************************************************************************************
 */

#ifndef __FREERTOS_TASK_H
#define __FREERTOS_TASK_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "semphr.h"

/* ---- 网络状态 (mqtt_wrapper 更新, led_task 读取) ---- */
/* NET_STATE_* 宏定义见 mqtt_wrapper.h */
extern volatile uint8_t g_net_state;

/* ---- 传感器数据结构 ---- */
typedef struct {
    uint8_t temp;       /* 温度 (℃) */
    uint8_t humi;       /* 湿度 (%)  */
} sensor_data_t;

/* ---- 传感器消息队列 (sensor_task → mqtt_task) ---- */
#define SENSOR_QUEUE_LEN  3
extern QueueHandle_t g_sensor_queue;

/* ---- FreeRTOS 启动入口 ---- */
void freertos_demo(void);

/* ---- FreeRTOS 任务函数 ---- */
void start_task(void *pvParameters);
void sensor_task(void *pvParameters);
void mqtt_task(void *pvParameters);
void led_task(void *pvParameters);
void w5500_monitor_task(void *pvParameters);

/* ---- 网络就绪信号量 (monitor_task 给, mqtt_task 等) ---- */
extern SemaphoreHandle_t g_net_ready_sem;

#endif /* __FREERTOS_TASK_H */
