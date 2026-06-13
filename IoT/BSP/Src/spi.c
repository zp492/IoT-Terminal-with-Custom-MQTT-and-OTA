/**
 ****************************************************************************************************
 * @file        spi.c
 * @brief       SPI2驱动 + W5500控制引脚初始化
 * @note        硬件连接 (正点原子 F103ZET6 + W5500模块):
 *              SPI2_SCK  → PB13  (AF推挽)
 *              SPI2_MISO → PB14  (AF推挽)
 *              SPI2_MOSI → PB15  (AF推挽)
 *              W5500_SCS → PB12  (推挽输出, 软件控制片选)
 *              W5500_RST → PD6   (推挽输出, 低电平复位)
 *              W5500_INT → PA1   (浮空输入, 中断引脚, 可选)
 ****************************************************************************************************
 */

#include "spi.h"

SPI_HandleTypeDef g_spi_handle;    /* SPI2句柄, 全局变量 */

/**
 * @brief       SPI2初始化 (W5500专用)
 * @note        SPI模式0 (CPOL=0, CPHA=0), 与W5500要求一致
 *              W5500最高SPI时钟80MHz, SPI2挂APB1(PCLK1=36MHz), 分频后最高18MHz
 */
void spi2_init(void)
{
    g_spi_handle.Instance = SPI2;
    g_spi_handle.Init.Mode = SPI_MODE_MASTER;                        /* 主机模式 */
    g_spi_handle.Init.Direction = SPI_DIRECTION_2LINES;              /* 双线全双工 */
    g_spi_handle.Init.DataSize = SPI_DATASIZE_8BIT;                  /* 8位数据帧 */
    g_spi_handle.Init.CLKPolarity = SPI_POLARITY_LOW;                /* CPOL=0, 空闲时SCK低电平 */
    g_spi_handle.Init.CLKPhase = SPI_PHASE_1EDGE;                    /* CPHA=0, 第一个边沿采样 */
    g_spi_handle.Init.NSS = SPI_NSS_SOFT;                            /* 软件管理NSS */
    g_spi_handle.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_256; /* 默认最低速, 之后调高 */
    g_spi_handle.Init.FirstBit = SPI_FIRSTBIT_MSB;                   /* 高位在前 */
    g_spi_handle.Init.TIMode = SPI_TIMODE_DISABLE;                   /* 不使用TI模式 */
    g_spi_handle.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;   /* 不使用硬件CRC */
    g_spi_handle.Init.CRCPolynomial = 7;                             /* CRC多项式默认值 */
    HAL_SPI_Init(&g_spi_handle);                                     /* 会调用 HAL_SPI_MspInit */

    __HAL_SPI_ENABLE(&g_spi_handle);

    spi2_write_read_byte(0XFF); /* 发送一个空字节, 产生8个SCK脉冲清空SPI_DR, 启动传输 */
}

/**
 * @brief       SPI2硬件层初始化 (HAL回调)
 * @note        初始化SPI2对应的GPIO引脚 + W5500控制引脚
 */
void HAL_SPI_MspInit(SPI_HandleTypeDef *hspi)
{
    GPIO_InitTypeDef gpio_init_struct = {0};

    if (hspi->Instance == SPI2)
    {
        /* ---- 1. 使能时钟 ---- */
        __HAL_RCC_SPI2_CLK_ENABLE();    /* SPI2外设时钟 */
        __HAL_RCC_GPIOB_CLK_ENABLE();   /* PB12/PB13/PB14/PB15 */
        __HAL_RCC_GPIOD_CLK_ENABLE();   /* PD6  (W5500_RST) */
        __HAL_RCC_GPIOA_CLK_ENABLE();   /* PA1  (W5500_INT) */
        __HAL_RCC_AFIO_CLK_ENABLE();    /* AFIO复用时钟 */

        /* ---- 2. SPI2 复用功能引脚 (PB13/PB14/PB15) ---- */
        gpio_init_struct.Pin   = GPIO_PIN_13 | GPIO_PIN_14 | GPIO_PIN_15;
        gpio_init_struct.Mode  = GPIO_MODE_AF_PP;       /* 复用推挽输出 */
        gpio_init_struct.Pull  = GPIO_NOPULL;
        gpio_init_struct.Speed = GPIO_SPEED_FREQ_HIGH;  /* 高速 */
        HAL_GPIO_Init(GPIOB, &gpio_init_struct);

        /* ---- 3. W5500 控制引脚 ---- */

        /* W5500_SCS (PB12): 片选, 推挽输出, 初始高电平(未选中) */
        gpio_init_struct.Pin   = W5500_SCS_PIN;
        gpio_init_struct.Mode  = GPIO_MODE_OUTPUT_PP;
        gpio_init_struct.Pull  = GPIO_PULLUP;
        gpio_init_struct.Speed = GPIO_SPEED_FREQ_HIGH;
        HAL_GPIO_Init(W5500_SCS_PORT, &gpio_init_struct);
        W5500_SCS_HIGH();  /* 初始: 取消片选 */

        /* W5500_RST (PD6): 硬件复位, 推挽输出, 初始高电平(不复位) */
        gpio_init_struct.Pin   = W5500_RST_PIN;
        gpio_init_struct.Mode  = GPIO_MODE_OUTPUT_PP;
        gpio_init_struct.Pull  = GPIO_PULLUP;
        gpio_init_struct.Speed = GPIO_SPEED_FREQ_LOW;
        HAL_GPIO_Init(W5500_RST_PORT, &gpio_init_struct);
        W5500_RST_HIGH();  /* 初始: 不复位 */

        /* W5500_INT (PA1): 中断输入, 浮空输入 (不使用中断时可不配置) */
        gpio_init_struct.Pin   = W5500_INT_PIN;
        gpio_init_struct.Mode  = GPIO_MODE_INPUT;
        gpio_init_struct.Pull  = GPIO_NOPULL;
        HAL_GPIO_Init(W5500_INT_PORT, &gpio_init_struct);
    }
}

/**
 * @brief       SPI2 写入一个字节并读取一个字节
 * @param       data: 要发送的数据
 * @retval      接收到的数据
 * @note        W5500 SPI协议: 写操作时忽略返回值, 读操作时发0xFF产生SCK时钟
 */
uint8_t spi2_write_read_byte(uint8_t data)
{
    uint8_t rxdata;
    HAL_SPI_TransmitReceive(&g_spi_handle, &data, &rxdata, 1, 1000);
    return rxdata;
}

/**
 * @brief       设置SPI2速度
 * @param       speed: 预分频值, 使用 SPI_SPEED_2 ~ SPI_SPEED_256 宏
 * @note        F103 SPI2时钟源 = PCLK1 = 36MHz (APB1, SYSCLK/2)
 *              SPI_SCK = PCLK1 / (2 << speed)
 *              例如: speed=SPI_SPEED_2 (=0) → SPI_SCK = 36 / 2 = 18MHz
 *              W5500 SPI最大时钟80MHz, F103下SPI2最高18MHz (SPI_SPEED_2)
 */
void spi2_set_speed(uint8_t speed)
{
    assert_param(IS_SPI_BAUDRATE_PRESCALER(speed)); /* 参数合法性检查 */
    __HAL_SPI_DISABLE(&g_spi_handle);
    g_spi_handle.Instance->CR1 &= 0xFFC7;
    g_spi_handle.Instance->CR1 |= (speed << 3);
    __HAL_SPI_ENABLE(&g_spi_handle);
}
