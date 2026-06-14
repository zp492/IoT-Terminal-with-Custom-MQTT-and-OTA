/**
 ****************************************************************************************************
 * @file        adc.c
 * @brief       ADC3 驱动 — 通道 6 / DMA 连续采集
 * @note        参考 adc_dma.c 模式重写:
 *              硬件:  PF8 → ADC3_CH6 (模拟输入)
 *              DMA:  ADC3 → DMA2_Channel4 → 内存
 *              中断:  DMA2_Channel4_5_IRQHandler, 优先级 3/3
 *              时钟:  ADC 工作于 PCLK2 / 6 = 72 / 6 = 12MHz
 *              转换:  单通道 / 连续 / 软件触发
 ****************************************************************************************************
 */

#include "adc.h"

/* ================================================================================
 * 硬件句柄 & 状态
 * ================================================================================ */

static ADC_HandleTypeDef g_adc_handle;          /* ADC3 句柄 */
static DMA_HandleTypeDef g_dma_adc_handle;      /* DMA2_Channel4 句柄 */
static volatile uint8_t  g_dma_adc_sta;         /* DMA 传输完成标志 (ISR 置 1) */
static uint16_t g_adc_buf[1];                   /* DMA 目标缓冲区 */

/* ================================================================================
 * 初始化 & 配置
 * ================================================================================ */

/**
 * @brief       ADC3 DMA 初始化
 * @param       Dst: DMA 目标地址 (此处为 g_adc_buf)
 */
static void adc_dma_init(uint32_t Dst)
{
    ADC_ChannelConfTypeDef adc_ch_conf = {0};

    /* ---- 1. DMA2 时钟 & 初始化 ---- */
    __HAL_RCC_DMA2_CLK_ENABLE();

    g_dma_adc_handle.Instance = DMA2_Chann·el4;
    g_dma_adc_handle.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    g_dma_adc_handle.Init.PeriphInc           = DMA_PINC_DISABLE;
    g_dma_adc_handle.Init.MemInc              = DMA_MINC_ENABLE;
    g_dma_adc_handle.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    g_dma_adc_handle.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    g_dma_adc_handle.Init.Mode                = DMA_NORMAL;
    g_dma_adc_handle.Init.Priority            = DMA_PRIORITY_HIGH;
    HAL_DMA_Init(&g_dma_adc_handle);          /* → 触发 HAL_DMA_MspInit */

    /* ---- 2. 关联 DMA → ADC ---- */
    __HAL_LINKDMA(&g_adc_handle, DMA_Handle, g_dma_adc_handle);

    /* ---- 3. ADC3 参数 ---- */
    g_adc_handle.Instance                   = ADC3;
    g_adc_handle.Init.DataAlign             = ADC_DATAALIGN_RIGHT;
    g_adc_handle.Init.ScanConvMode          = ADC_SCAN_DISABLE;
    g_adc_handle.Init.ContinuousConvMode    = ENABLE;
    g_adc_handle.Init.NbrOfConversion       = 1;
    g_adc_handle.Init.DiscontinuousConvMode = DISABLE;
    g_adc_handle.Init.ExternalTrigConv      = ADC_SOFTWARE_START;
    HAL_ADC_Init(&g_adc_handle);            /* → 触发 HAL_ADC_MspInit */

    HAL_ADCEx_Calibration_Start(&g_adc_handle);

    /* ---- 4. 通道配置 ---- */
    adc_ch_conf.Channel      = ADC_CHANNEL_6;
    adc_ch_conf.Rank         = ADC_REGULAR_RANK_1;
    adc_ch_conf.SamplingTime = ADC_SAMPLETIME_239CYCLES_5;
    HAL_ADC_ConfigChannel(&g_adc_handle, &adc_ch_conf);

    /* ---- 5. DMA 中断 ---- */
    HAL_NVIC_SetPriority(DMA2_Channel4_5_IRQn, 3, 3);
    HAL_NVIC_EnableIRQ(DMA2_Channel4_5_IRQn);

    /* ---- 6. 启动 DMA + ADC ---- */
    HAL_DMA_Start_IT(&g_dma_adc_handle, (uint32_t)&ADC3->DR, Dst, 0);
    HAL_ADC_Start_DMA(&g_adc_handle, &Dst, 0);
}

/**
 * @brief       ADC3 引脚初始化 (HAL 回调)
 */
void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC3)
    {
        GPIO_InitTypeDef       gpio_init = {0};
        RCC_PeriphCLKInitTypeDef clk_init = {0};

        __HAL_RCC_GPIOF_CLK_ENABLE();       /* PF8 时钟 */
        __HAL_RCC_ADC3_CLK_ENABLE();        /* ADC3 时钟 */

        /* PF8 → ADC3_CH6 模拟输入 */
        gpio_init.Pin  = GPIO_PIN_8;
        gpio_init.Mode = GPIO_MODE_ANALOG;
        HAL_GPIO_Init(GPIOF, &gpio_init);

        /* ADC 时钟 = PCLK2 / 6 = 72 / 6 = 12MHz */
        clk_init.PeriphClockSelection = RCC_PERIPHCLK_ADC;
        clk_init.AdcClockSelection    = RCC_ADCPCLK2_DIV6;
        HAL_RCCEx_PeriphCLKConfig(&clk_init);
    }
}

/**
 * @brief       启动 DMA 传输 (重新装载计数器)
 * @param       cndtr: 传输次数
 */
static void adc_dma_enable(uint16_t cndtr)
{
    ADC3->CR2 &= ~(1 << 0);                 /* 停 ADC */
    DMA2_Channel4->CCR &= ~(1 << 0);        /* 停 DMA */

    while (DMA2_Channel4->CCR & (1 << 0));  /* 等待 DMA 停稳 */
    DMA2_Channel4->CNDTR = cndtr;           /* 重载传输次数 */
    DMA2_Channel4->CCR |= 1 << 0;           /* 开 DMA */
    ADC3->CR2 |= 1 << 0;                    /* 开 ADC */

    ADC3->CR2 |= 1 << 22;                   /* 软件触发转换 */
}

/* ================================================================================
 * 中断服务
 * ================================================================================ */

/**
 * @brief       DMA2 Channel4/5 共用中断
 * @note        ADC3 → DMA2_Channel4, TCIF4 = DMA2_ISR bit13
 */
void DMA2_Channel4_5_IRQHandler(void)
{
    if (DMA2->ISR & (1 << 13))              /* TCIF4: Channel4 传输完成 */
    {
        g_dma_adc_sta = 1;
        DMA2->IFCR |= (1 << 13);            /* 清除标志 */
    }
}

/* ================================================================================
 * 公开接口
 * ================================================================================ */

/**
 * @brief       初始化 ADC3_CH6 + DMA 连续采集
 * @note        调用此函数后, DMA 自动将 ADC3_CH6 的转换结果写入内部缓冲区
 */
void adc_init(void)
{
    adc_dma_init((uint32_t)g_adc_buf);
    adc_dma_enable(1);
}

/**
 * @brief       获取最近一次 ADC 采样值
 * @retval      12 位原始值 (0 ~ 4095)
 */
uint16_t adc_get_value(void)
{
    return g_adc_buf[0];
}

/**
 * @brief       获取 ADC 电压 (mV)
 * @note        基准电压 3.3V = 3300mV
 *              mV = raw × 3300 / 4096
 */
uint16_t adc_get_mv(void)
{
    return (uint16_t)((uint32_t)g_adc_buf[0] * 3300 / 4096);
}
