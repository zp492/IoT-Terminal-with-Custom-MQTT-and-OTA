#include "./BSP/ADC/adc.h"

ADC_HandleTypeDef g_adc_handle;
DMA_HandleTypeDef g_dma_adc_handle;
uint8_t g_dma_adc_sta;

void adc_dma_init(uint32_t Dst)
{
    ADC_ChannelConfTypeDef adc_chy_handle = {0};
    
    __HAL_RCC_DMA1_CLK_ENABLE();
    
    g_dma_adc_handle.Instance = DMA1_Channel1;                           /* DMA的外设基地址配置带通道    */
    g_dma_adc_handle.Init.Direction = DMA_PERIPH_TO_MEMORY;
    g_dma_adc_handle.Init.PeriphInc = DMA_PINC_DISABLE;
    g_dma_adc_handle.Init.MemInc = DMA_MINC_ENABLE;
    g_dma_adc_handle.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;
    g_dma_adc_handle.Init.MemDataAlignment = DMA_MDATAALIGN_HALFWORD;
    g_dma_adc_handle.Init.Mode = DMA_NORMAL;
    g_dma_adc_handle.Init.Priority = DMA_PRIORITY_HIGH;
    HAL_DMA_Init(&g_dma_adc_handle);     /* DMA初始化，DMA作为通用外设无MspInit */
    
    __HAL_LINKDMA(&g_adc_handle, DMA_Handle, g_dma_adc_handle);//(__HANDLE__)->__PPP_DMA_FIELD__ = &(__DMA_HANDLE__);联系DMA与ADC
    
    g_adc_handle.Instance = ADC1;                                  /* ADC1 */
    g_adc_handle.Init.DataAlign = ADC_DATAALIGN_RIGHT;             /* 右对齐 */
    g_adc_handle.Init.ScanConvMode = ADC_SCAN_DISABLE;            /* 不扫描 */  
    g_adc_handle.Init.ContinuousConvMode = ENABLE;              /* 连续 */ 
    g_adc_handle.Init.NbrOfConversion = 1;                     /* 单通道 */
    g_adc_handle.Init.DiscontinuousConvMode = DISABLE;        /* 不间断 */
    g_adc_handle.Init.ExternalTrigConv = ADC_SOFTWARE_START; /* 软件触发 */
    HAL_ADC_Init(&g_adc_handle);          /* ADC初始化 */
    
    HAL_ADCEx_Calibration_Start(&g_adc_handle);
    
    adc_chy_handle.Channel = ADC_CHANNEL_1;          /* 配置adc1通道参数 */
    adc_chy_handle.Rank = ADC_REGULAR_RANK_1;
    adc_chy_handle.SamplingTime = ADC_SAMPLETIME_239CYCLES_5;
    HAL_ADC_ConfigChannel( &g_adc_handle, &adc_chy_handle);
    
    HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 3, 3);
    HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);
    
    HAL_DMA_Start_IT(&g_dma_adc_handle, (uint32_t)&ADC1->DR, Dst, 0);
    HAL_ADC_Start_DMA(&g_adc_handle, &Dst, 0);
    
}


void HAL_ADC_MspInit(ADC_HandleTypeDef* hadc)
{
    if (hadc->Instance == ADC1)
    {
        GPIO_InitTypeDef gpio_init_struct;
        RCC_PeriphCLKInitTypeDef adc_clk_init = {0};
        
        __HAL_RCC_GPIOA_CLK_ENABLE();                  /* 使能GPIO时钟及ADC1外设时钟 */
        __HAL_RCC_ADC1_CLK_ENABLE();
        
        gpio_init_struct.Pin = GPIO_PIN_1;
        gpio_init_struct.Mode = GPIO_MODE_ANALOG;       /* 模拟功能 */
        HAL_GPIO_Init(GPIOA, &gpio_init_struct);
        
        adc_clk_init.PeriphClockSelection = RCC_PERIPHCLK_ADC;      /* 配置adc1时钟6分频 */
        adc_clk_init.AdcClockSelection = RCC_ADCPCLK2_DIV6;
        HAL_RCCEx_PeriphCLKConfig(&adc_clk_init);
        
        
    }
}

/* 配置DMA ADC一次传输数目 */
void adc_dma_enable(uint16_t cndtr)
{
    ADC1->CR2 &= ~(1 << 0) ;
    DMA1_Channel1->CCR &= ~(1 << 0);
    
    while (DMA1_Channel1->CCR & (1 << 0));
    DMA1_Channel1->CNDTR = cndtr;  /* 配置好传输数量，开启DMA、ADC */
    DMA1_Channel1->CCR |= 1 << 0;
    ADC1->CR2 |= 1 << 0;
    
    ADC1->CR2 |= 1 << 22;  /*开启ADC规则组转换 */
}


void DMA1_Channel1_IRQHandler(void)
{
    if (DMA1->ISR & (1 << 1))
    {
        g_dma_adc_sta = 1;
        DMA1->IFCR |= (1 << 1);
    }
}


