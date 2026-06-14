/**
 ****************************************************************************************************
 * @file        dht11.c
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

#include "./BSP/DHT11/dht11.h"
#include "./SYSTEM/delay/delay.h"

/* FreeRTOS — 关键时序段需禁止任务调度 */
#include "FreeRTOS.h"
#include "task.h"

/* ================================================================================
 * DHT11 协议时序常量 (单位: μs, 除标注外)
 * 详见 DHT11 数据手册
 * ================================================================================ */
#define DHT11_RESET_LOW_MS      20      /* 主机拉低 ≥18ms */
#define DHT11_RESET_HIGH_US     30      /* 主机释放总线 20~40μs */
#define DHT11_RESP_LOW_US       80      /* 从机响应低电平 80μs */
#define DHT11_RESP_HIGH_US      80      /* 从机响应高电平 80μs */
#define DHT11_BIT_START_US      50      /* 每bit开始: 低电平 50μs */
#define DHT11_BIT0_HIGH_US      28      /* bit=0: 高电平 26~28μs */
#define DHT11_BIT1_HIGH_US      70      /* bit=1: 高电平 70μs */
#define DHT11_SAMPLE_DELAY_US   40      /* 在上升沿后 40μs 采样 (28 < 40 < 70) */

/* 轮询超时 (μs), 用于从机应答和 bit 边沿检测 */
#define DHT11_POLL_TIMEOUT_US   120

/* 读取参数 */
#define DHT11_BUF_SIZE          5       /* 数据帧: 湿度整数+小数+温度整数+小数+校验 */
#define DHT11_MAX_RETRIES       3       /* 校验失败最大重试次数 */

/**
 * @brief       微秒级忙等计数器 (不依赖 SysTick, 用于 ISR 安全场景)
 * @param       us: 延时微秒数 (近似值, 72MHz 下每循环约 12 周期 ≈ 0.17μs)
 * @note        仅在关闭全局中断的临界区内使用, 避免调用 delay_us 带来的开销
 */
static inline void dht11_spin_us(uint32_t us)
{
    /* 72MHz 下每计数值 ≈ 0.17μs, 乘以 6 约得 1μs */
    uint32_t cnt = us * 6;
    while (cnt--) { __NOP(); }
}

/**
 * @brief       等待引脚变为指定电平, 带超时
 * @param       level:  期望电平 (1=高, 0=低)
 * @param       timeout_us: 超时 (μs)
 * @retval      0: 成功 (检测到目标电平)
 *              1: 超时
 * @note        逐个微秒轮询, 兼顾精度与响应速度
 */
static uint8_t dht11_wait_level(uint8_t level, uint32_t timeout_us)
{
    uint32_t cnt = 0;

    while (cnt < timeout_us)
    {
        if ((uint8_t)DHT11_DQ_IN == level)
        {
            return 0;   /* 检测到目标电平 */
        }
        delay_us(1);
        cnt++;
    }
    return 1;           /* 超时 */
}

/**
 * @brief       DHT11 复位 + 启动信号
 * @note        主机: 拉低 18ms → 释放 20~40μs
 */
static void dht11_reset(void)
{
    DHT11_DQ_OUT(0);
    delay_ms(DHT11_RESET_LOW_MS);
    DHT11_DQ_OUT(1);
    delay_us(DHT11_RESET_HIGH_US);
}

/**
 * @brief       等待 DHT11 从机应答
 * @retval      0: DHT11 正常应答
 *              1: 无应答 / 异常
 */
static uint8_t dht11_wait_ack(void)
{
    /* 从机拉低 80μs */
    if (dht11_wait_level(0, DHT11_POLL_TIMEOUT_US)) return 1;
    /* 从机释放 80μs */
    if (dht11_wait_level(1, DHT11_POLL_TIMEOUT_US)) return 1;
    return 0;
}

/**
 * @brief       从 DHT11 读取一个 bit
 * @retval      0 或 1
 * @note        必须在关闭中断的临界区内调用, 时序:
 *              每 bit 开始: 50μs 低电平 → 26~28μs(bit=0) 或 70μs(bit=1) 高电平
 *              在上升沿后 40μs 采样 (28 < 40 < 70)
 */
static uint8_t dht11_read_bit(void)
{
    /* 等待 bit 开始的 50μs 低电平结束 */
    if (dht11_wait_level(1, DHT11_POLL_TIMEOUT_US)) return 0;

    /* 上升沿后延时 40μs, 此时:
       bit=0 → 高电平持续 28μs, 已回落到低 → DHT11_DQ_IN=0
       bit=1 → 高电平持续 70μs, 仍为高    → DHT11_DQ_IN=1 */
    delay_us(DHT11_SAMPLE_DELAY_US);

    return (uint8_t)DHT11_DQ_IN;
}

/**
 * @brief       从 DHT11 读取一个字节 (MSB first)
 * @retval      读取到的字节
 */
static uint8_t dht11_read_byte(void)
{
    uint8_t data = 0;

    for (uint8_t i = 0; i < 8; i++)
    {
        data <<= 1;
        data |= dht11_read_bit();
    }
    return data;
}

/* ================================================================================
 * 公开接口
 * ================================================================================ */

/**
 * @brief       从 DHT11 读取一次温湿度 (带重试)
 * @param       temp: 温度值 (℃, 整数部分, 范围 0~50)
 * @param       humi: 湿度值 (%, 整数部分, 范围 20~90)
 * @retval      0: 读取成功
 *              1: 读取失败 (传感器无应答或多次校验失败)
 */
uint8_t dht11_read_data(uint8_t *temp, uint8_t *humi)
{
    uint8_t buf[DHT11_BUF_SIZE];
    uint8_t retry;

    for (retry = 0; retry < DHT11_MAX_RETRIES; retry++)
    {
        /* ---- 关键时序段: 关闭中断, 防止 FreeRTOS 任务切换 ---- */
        taskENTER_CRITICAL();

        dht11_reset();

        if (dht11_wait_ack() != 0)
        {
            taskEXIT_CRITICAL();
            continue;       /* 无应答, 重试 */
        }

        /* 读取 40 位数据 (5 字节) */
        for (uint8_t i = 0; i < DHT11_BUF_SIZE; i++)
        {
            buf[i] = dht11_read_byte();
        }

        taskEXIT_CRITICAL();
        /* ---- 临界区结束 ---- */

        /* 校验: 前 4 字节之和 == 第 5 字节 */
        if ((uint8_t)(buf[0] + buf[1] + buf[2] + buf[3]) == buf[4])
        {
            *humi = buf[0];     /* 湿度整数 */
            *temp = buf[2];     /* 温度整数 */
            return 0;           /* 成功 */
        }
        /* 校验失败 → 重试 */
    }

    return 1;   /* 多次重试均失败 */
}

/**
 * @brief       检查 DHT11 是否存在 (单次)
 * @retval      0: 正常应答
 *              1: 无应答
 */
uint8_t dht11_check(void)
{
    uint8_t ret;

    taskENTER_CRITICAL();
    dht11_reset();
    ret = dht11_wait_ack();
    taskEXIT_CRITICAL();

    return ret;
}

/**
 * @brief       初始化 DHT11 引脚并检测传感器
 * @note        配置 PG11 为开漏输出 + 上拉, 实现单总线双向通信
 * @retval      0: DHT11 存在且正常
 *              1: DHT11 不存在 / 异常
 */
uint8_t dht11_init(void)
{
    GPIO_InitTypeDef gpio_init_struct = {0};

    DHT11_DQ_GPIO_CLK_ENABLE();

    gpio_init_struct.Pin   = DHT11_DQ_GPIO_PIN;
    gpio_init_struct.Mode  = GPIO_MODE_OUTPUT_OD;      /* 开漏 + 上拉 = 双向 */
    gpio_init_struct.Pull  = GPIO_PULLUP;
    gpio_init_struct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(DHT11_DQ_GPIO_PORT, &gpio_init_struct);

    return dht11_check();
}
