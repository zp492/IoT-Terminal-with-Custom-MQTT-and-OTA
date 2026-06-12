/**
 ****************************************************************************************************
 * @file        delay.c
 * @version     V2.0
 * @date        2026-06-12
 * @brief       SysTick延时模块(支持裸机和FreeRTOS双模式)
 * @details     提供微秒/毫秒级延时函数, 包括:
 *              - delay_init()   : 初始化SysTick, 计算时基倍乘数
 *              - delay_us()     : 微秒延时 (OS: 软轮询 / 裸机: 硬件倒计时)
 *              - delay_ms()     : 毫秒延时 (OS: 单次轮询累加 / 裸机: 分块倒计时)
 *              - HAL_Delay()    : HAL库内部延时适配 (仅裸机模式)
 *
 * @note        硬件平台: 正点原子 STM32F103ZET6 开发板
 *              - MCU:    STM32F103ZET6 (Cortex-M3, 72MHz主频)
 *              - 时钟源: SYSTICK使用内核时钟源8分频 (HCLK/8)
 *              - 计数器: 24位递减计数器, 最大值16,777,216
 *
 * @note        OS模式 (SYS_SUPPORT_OS=1):
 *              - SysTick用作FreeRTOS心跳, 中断周期 = 1 / configTICK_RATE_HZ
 *              - delay_us/delay_ms 通过读取 SysTick->VAL 当前值进行软轮询累加
 *              - 延时期间不关调度, 可被高优先级任务抢占
 *
 * @note        裸机模式 (SYS_SUPPORT_OS=0):
 *              - SysTick仅用于延时, 不开启中断
 *              - delay_us 直接设置LOAD值硬件倒计时, 受24位寄存器限制
 *              - delay_ms 按1000ms分块调用delay_us, 兼容超频场景(如128MHz)
 *
 * @note        修改历史
 *              V1.0 20200417  正点原子团队: 第一次发布, 原始版本
 *              V2.0 20260612  zp492: 优化OS版delay_ms(循环调用→单次轮询累加)
 *                                    合并delay_init中重复的#if块
 ****************************************************************************************************
 */

#include "./SYSTEM/sys/sys.h"
#include "./SYSTEM/delay/delay.h"

static uint16_t g_fac_us = 0; /* us延时倍乘数 */

/* 如果SYS_SUPPORT_OS定义了,说明要支持OS了(不限于UCOS) */
#if SYS_SUPPORT_OS

/* 添加公共头文件 (FreeRTOS 需要用到) */
#include "FreeRTOS.h"
#include "task.h"

extern void xPortSysTickHandler(void);
/**
 * @brief       systick中断服务函数,使用OS时用到
 * @param       ticks: 延时的节拍数
 * @retval      无
 */
void SysTick_Handler(void)
{
    HAL_IncTick();
    /* OS 开始跑了,才执行正常的调度处理 */
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED)
    {
        xPortSysTickHandler();
    }
}
#endif

/**
 * @brief       初始化延迟函数
 * @param       sysclk: 系统时钟频率, 即CPU频率(HCLK)
 * @retval      无
 */
void delay_init(uint16_t sysclk)
{
    SysTick->CTRL = 0;                                        /* 清Systick状态，以便下一步重设，如果这里开了中断会关闭其中断 */
    HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK_DIV8); /* SYSTICK使用内核时钟源8分频,产生1ms的基准，因systick的计数器最大值只有2^24 */

    g_fac_us = sysclk / 8; /* 不论是否使用OS,g_fac_us都需要使用,作为1us的基础时基 */
#if SYS_SUPPORT_OS         /* 如果需要支持OS. */
    {
        /* 优化：将reload声明与使用合并到同一个#if块内，用{}限定作用域 */
        uint32_t reload = sysclk / 8;   /* 每秒钟的计数次数 单位为M */
        /* 使用 configTICK_RATE_HZ 计算重装载值
         * configTICK_RATE_HZ 在 FreeRTOSConfig.h 中定义
         */
        reload *= 1000000 / configTICK_RATE_HZ; /* 根据delay_ostickspersec设定溢出时间
                                                 * reload为24位寄存器,最大值:16777216,在9M下,约合1.86s左右
                                                 */
        SysTick->CTRL |= 1 << 1;                /* 开启SYSTICK中断 */
        SysTick->LOAD = reload;                 /* 每1/delay_ostickspersec秒中断一次 */
        SysTick->CTRL |= 1 << 0;                /* 开启SYSTICK */
    }
#endif
}

#if SYS_SUPPORT_OS /* 如果需要支持OS, 用以下代码 */

/**
 * @brief       延时nus
 * @param       nus: 要延时的us数.
 * @note        nus取值范围: 0 ~ 477218588(最大值即2^32 / g_fac_us @g_fac_us = 9)
 * @retval      无
 */
void delay_us(uint32_t nus)
{
    uint32_t ticks;
    uint32_t told, tnow, tcnt = 0;
    uint32_t reload;
    reload = SysTick->LOAD; /* LOAD的值 */
    ticks = nus * g_fac_us; /* 需要的节拍数 */
                            //    delay_osschedlock();        /* 阻止OS调度，防止打断us延时 */
    told = SysTick->VAL;    /* 刚进入时的计数器值 */

    while (1)
    {
        tnow = SysTick->VAL;

        if (tnow != told)
        {
            if (tnow < told)
            {
                tcnt += told - tnow; /* 这里注意一下SYSTICK是一个递减的计数器就可以了. */
            }
            else
            {
                tcnt += reload - tnow + told;
            }

            told = tnow;

            if (tcnt >= ticks)
                break; /* 时间超过/等于要延迟的时间,则退出. */
        }
    }

    //    delay_osschedunlock();              /* 恢复OS调度 */
}

/**
 * @brief       延时nms
 * @param       nms: 要延时的ms数 (0< nms <= 65535)
 * @retval      无
 */
void delay_ms(uint16_t nms)
{
    /*
     * 优化说明：
     * 原实现为 for(i=0;i<nms;i++) delay_us(1000)，每次调用 delay_us
     * 都要重新读取 SysTick->LOAD、初始化局部变量、进入独立轮询循环。
     * 以500ms延时为例，需要500次函数调用，开销随延时时长线性增长。
     *
     * 优化后直接在此函数内完成 SysTick 轮询累加，一次完成整个 ms 延时，
     * 无论延时多长都只有一次函数调用和一次轮询循环，显著减少 CPU 开销。
     */
    uint32_t ticks;
    uint32_t told, tnow, tcnt = 0;/*进入值、当前值、差值*/
    uint32_t reload = SysTick->LOAD;    /* 读取重装载值，用于计数器溢出判断 */

    ticks = nms * g_fac_us * 1000;      /* 需要的节拍数 = ms * 每us节拍数 * 1000 */
    told = SysTick->VAL;                

    while (1)
    {
        tnow = SysTick->VAL;            
        if (tnow != told)               /* 计数器有变化才计算，避免空转 */
        {
            /* SysTick是递减计数器：tnow < told 说明未溢出，差值直接累加 */
            if (tnow < told)
                tcnt += told - tnow;
            /* tnow > told 说明计数器溢出重载，累加溢出前后的两段差值 */
            else
                tcnt += reload - tnow + told;
            told = tnow;
            if (tcnt >= ticks) break;    /* 累加节拍数达到目标，退出 */
        }
    }
}

#else /* 不使用OS时, 用以下代码 */

/**
 * @brief       延时nus
 * @param       nus: 要延时的us数.
 * @note        注意: nus的值,不要大于1864135us(最大值即2^24 / g_fac_us  @g_fac_us = 9)
 * @retval      无
 */
void delay_us(uint32_t nus)
{
    uint32_t temp;
    SysTick->LOAD = nus * g_fac_us; /* 时间加载 */
    SysTick->VAL = 0x00;            /* 清空计数器 */
    SysTick->CTRL |= 1 << 0;        /* 开始倒数 */

    do
    {
        temp = SysTick->CTRL;
    } while ((temp & 0x01) && !(temp & (1 << 16))); /* CTRL.ENABLE位必须为1, 并等待时间到达 */

    SysTick->CTRL &= ~(1 << 0); /* 关闭SYSTICK */
    SysTick->VAL = 0X00;        /* 清空计数器 */
}

/**
 * @brief       延时nms
 * @param       nms: 要延时的ms数 (0< nms <= 65535)
 * @retval      无
 */
void delay_ms(uint16_t nms)
{
    uint32_t repeat = nms / 1000; /*  这里用1000,是考虑到可能有超频应用,
                                   *  比如128Mhz的时候, delay_us最大只能延时1048576us左右了
                                   */
    uint32_t remain = nms % 1000;

    while (repeat)
    {
        delay_us(1000 * 1000); /* 利用delay_us 实现 1000ms 延时 */
        repeat--;
    }

    if (remain)
    {
        delay_us(remain * 1000); /* 利用delay_us, 把尾数延时(remain ms)给做了 */
    }
}

/**
  * @brief HAL库内部函数用到的延时
           HAL库的延时默认用Systick，如果我们没有开Systick的中断会导致调用这个延时后无法退出
  * @param Delay 要延时的毫秒数
  * @retval None
  */
void HAL_Delay(uint32_t Delay)
{
    delay_ms(Delay);
}

#endif
