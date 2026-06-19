---
name: freertos-heap-semaphore-crash
description: FreeRTOS xSemaphoreTake configASSERT 崩溃 — 堆不足 + 栈溢出排查
metadata:
  type: project
---

## 现象

串口打印 `Error: ..\..\FreeRTOS\queue.c, 1666` + `Error: ..\..\FreeRTOS\queue.c, 1670`，程序卡死。

## 根因分析

`queue.c` 第 1666 / 1670 行在 `xQueueSemaphoreTake()` 中：

```c
configASSERT( pxQueue );                    // line 1666: 句柄为 NULL
configASSERT( pxQueue->uxItemSize == 0 );   // line 1670: 不是信号量 (item size ≠ 0)
```

**直接原因:** `xSemaphoreCreateBinary()` 返回 NULL → 后续 `xSemaphoreTake(NULL)` 触发断言。

**深层原因:**
1. FreeRTOS 堆 (`configTOTAL_HEAP_SIZE = 10KB`) 不够——任务栈 + 队列 + 信号量控制块超过堆容量
2. 传感器/MQTT 任务栈太小，栈溢出冲掉堆内信号量控制块 → `uxItemSize` 被破坏

## 修复

1. `configTOTAL_HEAP_SIZE`: 10KB → **20KB**
2. `SENSOR_STACK_SIZE`: 256 → **512 words**
3. `MQTT_STACK_SIZE`: 768 → **1024 words**
4. 所有内核对象（队列、信号量）必须在 `freertos_demo()` 中、`vTaskStartScheduler()` 之前创建，不能放在任务内部或临界区内

## 排查技巧

- `configASSERT` 在 FreeRTOSConfig.h 中定义为 `vAssertCalled(__FILE__, __LINE__)`
- 根据行号定位到 FreeRTOS 内核源码，确认是哪个函数、哪个参数出问题
- 信号量/队列创建失败 → 查 configTOTAL_HEAP_SIZE 和栈大小
- 跨任务使用的内核对象句柄 → 检查是否为 NULL、是否被覆盖

## 相关模块

- [[freertos-mqtt-architecture]] — 任务/队列/信号量架构
- [[w5500-port-network-config]] — W5500 网络配置
