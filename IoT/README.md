# IoT 远程数据采集 + OTA 升级系统

> 基于 STM32F103ZET6 + W5500 + FreeRTOS + 自研 MQTT 的物联网终端，支持温湿度采集、云端指令控制和 OTA 固件远程升级。

## 演示视频

▶️ [B站观看](https://www.bilibili.com/video/BV1BKTE6iEED/)

## 核心亮点
- 自研 MQTT 3.1.1 协议栈（~500 行 C，零第三方依赖）
- 支持掉电保护的 OTA 远程升级（Flag 状态机 + 三层 CRC 校验 + 逐页原子搬运）
- FreeRTOS 5 任务架构，消息队列 + 信号量异步通信，空闲 WFI 低功耗

## 硬件平台

| 模块 | 型号 | 接口 |
|------|------|------|
| MCU | STM32F103ZET6 (512KB Flash / 64KB SRAM / 72MHz) | — |
| 以太网 | W5500 | SPI2 (PB12~PB15) |
| 液晶屏 | 正点原子 7 寸 TFT (800×480, ILI9341) | FSMC NE4 |
| 温湿度 | DHT11 | PG11 |
| 按键 | KEY0(PE4), KEY1(PE3), WKUP(PA0) | GPIO Input |
| LED | LED0(PB5), LED1(PE5) | GPIO Output (活低) |
| 调试串口 | USART1 (115200 bps) | PA9/PA10 |

## 系统架构

### Flash 分区 (512KB)

```
0x08000000 ┌──────────────────────────┐
           │    Bootloader (48KB)      │  裸机, 出厂烧录, 永不变
0x0800B000 ├──────────────────────────┤
           │  Flag 页 (2KB)            │  ota_flag_t: magic + fw_size + fw_crc32
0x0800C000 ├──────────────────────────┤
           │    App 主区 (232KB)       │  当前运行固件
           │    0x0800C200: fw_info_t  │  (版本信息)
0x08046000 ├──────────────────────────┤
           │    Download 区 (232KB)    │  OTA 新固件暂存
           │    0x08046200: fw_info_t  │  (新版本信息)
0x08080000 └──────────────────────────┘
```

### FreeRTOS 任务拓扑

```
优先级       生产者                    消费者
  4   mqtt_task ──────→ g_ota_queue ─────→ ota_task (pri=3)
      MQTT收发+回调      (队列,深度4,2KB/项)  激活时升最高+挂起MQTT
      │                                       Flash擦写+CRC+Flag
      ├─ g_sensor_queue ←── sensor_task (pri=2)
      │   (队列, 深度3)      DHT11(1s) + LCD
      │
      └─ g_net_ready_sem ←── w5500_monitor (pri=2)
          (二进制信号量)      PHY链路检测(500ms)

  1   led_task ←── g_net_state (volatile)
      LED慢闪/快闪/常亮/SOS

  Idle vApplicationIdleHook() → __WFI()  自动低功耗休眠
```

## 功能特性

### 传感器采集
- DHT11 温湿度采集，每秒采样
- 传感器数据通过 FreeRTOS 消息队列发送到 MQTT 任务
- OTA 下载期间自动暂停 LCD 刷新，避免与进度条重叠

### MQTT 通信
- **自研 MQTT 3.1.1 协议栈**（~500 行 C，无第三方依赖）
- 剩余长度编解码、CONNECT/PUBLISH(QoS0)/SUBSCRIBE/PINGREQ 报文组装与解析
- TCP 流式数据重组（`g_rx_pending` 残帧拼接 + 循环解析）
- 支持 OneNET 云平台和公共 Broker（编译开关 `MQTT_TEST_MODE` 一键切换）
- JSON 格式温湿度上报 + 平台指令下发控制 LED

### OTA 远程升级
- MQTT 通道传输固件，自定义二进制帧协议
- **掉电保护**：Flag 页状态机 + 三层校验 + 全量验证后才擦 Flag
- **逐页原子搬运**（Bootloader）：每页独立"读→擦→写→验"，断电最多损失 1 页
- **CRC32 硬件校验**：STM32 硬件 CRC 外设 (CRC-32/MPEG2)，与 Python 脚本完全一致
- **独立 OTA 任务**：MQTT 回调只做入队 (<1ms)，Flash 擦写在低优先级任务中执行
- **LCD 版本交互**：Bootloader 显示新旧版本号 + 确认对话框 + 进度条 + 结果
- **Python 下发工具**：自动解析 `.hex`/`.bin`，计算 CRC32，MQTT 分包推送
- **流控**：脚本 `ota_start` 后等待 6s，确保设备擦除完成

### 低功耗
- FreeRTOS idle hook + `__WFI()` 自动休眠
- 所有任务阻塞时 CPU 自动进入 WFI，任意中断唤醒
- 无需应用层感知

## 目录结构

```
IoT/
├── App/                         应用层
│   ├── freertos_task.c / .h      FreeRTOS 6任务 + IPC + 低功耗 Hook
│   ├── fw_info.c                 固件版本信息 (固定 Flash 地址)
│   ├── fw_version.h              版本号宏 (升级时修改此处)
│   ├── Onenet/
│   │   └── onenet.c / .h         OneNET MQTT 平台适配 (三元组/Token/Topic)
│   └── ota/
│       └── ota_download.c / .h   OTA 下载模块 (独立任务, 队列化架构)
│
├── Bootloader/                   裸机 Bootloader (独立 Keil 工程)
│   ├── main.c                    入口
│   ├── bootloader.c / .h         主流程: Flag检查→CRC校验→搬运→跳转
│   ├── bootloader_flash.c / .h   Flash 逐页擦写 + 字节验证
│   ├── bootloader_crc.c / .h     硬件 CRC32 封装
│   ├── bootloader_led.c / .h     LED 状态机 (慢闪/快闪/SOS)
│   ├── bootloader_lcd.c / .h     LCD 交互界面 (版本确认+进度条)
│   ├── bootloader_debug.c / .h   串口日志 (编译开关可控)
│   └── bootloader.sct            分散加载文件
│
├── BSP/                          板级支持包
│   ├── Inc/  (w5500, socket, transport, lcd, led, key, dht11, spi 等)
│   └── Src/  (对应驱动实现)
│
├── Core/                         内核配置
│   ├── Inc/
│   │   FreeRTOSConfig.h          FreeRTOS 配置 (堆 25KB, 优先级 32)
│   │   ota_partition.h           Flash 分区 + Flag 结构 + FW 信息 (单一真相源)
│   │   stm32f1xx_hal_conf.h      HAL 模块开关
│   └── Src/
│       main.c                    App 入口 (VTOR 偏移 + 外设初始化)
│       stm32f1xx_it.c            中断服务函数
│
├── Drivers/                      驱动层
│   ├── CMSIS/                    ARM Cortex-M3 标准接口
│   ├── STM32F1xx_HAL_Driver/     ST HAL 库
│   └── SYSTEM/                   正点原子系统驱动 (sys/delay/usart)
│
├── FreeRTOS/                     FreeRTOS 内核 (heap_4, ARM_CM3)
│
├── Middlewares/
│   └── MQTT/                     自研 MQTT 3.1.1 协议栈
│       ├── mqtt_client.c / .h    报文编解码 + 解析状态机
│       └── mqtt_wrapper.c / .h   客户端状态机 (连接/保活/自动重连/TCP流式重组)
│
├── Projects/MDK-ARM/             Keil MDK 工程文件
│   ├── app.uvprojx               App 工程 (ROM: 0x0800C000, 232KB)
│   └── bootloader.uvprojx        Bootloader 工程 (ROM: 0x08000000, 48KB)
│
├── Tools/
│   └── ota_push.py               OTA 固件推送脚本 (MQTT + CRC32)
│
├── Documents/                    项目文档
│   ├── Architecture.md           项目架构图
│   ├── OTA_Implementation_Summary.md   OTA 实现总结
│   ├── Debug_Log.md              调试记录 (11 bugs)
│   ├── Interview_QA.md           面试问答集 (23 题)
│   ├── MQTT_Call_Table.md        MQTT 调用关系
│   └── GPIO_Pinout.md            IO 分配表
│
├── Output/                       Keil 编译产物
└── README.md
```

## 快速开始

### 编译与烧录

**首次烧录（出厂）**

1. 编译 Bootloader → 烧录到 `0x08000000`
   Keil 打开 `Projects/MDK-ARM/bootloader.uvprojx` → Build → Download

2. 编译 App → 烧录到 `0x0800C000`
   Keil 打开 `Projects/MDK-ARM/app.uvprojx` → Build → Download

3. 复位，串口 (115200 bps) 观察启动日志

**后续升级（OTA）**

```bash
# 1. 修改 App/fw_version.h 版本号 → 编译 App
# 2. 推送
python Tools/ota_push.py Output/f103_zj.hex
```

### MQTT Broker 切换

编辑 `App/freertos_task.c`：

```c
#define MQTT_TEST_MODE  1   // 1=test.mosquitto.org (测试), 0=OneNET (正式)
```

## MQTT 协议

### Topic 定义

| 方向 | Topic | 说明 |
|------|-------|------|
| 设备 → 平台 | `stm32/sensor` | 传感器数据上报 (JSON, 10s 间隔) |
| 平台 → 设备 | `stm32/ota` | OTA 固件数据 + LED 指令 |

### 数据上报格式

```json
{"id":1,"dp":{"temp":[{"v":28}],"humi":[{"v":70}]}}
```

### 平台 LED 指令

```json
{"led0":1}   LED0 亮 (PB5=0)
{"led0":0}   LED0 灭
{"led1":1}   LED1 亮 (PE5=0)
{"led1":0}   LED1 灭
```

### OTA 协议

| 指令 | 格式 | 说明 |
|------|------|------|
| 启动 | `{"cmd":"ota_start","size":62548,"crc32":1334808027}` | 通知设备准备升级 |
| 数据 | `[OTAD][seq:2B BE][chunk data...]` | 二进制帧, 每帧 ≤ 1800 bytes |
| 结束 | `{"cmd":"ota_end"}` | 触发 CRC 校验 + 写 Flag + 软复位 |
| 取消 | `{"cmd":"ota_cancel"}` | 取消本次升级 |

## 关键技术点

| 技术点 | 方案 |
|--------|------|
| Flash 分区一致性 | `ota_partition.h` 单一真相源，Bootloader 和 App 共享 |
| 向量表偏移 | `SCB->VTOR = 0x0800C000`；App 和 Bootloader 各有一套向量表 |
| CRC32 一致性 | STM32 硬件 CRC = Python 软件 CRC (CRC-32/MPEG2)，多项式 0x04C11DB7 |
| 掉电保护 | Flag 在三层校验全部通过后才擦除；Bootloader 逐页原子搬运 |
| MQTT 不阻塞 | 独立 OTA 任务 + FreeRTOS 队列，Flash 操作在低优先级任务 |
| TCP 流式重组 | `g_rx_pending` 保留不完整帧，循环 `mqtt_parse` + `memmove` 拼接 |
| MQTT 载荷对齐 | `payload_len = total_len - pos` 而非 `len - pos` |
| JSON 格式兼容 | `strstr` 后跳过冒号空格再解析数字 |
| 队列流控 | Python 脚本 `ota_start` 后延时 6s 等设备擦除完成 |
| LCD 互斥 | `ota_get_state() != OTA_IDLE` 时 sensor 任务跳过写屏 |
| 堆安全 | `configASSERT` 导致停机；堆 20KB → 25KB |
| 低功耗 | idle hook + `__WFI()`，空闲期 CPU 自动休眠 |
| ARMCC5/C89 兼容 | 无 `inline`, 无 `//` 注释, 无 LL 库, 变量在块首声明 |
| Bootloader 裸机 | 无 FreeRTOS，自实现 SysTick 延时，显式 `__enable_irq()` |
| 软件定时器 | 关 (`configUSE_TIMERS=0`)，所有延时用 `vTaskDelay` |

## 文档

| 文档 | 说明 |
|------|------|
| [系统架构图](Documents/系统架构图.png) | 四层分层设计 + Bootloader 独立工程 |
| [Flash分区图](Documents/Flash分区图.png) | 512KB Flash 分区策略 |
| [任务IPC图](Documents/任务IPC图.png) | 5 任务 + 队列 + 信号量 + volatile 变量 |
| [任务时序表](Documents/任务时序表.png) | 2 秒窗口内各任务运行状态 |
| [OTA时序图](Documents/OTA时序图.png) | 从下发指令到新固件运行全流程 |
| [OTA阶段表](Documents/OTA阶段表.png) | 各阶段耗时和关键操作 |
| [MQTT数据流图](Documents/MQTT数据流图.png) | 协议栈报文解析与分发流程 |
| [Bootloader流程图](Documents/Bootloader流程图.png) | Flag检查→校验→搬运→跳转完整流程 |
| [GPIO_Pinout.md](Documents/GPIO_Pinout.md) | GPIO 引脚分配表 |
| [Debug_Log.md](Documents/Debug_Log.md) | 开发调试记录 |

## 版本历史

| 版本 | 日期 | 说明 |
|------|------|------|
| v1.1.0 | 2026-06-28 | LCD 升级交互、版本管理、低功耗、项目文档完善、删除冗余文件 |
| v1.0.0 | 2026-06-22 | OTA 完整实现 (Bootloader + 下载 + 搬运 + Python 工具 + 联调) |
| — | 2026-06-19 | MQTT 自研协议栈 + OneNET 适配 + FreeRTOS 多任务架构 |

## 许可

Educational / Personal Project.
