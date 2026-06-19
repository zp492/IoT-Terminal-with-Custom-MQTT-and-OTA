---
name: session-summary-2026-06-19
description: 本次会话全部改动总结——从W5500配置到MQTT OneNET数据成功上报
metadata:
  type: project
---

## 全部改动总结

### 一、W5500 网络层

| 文件 | 改动 |
|------|------|
| [w5500_port.c](BSP/Src/w5500_port.c) | MAC 由 UID 自动生成；IP `192.168.1.200→192.168.1.100`；SPI 速度注释修正 |
| [spi.h](BSP/Inc/spi.h) | SPI 频率注释修正为 PCLK1=36MHz 基准；W5500_INT 从 PA1→PA2 |
| [spi.c](BSP/Src/spi.c) | 频率注释修正（APB1 限制） |
| [w5500_port.h](BSP/Inc/w5500_port.h) | 无改动 |

**结论:** SPI2 挂在 APB1(PCLK1=36MHz)，最高 SCK=18MHz，W5500 数据手册要求 ≥6ns 满足。

### 二、传感器驱动

| 文件 | 改动 |
|------|------|
| [dht11.c](BSP/Src/dht11.c) | 重写：修正 checksum 失败不报错的 bug；魔法数字→具名常量；加重试机制；加 FreeRTOS 临界区保护；删 `dht11_spin_us` 死代码；修正 `dht11_read_bit` 等低→等高的逻辑 |

### 三、ADC 驱动

| 文件 | 改动 |
|------|------|
| [adc.h](BSP/Inc/adc.h) | 新接口：`adc_init()`、`adc_get_value()`、`adc_get_mv()` |
| [adc.c](BSP/Src/adc.c) | 重写：ADC1_CH1→ADC3_CH6(PF8)；DMA2_Channel5；静态 DMA 缓冲区；删 `adc_dma.h`（合并） |
| `adc_dma.c/h` | **已删除** |

### 四、MQTT 协议栈（全新）

| 文件 | 状态 | 说明 |
|------|------|------|
| `mqtt_client.h` | 🆕 | MQTT 3.1.1 报文类型、结构体、API |
| `mqtt_client.c` | 🆕 | 剩余长度编解码、5 种报文组装、解析状态机 |
| `mqtt_wrapper.h` | 🆕 | MQTT 状态机配置、入口 API |
| `mqtt_wrapper.c` | 🆕 | 连接/保活/上报/收指令 调度 |
| `transport.h` | 🆕 | 传输层抽象接口 |
| `transport.c` | 🆕 | W5500 Socket API 封装（非阻塞 connect/recv） |

### 五、OneNET 适配层（全新）

| 文件 | 状态 | 说明 |
|------|------|------|
| `onenet.h` | 🆕 | OneNET API：`onenet_set_auth`、`onenet_fill_cfg`、`onenet_build_payload` |
| `onenet.c` | 🆕 | JSON 组装（`{"id":%lu,"dp":{"temp":[{"v":%d}],...}}`）、Topic 拼装、三元组填充 |

**当前配置:** 产品 `507rVcegvD`、设备 `w5500`、服务器 `183.230.40.96:1883`、Token 认证。

### 六、FreeRTOS 任务

| 任务 | 优先级 | 栈 | 职责 |
|------|--------|-----|------|
| `start_task` | 1 | 128 | 创建子任务后自杀 |
| `led_task` | 1 | 96 | LED0 断线慢闪 / LED1 连接快闪常亮 |
| `w5500_monitor_task` | 2 | 128 | 500ms 轮询 PHY，维护信号量 |
| `sensor_task` | 3 | 512 | 1s 读 DHT11→LCD+队列 |
| `mqtt_task` | 4 | 1024 | MQTT 连接 OneNET，10s 上报 |

**内核对象:** `g_sensor_queue`（队列，深度 3）、`g_net_ready_sem`（信号量），均在 `freertos_demo()` 中创建。

**传感器数据流:** `sensor_task`→`xQueueSend`→`g_sensor_queue`→`mqtt_wrapper`→`mqtt_publish`→OneNET

### 七、tcp_client 引用清理

保留 `tcp_client.c/h` 文件，移除 `main.c`、`freertos_task.c`、`freertos_task.h` 中所有引用。

### 八、文档

| 文件 | 说明 |
|------|------|
| [GPIO_Pinout.md](Documents/GPIO_Pinout.md) | 52 引脚完整映射表，包括 ADC3 PF8 |

### 九、关键 Bug 修复

| Bug | 根因 | 修复 |
|-----|------|------|
| FreeRTOS queue.c assert 崩溃 | 堆不足(10KB)+栈小+`xQueueCreate` 在临界区 | 堆→20KB, 栈加大, 对象移到 `freertos_demo` |
| MQTT 数据 OneNET 显示 null | `mqtt_build_publish` 覆盖同 buffer 内 JSON 载荷 | 独立 `json_payload[128]` 缓冲区 |
| DHT11 读出 180%/0°C | `dht11_read_bit` 缺失"等低电平"步骤 | 恢复原逻辑：等0→等1→delay40μs→采样 |
| CONNECT/SUBSCRIBE/PINGREQ 发送失败无感知 | `transport_send` 返回值未检查 | 三处加返回值判断 |
| SUBACK 0x80 当成成功 | 未检查 SUBACK 拒绝码 | 加 `g_suback_rc==0x80` 判断 |
| snprintf 失败返回 65535 | `(uint16_t)-1` 隐式转换 | 加 `>0` 判断返回 0 |
| `total_len` uint32_t 截断 | `(uint16_t)total_len` | → `(uint32_t)len < total_len` |

### 十、FreeRTOS 配置

| 参数 | 旧 | 新 |
|------|-----|-----|
| `configTOTAL_HEAP_SIZE` | 10KB | **20KB** |
| `SENSOR_STACK_SIZE` | 256 | **512** |
| `MQTT_STACK_SIZE` | 768 | **1024** |

### 十一、记忆文件

- [[freertos-heap-semaphore-crash]] — 堆不足崩溃排查
- [[mqtt-buffer-overlap-bug]] — MQTT buffer 重叠 bug
- [[require-approval-before-edit]] — 修改前需同意

### 架构图

```
main.c
├── HAL / Clock 72MHz / delay / USART / LED / KEY
├── w5500_init() → SPI + W5500 + PHY Link
└── freertos_demo() → 创建队列+信号量 → vTaskStartScheduler()
     └─ start_task(prio1)
          ├── sensor_task(prio3)  → DHT11→LCD + xQueueSend
          ├── mqtt_task(prio4)    → transport→MQTT→OneNET PUBLISH
          ├── led_task(prio1)     → LED0/LED1 状态指示
          └── w5500_monitor(prio2)→ PHY 轮询 + 信号量
```
