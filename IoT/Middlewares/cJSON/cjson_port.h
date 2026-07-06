#ifndef __CJSON_PORT_H
#define __CJSON_PORT_H

#include "FreeRTOS.h"

// 将 cJSON 的内存分配映射到 FreeRTOS
#define cJSON_malloc(size) pvPortMalloc(size)
#define cJSON_free(ptr) vPortFree(ptr)

// 然后包含真正的 cJSON 头文件
#include "cJSON.h"

#endif
