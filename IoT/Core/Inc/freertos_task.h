/**
 ****************************************************************************************************
 * @file        freertos_task.h
 * @brief       FreeRTOS 任务声明 — 所有任务的入口函数在此声明
 * @note        任务实现位于 freertos_task.c
 ****************************************************************************************************
 */

#ifndef __FREERTOS_TASK_H
#define __FREERTOS_TASK_H

/* ---- FreeRTOS 启动入口 ---- */
void freertos_demo(void);

/* ---- FreeRTOS 任务函数 ---- */
void start_task(void *pvParameters);
void tcp_client_task(void *pvParameters);

#endif /* __FREERTOS_TASK_H */
