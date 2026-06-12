#ifndef __SPI_H
#define __SPI_H

#include "./SYSTEM/sys/sys.h"

/* ================================================================================
 * SPI总线速度设置 (预分频系数)
 * 72MHz / (2 << speed) = SPI时钟频率
 * ================================================================================ */
#define SPI_SPEED_2     0   /* 72 / 2  = 36MHz  */
#define SPI_SPEED_4     1   /* 72 / 4  = 18MHz  */
#define SPI_SPEED_8     2   /* 72 / 8  = 9MHz   */
#define SPI_SPEED_16    3   /* 72 / 16 = 4.5MHz */
#define SPI_SPEED_32    4   /* 72 / 32 = 2.25MHz*/
#define SPI_SPEED_64    5   /* 72 / 64 = 1.125MHz*/
#define SPI_SPEED_128   6   /* 72 / 128= 562KHz */
#define SPI_SPEED_256   7   /* 72 / 256= 281KHz */

/* ================================================================================
 * W5500 硬件引脚定义 (正点原子 STM32F103ZET6 + W5500模块)
 * --------------------------------------------------------------------------------
 * SPI2 引脚 (复用推挽, AFIO):
 *   PB13 → SPI2_SCK    (时钟)
 *   PB14 → SPI2_MISO   (主机输入/从机输出)
 *   PB15 → SPI2_MOSI   (主机输出/从机输入)
 *
 * 普通GPIO引脚:
 *   PB12 → W5500_SCS   (SPI片选, 推挽输出, 低电平选中)
 *   PD6  → W5500_RST   (硬件复位, 推挽输出, 低电平复位)
 *   PA1  → W5500_INT   (中断输入, 可选, 浮空输入)
 * ================================================================================ */
#define W5500_SCS_PORT      GPIOB
#define W5500_SCS_PIN       GPIO_PIN_12
#define W5500_RST_PORT      GPIOD
#define W5500_RST_PIN       GPIO_PIN_6
#define W5500_INT_PORT      GPIOA
#define W5500_INT_PIN       GPIO_PIN_1

/* SPI句柄声明 (定义在spi.c中) */
extern SPI_HandleTypeDef g_spi_handle;

/* SPI2 基本操作 */
void    spi2_init(void);
uint8_t spi2_write_read_byte(uint8_t data);
void    spi2_set_speed(uint8_t speed);

/* W5500 片选控制 (宏, 供w5500_port.c使用) */
#define W5500_SCS_LOW()     HAL_GPIO_WritePin(W5500_SCS_PORT, W5500_SCS_PIN, GPIO_PIN_RESET)
#define W5500_SCS_HIGH()    HAL_GPIO_WritePin(W5500_SCS_PORT, W5500_SCS_PIN, GPIO_PIN_SET)
#define W5500_RST_LOW()     HAL_GPIO_WritePin(W5500_RST_PORT, W5500_RST_PIN, GPIO_PIN_RESET)
#define W5500_RST_HIGH()    HAL_GPIO_WritePin(W5500_RST_PORT, W5500_RST_PIN, GPIO_PIN_SET)
#define W5500_INT_READ()    HAL_GPIO_ReadPin(W5500_INT_PORT, W5500_INT_PIN)

#endif
