## 功能特性

### 传感器采集
- **DHT11** 温湿度采集，每秒采样
- **ADC3** 模拟量采集 (PF8 / CH6)
- 传感器数据通过 FreeRTOS 消息队列发送到 MQTT 任务

### MQTT 通信
- 自研 MQTT 3.1.1 协议栈（无第三方依赖）
- TCP 流式数据重组（W5500 非阻塞收发）
- 支持 OneNET 云平台 和 公共 Broker（编译开关切换）
- JSON 格式数据上报
- 平台指令下发控制 LED

### OTA 远程升级
- MQTT 通道传输固件（无需额外连接）
- **掉电保护**：逐页原子搬运，任意时刻断电最多损失 1 页
- **CRC32 硬件校验**：与 STM32 硬件 CRC 外设一致的多项式
- **掉电恢复**：Flag 未清除则重试升级，操作幂等
- **LCD 进度条**：实时显示下载/升级进度
- **Python 下发工具**：自动解析 .hex/.bin，分包推送

### 安全设计
- HardFault 时 `configASSERT` 停机（避免静默崩溃）
- 队列溢出自动丢弃（非阻塞发送，MQTT 主循环不卡死）
- OTA 期间 LCD 互斥（sensor 任务跳过写屏）
- Flash 操作在独立低优先级任务中执行

## 目录结构

```
IoT/
├── App/                        # 应用层
│   ├── Onenet/                 # OneNET 云平台适配 (三元组/Token/Topic)
│   ├── ota/                    # OTA 固件下载模块 (队列化架构)
│   ├── freertos_task.c/h       # FreeRTOS 任务创建 & MQTT/OTA 主循环
│   ├── fw_version.h            # 固件版本号 (升级时修改此处)
│   └── fw_info.c               # 固件版本信息嵌入 (固定 Flash 偏移)
├── Bootloader/                 # 裸机 Bootloader (独立 Keil 工程)
│   ├── bootloader.c/h          # 核心流程 (检查→校验→搬运→跳转)
│   ├── bootloader_flash.c/h    # Flash 擦写 (逐页原子操作)
│   ├── bootloader_crc.c/h      # CRC32 硬件校验
│   ├── bootloader_lcd.c/h      # LCD 状态显示
│   ├── bootloader_led.c/h      # LED 状态指示
│   └── bootloader_debug.c/h    # 串口调试输出
├── BSP/                        # 板级驱动
│   ├── Inc/                    # 驱动头文件
│   └── Src/                    # 驱动实现
│       ├── w5500.c             # W5500 以太网驱动
│       ├── socket.c            # Socket API 封装
│       ├── transport.c         # TCP 非阻塞收发传输层
│       ├── lcd.c               # TFT LCD 驱动 (FSMC)
│       ├── dht11.c             # DHT11 温湿度传感器
│       ├── adc.c               # ADC 采集
│       ├── led.c / key.c       # LED 按键驱动
│       └── spi.c               # SPI 驱动 (W5500 通信)
├── Core/
│   ├── Inc/ota_partition.h     # Flash 分区定义 (单一真相源)
│   ├── Inc/FreeRTOSConfig.h    # FreeRTOS 配置 (heap 25KB)
│   └── Src/main.c              # App 入口 (向量表重定位)
├── Middlewares/
│   ├── MQTT/
│   │   ├── mqtt_client.c/h     # MQTT 3.1.1 报文组装/解析
│   │   └── mqtt_wrapper.c/h    # MQTT 连接/保活/收发状态机
│   └── MALLOC/                 # 内存管理 (分页式)
├── Documents/                  # 项目文档
│   ├── GPIO_Pinout.md          # GPIO 引脚分配表
│   ├── MQTT_Call_Table.md      # MQTT 协议栈回调函数关系表
│   ├── OTA_Implementation_Summary.md  # OTA 完整实现总结
│   └── Debug_Log.md            # 联调踩坑日志
├── Tools/
│   └── ota_push.py             # OTA 固件推送工具 (Python)
├── Projects/MDK-ARM/           # Keil MDK 工程文件
│   ├── app.uvprojx             # App 工程
│   └── bootloader.uvprojx      # Bootloader 工程
├── FreeRTOS/                   # FreeRTOS 内核源码
├── Drivers/                    # HAL 库 & CMSIS
│   ├── STM32F1xx_HAL_Driver/
│   ├── CMSIS/
│   └── SYSTEM/                 # 正点原子系统组件 (delay/sys/usart)
└── Output/                     # 分散加载文件 (.sct)
```

## 快速开始

### 前置条件

- Keil MDK 5 (ARMCC5 编译器)
- Python 3.7+ (OTA 推送工具)
- ST-Link 或 J-Link 调试器

### 编译与烧录

**首次烧录（出厂）**

```bash
# 1. 编译 Bootloader → 烧录到 0x08000000
#    Keil 打开 Projects/MDK-ARM/bootloader.uvprojx → Build → Download

# 2. 编译 App → 烧录到 0x0800C000
#    Keil 打开 Projects/MDK-ARM/app.uvprojx → Build → Download

# 3. 复位，串口 (115200bps) 观察启动日志
```

**后续升级**

```bash
# 修改 App/fw_version.h 版本号 → 编译 App → 使用 OTA 推送
python Tools/ota_push.py Output/f103_zj.hex
```

### MQTT 模式切换

编辑 `App/freertos_task.c`：

```c
#define MQTT_TEST_MODE  1   // 1=测试模式(test.mosquitto.org), 0=正式模式(OneNET)
```

### OTA 升级流程

```
┌──────────┐     ┌──────────┐     ┌──────────┐     ┌──────────┐
│ ota_push │     │  MQTT    │     │  STM32   │     │  STM32   │
│  .py     │     │ Broker   │     │  (App)   │     │(Bootldr) │
└────┬─────┘     └────┬─────┘     └────┬─────┘     └────┬─────┘
     │                │                │                  │
     │ ota_start      │                │                  │
     │───────────────→│───────────────→│                  │
     │                │                │ 擦除 Download    │
     │ 等待 6s...     │                │ (232KB, ~4.5s)   │
     │                │                │                  │
     │ OTAD chunk × N │                │                  │
     │───────────────→│───────────────→│ 逐页写 Flash     │
     │                │                │ LCD 进度条       │
     │ ota_end        │                │                  │
     │───────────────→│───────────────→│ CRC32 校验       │
     │                │                │ 写 Flag 页       │
     │                │                │ NVIC_SystemReset │
     │                │                │                  │
     │                │                ┌──────────────────┘
     │                │                │ 检查 Flag
     │                │                │ CRC 校验 Download
     │                │                │ 逐页搬运 → App 区
     │                │                │ 全量回读校验
     │                │                │ 擦 Flag → 跳转
     │                │                └─→ 新固件启动 ✓
```

## MQTT 协议

### Topic 定义

| 方向 | Topic | 说明 |
|------|-------|------|
| 设备 → 平台 | `stm32/sensor` | 传感器数据上报 (JSON) |
| 平台 → 设备 | `stm32/ota` | OTA 固件数据 + LED 指令 |

### 数据上报格式 (OneNET)

```json
{
  "temp": 26,
  "humi": 58,
  "adc": 2048
}
```

### 平台指令

| 指令 | 格式 | 说明 |
|------|------|------|
| LED0 开 | `LED0 1` | 打开 LED0 (PB5) |
| LED0 关 | `LED0 0` | 关闭 LED0 |
| LED1 开 | `LED1 1` | 打开 LED1 (PE5) |
| LED1 关 | `LED1 0` | 关闭 LED1 |

### OTA 协议

| 指令 | 格式 | 说明 |
|------|------|------|
| 启动 | `{"cmd":"ota_start","size":237568,"crc32":3735928559}` | 通知设备准备升级 |
| 数据 | `[OTAD][seq:2B BE][chunk data...]` | 二进制帧，每帧 ≤ 1800 bytes |
| 结束 | `{"cmd":"ota_end"}` | 触发 CRC 校验 + 写 Flag |
| 取消 | `{"cmd":"ota_cancel"}` | 取消本次升级 |

## 关键技术点

| 技术点 | 方案 |
|--------|------|
| Flash 分区一致性 | `ota_partition.h` 单一真相源，Bootloader 和 App 共享 |
| 向量表偏移 | `SCB->VTOR = 0x0800C000` |
| CRC32 一致性 | STM32 硬件 CRC = Python 软件 CRC (CRC-32/MPEG2) |
| 掉电保护 | Flag 在全部验证通过后才擦除；逐页原子操作 |
| MQTT 不阻塞 | 独立 OTA 任务 + FreeRTOS 队列，Flash 重操作不卡 MQTT |
| TCP 流式重组 | `g_rx_pending` 保留不完整帧，循环解析 + 下次拼接 |
| JSON 空格兼容 | `strstr` 后跳过冒号空格再解析数字 |
| 队列流控 | Python 脚本 `ota_start` 后延时 6s 等设备擦除完成 |
| LCD 互斥 | `ota_get_state() != OTA_IDLE` 时 sensor 任务跳过写屏 |
| 堆安全 | `configASSERT` 导致停机；堆 20KB → 25KB |

## 文档

- [GPIO_Pinout.md](Documents/GPIO_Pinout.md) — GPIO 引脚分配表
- [MQTT_Call_Table.md](Documents/MQTT_Call_Table.md) — MQTT 协议栈回调/函数调用关系
- [OTA_Implementation_Summary.md](Documents/OTA_Implementation_Summary.md) — OTA 完整实现总结（含联调踩坑）
- [Debug_Log.md](Documents/Debug_Log.md) — 联调调试日志

## 版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| v1.1.0 | 2026-06-26 | OTA 独立任务架构重构、TCP 流式重组修复、MQTT 测试模式 |
| v1.0.0 | 2026-06-22 | OTA 完整实现 (Bootloader + 下载 + 搬运 + Python 工具) |
| — | 2026-06-19 | MQTT 协议栈完整实现 + OneNET 适配 + FreeRTOS 四任务架构 |

## 许可

Educational / Personal Project.
