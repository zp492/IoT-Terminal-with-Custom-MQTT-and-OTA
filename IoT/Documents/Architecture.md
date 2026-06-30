# IoT Terminal 项目架构图

## 一、硬件层 → 软件层

```
┌──────────────────────────────────────────────────────────────────┐
│                        硬件平台                                   │
│  STM32F103ZET6 (Cortex-M3, 72MHz, 512KB Flash, 64KB SRAM)       │
│  W5500 以太网芯片 (SPI2, 硬件 TCP/IP 栈)                          │
│  正点原子 TFT-LCD (ILI9341, FSMC NE4, 800×480)                   │
│  DHT11 温湿度传感器 (PG11)    LED0(PB5) LED1(PE5)                │
│  KEY0(PE4) KEY1(PE3) WKUP(PA0)                                   │
└──────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────┐
│                    STM32 HAL 驱动层                               │
│  RCC / GPIO / FLASH / CRC / UART / DMA / FSMC / Cortex           │
└──────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌──────────────────────────────────────────────────────────────────┐
│                    BSP 板级驱动层                                 │
│  w5500 / w5500_port / socket / wizchip_conf / spi                │
│  transport / tcp_client                                          │
│  lcd / lcdfont / led / key / dht11 / adc                         │
└──────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌───────────────────┬────────────────────┬─────────────────────────┐
│    Bootloader     │   Middlewares      │       App 层            │
│    (裸机,独立工程) │                    │    (FreeRTOS)           │
├───────────────────┼────────────────────┼─────────────────────────┤
│ bootloader.c      │ MQTT/mqtt_client   │ freertos_task           │
│  ├─ check_flag    │  ─ 报文编解码       │   ├─ sensor_task (pri2)  │
│  ├─ verify_fw     │  ─ 剩余长度         │   ├─ mqtt_task   (pri4) │
│  ├─ do_upgrade    │  ─ CONNECT/PUB/SUB  │   ├─ led_task    (pri1) │
│  ├─ clear_flag    │                     │   ├─ w5500_mon   (pri2) │
│  └─ jump_to_app   │ MQTT/mqtt_wrapper   │   └─ ota_task    (pri3) │
│                   │  ─ 连接状态机       │                         │
│ bootloader_flash  │  ─ 保活/PING        │ Onenet/onenet           │
│  ─ 页擦除/字编程   │  ─ 自动重连         │  ─ OneNET 平台适配      │
│  ─ 逐页搬运/验证    │  ─ TCP流式重组      │                         │
│                    │  ─ g_rx_pending    │ ota/ota_download        │
│ bootloader_crc      │                     │  ─ OTA 状态机           │
│  ─ 硬件 CRC32       │                     │  ─ Flash 写入/CRC/Flag  │
│                     │                     │  ─ 独立任务架构         │
│ bootloader_led      │                     │                         │
│  ─ 慢闪/快闪/SOS    │                     │ fw_info / fw_version    │
│                     │                     │  ─ 固件版本信息         │
│ bootloader_lcd      │                     │                         │
│  ─ 升级交互界面     │                     │                         │
└───────────────────┴────────────────────┴─────────────────────────┘
```

---

## 二、512KB Flash 分区布局

```
0x08000000 ┌──────────────────────────┐  ┌─────────────────────────┐
           │    Bootloader (48KB)      │  │  裸机, 出厂烧录, 永不变  │
           │    24 页 × 2KB             │  │  检查 Flag → 搬运 → 跳转 │
0x0800B000 ├──────────────────────────┤  ├─────────────────────────┤
           │  Flag 页 (2KB, 第22页)    │  │  ota_flag_t: magic       │
           │                           │  │  + fw_size + fw_crc32   │
0x0800B800 ├──────────────────────────┤  │  Bootloader 读 ← →      │
           │  保留 (6KB, 第23~24页)    │  │  App OTA 任务写          │
0x0800C000 ├──────────────────────────┤  ├─────────────────────────┤
           │                           │  │                         │
           │    App 主区 (232KB)       │  │  当前运行固件             │
           │    116 页 × 2KB            │  │  0x0800C200: fw_info_t  │
           │                           │  │  (版本信息)              │
0x08046000 ├──────────────────────────┤  ├─────────────────────────┤
           │                           │  │                         │
           │    Download 区 (232KB)    │  │  OTA 新固件暂存           │
           │    116 页 × 2KB            │  │  0x08046200: fw_info_t  │
           │                           │  │  (新版本信息)            │
0x08080000 └──────────────────────────┘  └─────────────────────────┘
```

---

## 三、FreeRTOS 任务架构 + IPC

```
   优先级
   ┌────┐
 4 │    │ mqtt_task ──xQueueReceive── g_sensor_queue
   │    │ (MQTT 收发+回调)  │
   │    │   │               │
   │    │   ├─ mqtt_on_cmd() 入队 ──xQueueSendToBack──→ g_ota_queue (4, 2050B/项)
   │    │   │
   │    │   └─ 定时 PUBLISH 温湿度 (10s)
   │    │
 3 │    │ ota_task ──xQueueReceive──→ g_ota_queue
   │    │ (平常阻塞; 激活时升最高+挂起MQTT; Flash擦写+CRC+Flag)
   │    │
 2 │    │ sensor_task ──xQueueSend──→ g_sensor_queue (3)
   │    │ (DHT11+LCD, 1s周期; OTA期间跳过LCD)
   │    │
 2 │    │ w5500_monitor ──xSemaphoreGive──→ g_net_ready_sem
   │    │ (PHY监控, 500ms)                        │
   │    │                               ┌────────┘
   │    │                               │ xSemaphoreTake
   │    │                               │ mqtt_task 等待网线插入
   │    │
 1 │    │ led_task ←── 读 g_net_state (volatile, 无 IPC)
   └────┘ (LED闪烁, 250~500ms周期)

IPC 汇总:
  g_sensor_queue    队列       sensor → mqtt (温湿度)
  g_ota_queue       队列       mqtt   → ota   (OTA数据包)
  g_net_ready_sem   信号量     w5500_mon → mqtt (链路就绪通知)
  g_net_state       volatile   mqtt_wrapper → led_task (LED状态)
```

---

## 四、OTA 升级全流程

```
┌──────────┐     MQTT      ┌──────────┐     TCP      ┌──────────────┐
│ Python   │ ───────────→  │  test.   │ ───────────→ │   STM32      │
│ ota_push │  自定义协议    │mosquitto │   以太网      │  (App 端)    │
│ .py      │               │  .org    │              │              │
└──────────┘               └──────────┘              └──────┬───────┘
      │                                                      │
      │ ① {"cmd":"ota_start","size":N,"crc32":N}            │
      │   等待 6s (设备擦除 Download 区 4.5s)                 │
      │ ② [OTAD][seq:2B BE][data...] × 35 块               │
      │ ③ {"cmd":"ota_end"}                                  │
      │                                                      ▼
      │                                          ┌───────────────────┐
      │                                          │  OTA 下载阶段      │
      │                                          │  ① 擦 Download    │
      │                                          │  ② 写 Flash       │
      │                                          │  ③ CRC32 校验     │
      │                                          │  ④ 写 Flag        │
      │                                          │  ⑤ 软复位         │
      │                                          └────────┬──────────┘
      │                                                   │ NVIC_SystemReset()
      │                                                   ▼
      │                                          ┌───────────────────┐
      │                                          │  Bootloader 阶段   │
      │                                          │  ① 读 Flag        │
      │                                          │  ② CRC32 校验 DL  │
      │                                          │  ③ LCD 版本确认   │
      │                                          │  ④ 逐页搬运(31页) │
      │                                          │  ⑤ 全量回读验证   │
      │                                          │  ⑥ 擦 Flag        │
      │                                          │  ⑦ 跳转新 App     │
      │                                          └────────┬──────────┘
      │                                                   │
      ▼                                                   ▼
  新固件 v1.1 运行 ◄────────────────────────────────────────┘
```

---

## 五、任务时序图

### 5.1 正常运行时序 (无 OTA)

```
时间轴 →

SysTick ─┬──────┬──────┬──────┬──────┬──────┬──────┬──────┬────── (1ms)
          │      │      │      │      │      │      │      │

MQTT(4)   ██████████████████████████████████████████████████████
          │ transport_recv(500ms) + PUBLISH(10s) + PING(40s)  │
          │                                                    │
sensor(2) ░░░░███░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░███░░
          │   │DHT11│ vTaskDelay(1000)  WFI休眠            │下次│
          │   │~5ms │                                       │采集│
          │   │     │                                        │   │
LED(1)    ░░░░░░░░░░███░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░
          │              │LED刷新│                          │
          │              │~1ms  │                          │
          │              │      │                          │
Idle      ██████████████████████░░░░░░░░░░░░░░░░░░░░░░█████████
          │ 短暂运行          │__WFI() 休眠               │
          │                   │ 所有任务阻塞时自动进入     │
```

### 5.2 OTA 激活时序 (从 ota_start 到软复位)

```
时间轴 (秒) →
          0         1         2         3         4         5         ...

MQTT(4)   ████████████████████████████████████████████████████████████
          │ 接收ota_start │ 被挂起 (vTaskSuspend)    │ 恢复, 收数据块     │
          │ 入队 → 返回    │                          │ xQueueSendToBack  │
          │               │                          │                   │
OTA(31)   ░░░░████████████████████████████████████████████████████████████
          │阻塞│ 接管!       │ 擦除 Download 区        │逐块写Flash │CRC+Flag│
          │队列│ 升最高优先级 │ 116页×40ms=4.5s        │每块~9ms   │→ 软复位│
          │    │ 挂起MQTT    │ MQTT暂停,CPU Stall     │MQTT恢复    │       │
          │    │             │                        │OTA最高优先级│       │
          │    │             │                        │            │       │
脚本                │ ota_start │ 等待 6 秒...         │ 发35个OTAD块│ota_end│
                    │           │                      │             │       │

          ↓ NVIC_SystemReset() ↓

Bootloader ████████████████████████████████████████████████████████████
           │ Flag检查→CRC→LCD确认→逐页搬运(31页×50ms)→回读→擦Flag→跳转 │
           │ ~2 秒                                                       │

新App      ░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░░████████████████
           │                                              │ 正常运行     │
```

### 5.3 Bootloader 逐页搬运时序 (31 页 × 50ms/页)

```
时间轴 (ms) →
          0     50    100    150    200    ...   1500   1550

Page 1   [读][擦][写][验]
             40ms 10ms <1ms

Page 2         [读][擦][写][验]

Page 3                [读][擦][写][验]
                                         ...
Page 31                                          [读][擦][写][验]

         ├─ 每页: 读(Download→RAM, <1ms) + 擦(App页, 40ms) + 写(RAM→App, 10ms) + 验(字节比对, <1ms)
         ├─ 任意时刻断电: 最多损坏当前一页, Flag 有效 → 下次上电重试
         └─ 总计: 31 × ~50ms ≈ 1.5 秒
```

---

## 六、MQTT 接收路径 (TCP 流 → MQTT 帧)

```
W5500 Socket1
  │  transport_recv() 500ms 轮询
  ▼
g_rx_buf[2048] ← g_rx_pending (残帧拼接)
  │
  ├─ 新数据追加: recv → g_rx_buf + g_rx_pending
  │
  ▼
mqtt_parse() 循环解析:
  │  ① 读固定头 data[0] → pkt_type (高4位)
  │  ② mqtt_decode_length → remaining (7bit 变长编码)
  │  ③ total_len = 1 + len_consumed + remaining
  │  ④ if len < total_len → -1 (不完整)
  │  ⑤ 按 pkt_type 分派:
  │
  ├─ PUBLISH → _cb_publish(topic, payload, payload_len)
  │              │
  │              ├─ mqtt_on_cmd()  ← freertos_task.c
  │              │   ├─ "OTAD" 魔数 → ota_handle_packet → 入 OTA 队列
  │              │   ├─ JSON "ota_start/end/cancel" → 同上
  │              │   └─ 其他 JSON → LED 控制
  │              │
  │              └─ ota_task: xQueueReceive → 写 Flash/CRC/Flag
  │
  ├─ CONNACK → _cb_connack(ret_code)
  ├─ SUBACK  → _cb_suback(pkt_id, ret_code)
  └─ PINGRESP → (静默处理)

  ⑥ if consumed == -1 → memmove 残帧到头部 → g_rx_pending = 剩余
  ⑦ if consumed <= -2 → 协议错误, 丢弃
```

---

## 七、Bootloader 主流程

```
芯片上电 (0x08000000)
  │
  ▼
Reset_Handler → SystemInit → __main → main()
  │
  bootloader_run()
  │
  ├─ init_hw()    时钟72MHz / SysTick / LED / 串口 / CRC
  │               __enable_irq()  ← 裸机必须手动开中断
  │
  ├─ check_flag() 读 OTA_FLAG->magic
  │   ├─ != 0x4F544101 → 无升级 → jump_to_app()
  │   └─ == 0x4F544101 → 继续
  │
  ├─ verify_firmware()  CRC32(Download区) == flag->fw_crc32?
  │   ├─ 不匹配 → 擦Flag → jump_to_app(旧App)  ← 回退
  │   └─ 匹配 → 继续
  │
  ├─ LCD 初始化 + 版本确认
  │   ├─ 用户按 KEY1 → 擦Flag → jump_to_app(旧App)  ← 取消
  │   └─ 用户按 KEY0 或 10s超时 → 继续
  │
  ├─ do_upgrade() 逐页搬运 (31页 × 50ms)
  │   ┌─────────────────────────────────┐
  │   │ 每页: 读源(2KB→RAM)              │
  │   │       擦目标(40ms)               │
  │   │       写目标(RAM→App区, 10ms)    │
  │   │       逐字节验证(Download==App)  │
  │   └─────────────────────────────────┘
  │
  ├─ 全量回读验证  232KB 逐字节比对
  │
  ├─ clear_flag()  擦 Flag 页 → magic=0xFFFFFFFF
  │
  └─ jump_to_app() 关中断→停SysTick→设MSP→设VTOR→跳转
                    ▼
              App 启动 (0x0800C000)
```

---

## 八、目录结构

```
IoT/
├── App/                         应用层
│   ├── freertos_task.c / .h      FreeRTOS 6任务 + IPC
│   ├── fw_info.c                 固件版本信息 (固定Flash地址)
│   ├── fw_version.h              版本号宏
│   ├── Onenet/
│   │   └── onenet.c / .h         OneNET MQTT 平台适配
│   └── ota/
│       └── ota_download.c / .h   OTA 下载模块 (独立任务)
│
├── Bootloader/                   裸机 Bootloader (独立Keil工程)
│   ├── main.c                    入口
│   ├── bootloader.c / .h         主逻辑 (Flag检查→搬运→跳转)
│   ├── bootloader_flash.c / .h   Flash 擦除/编程/验证
│   ├── bootloader_crc.c / .h     硬件 CRC32
│   ├── bootloader_led.c / .h     LED 状态机 (慢闪/快闪/SOS)
│   ├── bootloader_lcd.c / .h     LCD 交互界面
│   ├── bootloader_debug.c / .h   串口日志
│   └── bootloader.sct            分散加载文件
│
├── BSP/                          板级支持包
│   ├── Inc/  (15个头文件)
│   │   w5500, socket, wizchip_conf, transport, tcp_client
│   │   lcd, lcdfont, led, key, dht11, adc, sram, spi
│   └── Src/  (13个源文件)
│
├── Core/                         内核配置
│   ├── Inc/
│   │   FreeRTOSConfig.h          FreeRTOS 配置 (堆25KB, 优先级32)
│   │   ota_partition.h           Flash 分区 + Flag 结构 + FW信息
│   │   stm32f1xx_hal_conf.h      HAL 模块开关
│   │   stm32f1xx_it.h            中断声明
│   └── Src/
│       main.c                    App 入口 (VTOR偏移 + 外设初始化)
│       stm32f1xx_it.c            中断服务函数
│
├── Drivers/                      驱动层
│   ├── CMSIS/                    ARM Cortex-M3 标准接口
│   ├── STM32F1xx_HAL_Driver/     ST HAL 库
│   └── SYSTEM/                   正点原子系统驱动
│       ├── sys/sys.c / .h        时钟 + NVIC + 复位 + WFI
│       ├── delay/delay.c / .h    SysTick 延时
│       └── usart/usart.c / .h    串口 + printf 重定向
│
├── FreeRTOS/                     FreeRTOS 内核 (heap_4, ARM_CM3)
│
├── Middlewares/                  中间件
│   └── MQTT/                     自研 MQTT 3.1.1 协议栈
│       ├── mqtt_client.c / .h    报文编解码 + 解析状态机
│       └── mqtt_wrapper.c / .h   客户端状态机 + 连接/保活/收发
│
├── Projects/MDK-ARM/
│   ├── app.uvprojx               App Keil 工程
│   └── bootloader.uvprojx        Bootloader Keil 工程
│
├── Tools/
│   └── ota_push.py               OTA 固件推送脚本 (MQTT + CRC32)
│
├── Documents/
│   ├── OTA_Implementation_Summary.md   实现总结
│   ├── Debug_Log.md                    调试记录 (11 bugs)
│   ├── Interview_QA.md                 面试问答集
│   ├── MQTT_Call_Table.md              MQTT 调用关系
│   └── GPIO_Pinout.md                  IO 分配表
│
└── Output/                       Keil 编译产物 (.axf, .hex, .map)
```
