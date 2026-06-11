#include "btim.h"

TIM_HandleTypeDef g_btim_handle;

void btim_timx_int_init(uint16_t psc, uint16_t arr) /* 参数psc预分频系数、自动重装载值arr用户定义 */
{
    g_btim_handle.Instance = TIM6;
    g_btim_handle.Init.Prescaler = psc;
    g_btim_handle.Init.Period = arr;

    HAL_TIM_Base_Init(&g_btim_handle);

    HAL_TIM_Base_Start_IT(&g_btim_handle);
}

void HAL_TIM_Base_MspInit(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM6) /* 判断为定时器6，以下为对定时器6操作 */
    {
        __HAL_RCC_TIM6_CLK_ENABLE(); /* 使能定时器6时钟 */
        HAL_NVIC_SetPriority(TIM6_IRQn, 2, 2);
        HAL_NVIC_EnableIRQ(TIM6_IRQn); /* 使能中断，设置对应中断号的中断优先级 */
    }
}

void TIM6_IRQHandler()
{
    HAL_TIM_IRQHandler(&g_btim_handle); /* 定时器中断公共处理函数在hal_tim.c中 */
}

void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
    if (htim->Instance == TIM6)
    {
        HAL_GPIO_TogglePin(GPIOB, GPIO_PIN_5);
    }
}
