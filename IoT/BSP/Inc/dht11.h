/**
 ****************************************************************************************************
 * @file        dht11.h
 * @brief       DHT11 数字温湿度传感器驱动 (优化版)
 * @author      zpzp492基于正点原子 V1.0 重写
 * @note        优化点:
 *              1. 修正校验失败不返回错误的 bug
 *              2. 魔法数字替换为具名常量
 *              3. 增加读取失败自动重试 (最多3次)
 *              4. 关键时序段关闭中断, 防止 FreeRTOS 任务切换破坏 DHT11 协议时序
 *              5. 轮询等待改为计数器 + delay_us 混合, 减少函数调用开销
 * @attention   DHT11 单次读取耗时 ~25ms, 采样间隔建议 ≥1s
 ****************************************************************************************************
 */

#ifndef __DHT11_H
#define __DHT11_H

#include "./SYSTEM/sys/sys.h"

/******************************************************************************************/
/* DHT11 引脚 定义 */

#define DHT11_DQ_GPIO_PORT GPIOG
#define DHT11_DQ_GPIO_PIN GPIO_PIN_11
#define DHT11_DQ_GPIO_CLK_ENABLE()    \
    do                                \
    {                                 \
        __HAL_RCC_GPIOG_CLK_ENABLE(); \
    } while (0) /* PG口时钟使能 */

/******************************************************************************************/

/* IO操作函数 */
#define DHT11_DQ_OUT(x)                                                                                                                                        \
    do                                                                                                                                                         \
    {                                                                                                                                                          \
        x ? HAL_GPIO_WritePin(DHT11_DQ_GPIO_PORT, DHT11_DQ_GPIO_PIN, GPIO_PIN_SET) : HAL_GPIO_WritePin(DHT11_DQ_GPIO_PORT, DHT11_DQ_GPIO_PIN, GPIO_PIN_RESET); \
    } while (0) /* 数据端口输出 */
#define DHT11_DQ_IN HAL_GPIO_ReadPin(DHT11_DQ_GPIO_PORT, DHT11_DQ_GPIO_PIN) /* 数据端口输入 */

uint8_t dht11_init(void);                              /* 初始化DHT11 */
uint8_t dht11_check(void);                             /* 检测是否存在DHT11 */
uint8_t dht11_read_data(uint8_t *temp, uint8_t *humi); /* 读取温湿度 */

#endif
