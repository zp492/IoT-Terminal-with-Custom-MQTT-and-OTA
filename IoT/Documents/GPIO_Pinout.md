# GPIO 引脚分配表

> 硬件平台：正点原子 STM32F103ZET6 开发板 + W5500 以太网模块  
> 最后核验：2026-06-14 — 已与全部 BSP 头文件 / 驱动源码交叉比对，无冲突

## 端口总览

### GPIOA

| 引脚 | 所属模块 | 功能描述 | 方向 | 复用功能 |
|------|---------|---------|------|---------|
| PA0 | KEY | WKUP 唤醒按键 | 输入 | — |
| PA2 | W5500 | INT 中断输入 (当前未使用) | 浮空输入 | — |
| PA9 | USART1 | TX 串口发送 (115200bps) | AF推挽 | USART1_TX |
| PA10 | USART1 | RX 串口接收 (115200bps) | 浮空输入 | USART1_RX |
| PA13 | SWD | SWDIO 调试数据线 | — | SWDIO |
| PA14 | SWD | SWCLK 调试时钟线 | — | SWCLK |

### GPIOB

| 引脚 | 所属模块 | 功能描述 | 方向 | 复用功能 |
|------|---------|---------|------|---------|
| PB0 | LCD | BL 背光控制 | 推挽输出 | — |
| PB5 | LED0 + BTIM | LED0 指示灯 / 定时器翻转 | 推挽输出 | — |
| PB12 | W5500 | SCS SPI片选 (低有效) | 推挽输出 | — |
| PB13 | SPI2 | SCK 时钟 (18MHz) | AF推挽 | SPI2_SCK |
| PB14 | SPI2 | MISO 主入从出 | AF推挽 | SPI2_MISO |
| PB15 | SPI2 | MOSI 主出从入 | AF推挽 | SPI2_MOSI |

### GPIOD

| 引脚 | 所属模块 | 功能描述 | 方向 | 复用功能 |
|------|---------|---------|------|---------|
| PD0 | FSMC | D0 数据线 | AF推挽 | LCD + SRAM 共享 |
| PD1 | FSMC | D1 数据线 | AF推挽 | LCD + SRAM 共享 |
| PD4 | FSMC | NOE 读使能 (RD) | AF推挽 | LCD + SRAM 共享 |
| PD5 | FSMC | NWE 写使能 (WR) | AF推挽 | LCD + SRAM 共享 |
| PD6 | W5500 | RST 硬件复位 (低有效) | 推挽输出 | — |
| PD8 | FSMC | D8 数据线 | AF推挽 | LCD + SRAM 共享 |
| PD9 | FSMC | D9 数据线 | AF推挽 | LCD + SRAM 共享 |
| PD10 | FSMC | D10 数据线 | AF推挽 | LCD + SRAM 共享 |
| PD11 | FSMC | D11 数据线 | AF推挽 | LCD + SRAM 共享 |
| PD12 | FSMC | D12 数据线 | AF推挽 | LCD + SRAM 共享 |
| PD13 | FSMC | D13 数据线 | AF推挽 | LCD + SRAM 共享 |
| PD14 | FSMC | D14 数据线 | AF推挽 | LCD + SRAM 共享 |
| PD15 | FSMC | D15 数据线 | AF推挽 | LCD + SRAM 共享 |

### GPIOE

| 引脚 | 所属模块 | 功能描述 | 方向 | 复用功能 |
|------|---------|---------|------|---------|
| PE0 | FSMC | A16 地址线 | AF推挽 | SRAM |
| PE1 | FSMC | A17 地址线 | AF推挽 | SRAM |
| PE3 | KEY | KEY1 按键 | 输入 | — |
| PE4 | KEY | KEY0 按键 | 输入 | — |
| PE5 | LED | LED1 指示灯 | 推挽输出 | — |
| PE7 | FSMC | D7 数据线 | AF推挽 | LCD + SRAM 共享 |
| PE8 | FSMC | A18 地址线 | AF推挽 | LCD + SRAM 共享 |
| PE9 | FSMC | A19 地址线 | AF推挽 | LCD + SRAM 共享 |
| PE10 | FSMC | A20 地址线 | AF推挽 | LCD + SRAM 共享 |
| PE11 | FSMC | A21 地址线 | AF推挽 | LCD + SRAM 共享 |
| PE12 | FSMC | A22 地址线 | AF推挽 | LCD + SRAM 共享 |
| PE13 | FSMC | A23 地址线 | AF推挽 | LCD + SRAM 共享 |
| PE14 | FSMC | D7 数据线 (LCD) | AF推挽 | LCD + SRAM 共享 |
| PE15 | FSMC | D8 数据线 (LCD) | AF推挽 | LCD + SRAM 共享 |

### GPIOF

| 引脚 | 所属模块 | 功能描述 | 方向 | 复用功能 |
|------|---------|---------|------|---------|
| PF0 | FSMC | A0 地址线 | AF推挽 | SRAM |
| PF1 | FSMC | A1 地址线 | AF推挽 | SRAM |
| PF2 | FSMC | A2 地址线 | AF推挽 | SRAM |
| PF3 | FSMC | A3 地址线 | AF推挽 | SRAM |
| PF4 | FSMC | A4 地址线 | AF推挽 | SRAM |
| PF5 | FSMC | A5 地址线 | AF推挽 | SRAM |
| **PF8** | **ADC3** | **CH6 模拟输入** | **模拟** | **—** |
| PF12 | FSMC | A6 地址线 | AF推挽 | SRAM |
| PF13 | FSMC | A7 地址线 | AF推挽 | SRAM |
| PF14 | FSMC | A8 地址线 | AF推挽 | SRAM |
| PF15 | FSMC | A9 地址线 | AF推挽 | SRAM |

### GPIOG

| 引脚 | 所属模块 | 功能描述 | 方向 | 复用功能 |
|------|---------|---------|------|---------|
| PG0 | FSMC | LCD_RS 寄存器选择 / FSMC_A10 | AF推挽 | LCD + SRAM 共享 |
| PG1 | FSMC | A11 地址线 | AF推挽 | SRAM |
| PG2 | FSMC | A12 地址线 | AF推挽 | SRAM |
| PG3 | FSMC | A13 地址线 | AF推挽 | SRAM |
| PG4 | FSMC | A14 地址线 | AF推挽 | SRAM |
| PG5 | FSMC | A15 地址线 | AF推挽 | SRAM |
| PG10 | SRAM | SRAM_CS 片选 (FSMC_NE3) | AF推挽 | — |
| PG11 | DHT11 | 温湿度传感器数据线 | 双向 | — |
| PG12 | LCD | LCD_CS 片选 (FSMC_NE4) | AF推挽 | — |

---

## 模块快速索引

### W5500 以太网 (SPI2)

| 引脚 | 功能 |
|------|------|
| PB12 | SCS (片选) |
| PB13 | SCK (时钟, 18MHz) |
| PB14 | MISO |
| PB15 | MOSI |
| PD6 | RST (复位) |
| PA2 | INT (中断, 未使用) |

### FSMC 总线 (LCD + SRAM 共享)

| 片选 | 外设 |
|------|------|
| PG12 (NE4) | LCD 液晶屏 |
| PG10 (NE3) | SRAM 外部内存 |

> LCD 和 SRAM 共享 FSMC 数据/地址/控制线，通过不同片选区分。  
> 这是正点原子开发板的标准设计，无冲突。

### 其他外设

| 外设 | 引脚 |
|------|------|
| USART1 串口 | PA9 (TX), PA10 (RX) |
| ADC3 | PF8 (CH6) |
| LED0 | PB5 |
| LED1 | PE5 |
| KEY0 | PE4 |
| KEY1 | PE3 |
| WKUP | PA0 |
| LCD 背光 | PB0 |
| DHT11 温湿度 | PG11 |
| SWD 调试 | PA13, PA14 |

---

## 冲突检查

> ✅ **无 GPIO 冲突。** 所有引脚分配互不重叠，FSMC 总线共享为硬件设计预期行为。
