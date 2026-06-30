# IoT Terminal 项目架构图

## 一、硬件层 → 软件层

```
 ┌─────────────────────────────────────────────────────────┐
 │                     硬件平台                             │
 │  STM32F103ZET6(Cortex-M3, 72MHz, 512KB Flash, 64KB SRAM)│
 │  W5500 以太网   (SPI2, 硬件 TCP/IP 栈)                   │
 │  TFT-LCD         (ILI9341, FSMC NE4, 320x240)           │
 │  DHT11 (PG11)    LED0(PB5) LED1(PE5)                    │
 │  KEY0(PE4) KEY1(PE3) WKUP(PA0)                          │
 └───────────────────────┬─────────────────────────────────┘
                         │
                         ▼
 ┌─────────────────────────────────────────────────────────┐
 │                   STM32 HAL 驱动层                       │
 │  RCC / GPIO / FLASH / CRC / UART / DMA / FSMC / Cortex  │
 └───────────────────────┬─────────────────────────────────┘
                         │
                         ▼
 ┌─────────────────────────────────────────────────────────┐
 │                   BSP 板级驱动层                         │
 │  w5500 / w5500_port / socket / wizchip_conf / spi       │
 │  transport / tcp_client / lcd / lcdfont                 │
 │  led / key / dht11 / adc                                │
 └───────────────────────┬─────────────────────────────────┘
                         │
                         ▼
 ┌──────────────────┬──────────────────┬──────────────────┐
 │   Bootloader     │   Middlewares    │     App 层       │
 │  (裸机, 独立工程) │                  │   (FreeRTOS)     │
 ├──────────────────┼──────────────────┼──────────────────┤
 │ bootloader.c     │ MQTT/mqtt_client │ freertos_task    │
 │  ├ check_flag    │  报文编解码       │  ├ sensor(pri2)   │
 │  ├ verify_fw     │  剩余长度         │  ├ mqtt  (pri4)   │
 │  ├ do_upgrade    │  CONNECT/PUB/SUB │  ├ led   (pri1)   │
 │  ├ clear_flag    │                  │  ├ w5500m(pri2)   │
 │  └ jump_to_app   │ MQTT/mqtt_wrapper│  └ ota   (pri3)   │
 │                  │  连接状态机       │                   │
 │ bootloader_flash │  保活/PING        │ Onenet/onenet     │
 │  页擦除/字编程    │  自动重连         │  OneNET 平台适配   │
 │  逐页搬运/验证    │  TCP流式重组      │                   │
 │                  │  g_rx_pending    │ ota/ota_download  │
 │ bootloader_crc   │                  │  OTA 状态机        │
 │  硬件 CRC32      │                  │  Flash写入/CRC/Flag│
 │                  │                  │  挂起MQTT+提权     │
 │ bootloader_led   │                  │                   │
 │  慢闪/快闪/SOS    │                  │ fw_info           │
 │                  │                  │ fw_version        │
 │ bootloader_lcd   │                  │  固件版本信息      │
 │  升级交互界面     │                  │                   │
 └──────────────────┴──────────────────┴──────────────────┘
```

---

## 二、512KB Flash 分区布局

```
 0x08000000 ┌──────────────────────┐
            │  Bootloader (48KB)   │  裸机, 出厂烧录, 永不变
            │  24 页 x 2KB         │  检查 Flag -> 搬运 -> 跳转
 0x0800B000 ├──────────────────────┤
            │  Flag 页 (2KB)       │  ota_flag_t:
            │  第22页              │  magic + fw_size + fw_crc32
 0x0800B800 ├──────────────────────┤
            │  保留 (6KB)          │  Bootloader 区域余量
            │  第23~24页           │
 0x0800C000 ├──────────────────────┤
            │                      │
            │  App 主区 (232KB)    │  当前运行固件
            │  116 页 x 2KB        │  @0x0800C200: fw_info_t
            │                      │  (版本信息)
 0x08046000 ├──────────────────────┤
            │                      │
            │  Download 区 (232KB) │  OTA 新固件暂存
            │  116 页 x 2KB        │  @0x08046200: fw_info_t
            │                      │  (新版本信息)
 0x08080000 └──────────────────────┘
```

---

## 三、FreeRTOS 任务架构 + IPC

```
 优先级
 ┌────┐
4│    │ mqtt_task
 │    │  ├ xQueueReceive(g_sensor_queue)  <── sensor_task 温湿度
 │    │  ├ mqtt_on_cmd() 入队 ──xQueueSendToBack──> g_ota_queue (4 x 2050B)
 │    │  └ PUBLISH 温湿度 (10s)
 │    │
3│    │ ota_task
 │    │  ├ xQueueReceive(g_ota_queue)      <── mqtt_task OTA数据
 │    │  └ 激活时: 挂起MQTT + 升最高(31) + Flash擦写+CRC+Flag
 │    │
2│    │ sensor_task                        w5500_monitor
 │    │  ├ xQueueSend(g_sensor_queue) ──>  xSemaphoreGive(g_net_ready_sem) ─┐
 │    │  └ DHT11(1s) LCD(OTA时暂停)                                          │
 │    │                                                                     │
1│    │ led_task                             xSemaphoreTake <───────────────┘
 │    │  读 g_net_state (volatile)               mqtt_task 等网线插入
 └────┘ 慢闪/快闪/常亮/SOS

 IPC 汇总:
   g_sensor_queue    队列       sensor ──> mqtt     (温湿度上报)
   g_ota_queue       队列       mqtt   ──> ota      (OTA 数据包)
   g_net_ready_sem   信号量     w5500m ──> mqtt     (链路就绪通知)
   g_net_state       volatile   mqtt_wrapper ──> led (LED 状态)
```

---

## 四、OTA 升级全流程

```
 ┌──────────┐   MQTT     ┌──────────┐   TCP    ┌──────────────┐
 │  Python  │ ────────> │  test.   │ ───────> │    STM32     │
 │ ota_push │ 自定义协议 │ mosquitto│  以太网   │   (App 端)   │
 │   .py    │           │   .org   │          │              │
 └──────────┘           └──────────┘          └──────┬───────┘
      │                                              │
      │ (1) ota_start {size, crc32}                  │
      │     等待 6s (设备擦除 4.5s)                   │
      │ (2) [OTAD][seq:2B][data] x 35 块             │
      │ (3) ota_end                                  │
      │                                              ▼
      │                               ┌──────────────────────┐
      │                               │ OTA 下载阶段           │
      │                               │  (1) 接管:挂起MQTT+提权 │
      │                               │  (2) 擦 Download(4.5s) │
      │                               │  (3) 逐块写Flash(~9ms) │
      │                               │  (4) CRC32 校验        │
      │                               │  (5) 写 Flag           │
      │                               │  (6) 软复位            │
      │                               └──────────┬───────────┘
      │                                          │ NVIC_SystemReset()
      │                                          ▼
      │                               ┌──────────────────────┐
      │                               │ Bootloader 升级阶段    │
      │                               │  (1) 读 Flag          │
      │                               │  (2) CRC32 校验 DL    │
      │                               │  (3) LCD 版本确认     │
      │                               │  (4) 逐页搬运(31页)   │
      │                               │  (5) 全量回读验证     │
      │                               │  (6) 擦 Flag          │
      │                               │  (7) 跳转新 App       │
      │                               └──────────┬───────────┘
      ▼                                          ▼
  新固件 v1.1 运行 <─────────────────────────────────┘
```

---

## 五、任务时序图

### 5.1 正常运行时序 (无 OTA, 2 秒窗口)

```
 时间:       0ms              500ms            1000ms           1500ms           2000ms
 SysTick:    ├──┬──┬──┬──┬──┬──┼──┬──┬──┬──┬──┬──┼──┬──┬──┬──┬──┬──┼──┬──┬──┬──┬──┬──┤ (1ms)

 MQTT(4)     [  transport_recv()  ][  transport_recv()  ][  transport_recv()  ]
             [  500ms 轮询收包     ][  500ms 轮询收包     ][  500ms 轮询收包     ]
             [  PUBLISH 每10s / PING 每40s                                ]

 sensor(2)   ..................[DHT11+LCD]....................[DHT11+LCD]......
             vTaskDelay(1000ms)  xQueueSend > g_sensor_queue   xQueueSend
                                 │                            │
                                 v                            v
                             MQTT 收到                     MQTT 收到
             PRI=4 > 2: MQTT 随时抢占 sensor, 网络优先

 LED(1)      .....[blink]...[blink]...[blink]...[blink]...[blink]...[blink]...
             250ms 周期, 读 g_net_state 决定闪烁模式

 Idle        [WFI][WFI].......[WFI][WFI].......[WFI][WFI].......[WFI][WFI]...
             所有任务阻塞时 CPU 自动 WFI 休眠
```

### 5.2 OTA 激活时序 (ota_start -> 新固件启动, 全时间线)

```
 时间:       0s      1s      2s      3s      4s      5s      6s      7s  ...

            │ 阶段A: 接管            │ 阶段B: 接收                   │ 阶段C│

 脚本:      ota_start               等待 6 秒...          35个OTAD块 ota_end
            {size:N, crc32:N}       (设备擦除中)          每50ms/块
            │                                               │
            v (MQTT 入队 g_ota_queue)                       │
            │                                               │
 MQTT(4)    [入队]──── SUSPEND ──── vTaskSuspend ────[恢复运行]──[收包]──[收包]──
            │             .                          .         │入队│    │入队
            │             .                          .         │    │    │
 OTA(31)    ==Queue阻塞==[接管]======================[接收循环]=======[CRC+Flag]==
                          │                          │                │
                          ota_takeover():            逐块处理:        3s后:
                          1.vTaskSuspend(mqtt)       写Flash(~9ms)   NVIC_SystemReset
                          2.vTaskPrioritySet(31)     LCD进度条更新    │
                          3.擦除Download(4.5s)       保持PRI=31      v
                          4.ota_resume_mqtt()                      Bootloader
                            MQTT恢复,OTA保持31                        │
                                                                    新App启动
 sensor(2)  [ LCD暂停 ]──────── ota_get_state()!=IDLE ──────────[ LCD恢复 ]
```

### 5.3 OTA 单帧微观时序 (MQTT 收包 -> OTA 写 Flash, 一轮交互)

```
 mqtt_task(PRI=4)                              ota_task(PRI=31)
 ════════════════                              ════════════════
     │                                              │
     │ transport_recv() -> mqtt_parse()            │ xQueueReceive() 阻塞
     │ _cb_publish("stm32/ota", OTAD...,1806)      │
     │ mqtt_on_cmd(payload, 1806):                 │
     │   ota_handle_packet():                      │
     │     memcpy -> queue_item                    │
     │     xQueueSendToBack(g_ota_queue) ────────> │ 唤醒! (PRI=31>4, 抢占MQTT)
     │     return (< 1ms)                          │
     │                                             ├ ota_process_data_chunk()
     │  (MQTT 暂时挂起)                             │  ota_write_chunk_to_flash(~9ms)
     │                                             │  LCD 进度条 45%
     │                                             └ xQueueReceive() 阻塞
     │  transport_recv() -> 收下一个包 -> ...       │  MQTT 恢复运行
     │  (下一轮)                                   │  (等待队列)
```

### 5.4 Bootloader 逐页搬运时序 (31 页 x ~50ms/页, 流水线)

```
 Page 1:  [读:DL>RAM] -> [擦:App页,40ms] -> [写:RAM>App,10ms] -> [验:字节比对]
 Page 2:               [读:DL>RAM] -> [擦:App页,40ms] -> [写:RAM>App,10ms] -> [验]
 Page 3:                              [读:DL>RAM] -> [擦:App页,40ms] -> [写] -> ...
 ...
 Page31:                                                                      [验]

 每页耗时: 读(<1ms) + 擦(40ms) + 写(10ms) + 验(<1ms) = ~50ms
 总耗时:   31 x 50ms = ~1.5s

 断电安全: 任意时刻断电最多损坏当前一页, Flag 有效 -> 下次上电重试 (幂等)
```

---

## 六、MQTT 接收路径 (TCP 流 -> MQTT 帧)

```
 W5500 Socket1
   │  transport_recv() 500ms 轮询
   ▼
 g_rx_buf[2048]  (g_rx_pending 残帧拼接)
   │
   ├ 新数据追加: recv -> g_rx_buf + g_rx_pending
   │
   ▼
 mqtt_parse() 循环解析:
   │
   (1) 读 data[0] -> pkt_type (高4位)
   (2) mqtt_decode_length -> remaining (7bit 变长编码)
   (3) total_len = 1 + len_consumed + remaining
   (4) if len < total_len -> return -1 (不完整)
   (5) 按 pkt_type 分派:
   │
   ├ PUBLISH -> _cb_publish(topic, payload, payload_len)
   │             │
   │             ├ mqtt_on_cmd()  <-- freertos_task.c
   │             │   ├ "OTAD" -> ota_handle_packet -> 入OTA队列
   │             │   ├ JSON "ota_start/end/cancel" -> 同上
   │             │   └ 其他 JSON -> LED 控制
   │             │
   │             └ ota_task: xQueueReceive -> 写Flash/CRC/Flag
   │
   ├ CONNACK  -> _cb_connack(ret_code)
   ├ SUBACK   -> _cb_suback(pkt_id, ret_code)
   └ PINGRESP -> (静默处理)

   (6) consumed == -1 -> memmove 残帧到头部 -> g_rx_pending = 剩余
   (7) consumed <= -2 -> 协议错误, 丢弃
```

---

## 七、Bootloader 主流程

```
 芯片上电 (0x08000000)
   │
   ▼
 Reset_Handler -> SystemInit -> __main -> main()
   │
   bootloader_run()
   │
   ├ init_hw()    时钟72MHz / SysTick / LED / 串口 / CRC
   │              __enable_irq()  <-- 裸机必须手动开中断
   │
   ├ check_flag() 读 OTA_FLAG->magic
   │   ├ != 0x4F544101 -> 无升级 -> jump_to_app()
   │   └ == 0x4F544101 -> 继续
   │
   ├ verify_firmware()  CRC32(Download区) == flag->fw_crc32?
   │   ├ 不匹配 -> 擦Flag -> jump_to_app(旧App)  <-- 回退
   │   └ 匹配 -> 继续
   │
   ├ LCD 初始化 + 版本确认
   │   ├ KEY1 -> 擦Flag -> jump_to_app(旧App)     <-- 取消
   │   └ KEY0 或 10s超时 -> 继续
   │
   ├ do_upgrade() 逐页搬运 (31页 x 50ms)
   │   ┌──────────────────────────────────────┐
   │   │ 每页: 读源(2KB>RAM)                    │
   │   │        擦目标(40ms)                     │
   │   │        写目标(RAM>App区, 10ms)          │
   │   │        逐字节验证(Download==App)        │
   │   └──────────────────────────────────────┘
   │
   ├ 全量回读验证  232KB 逐字节比对
   │
   ├ clear_flag()  擦 Flag 页 -> magic=0xFFFFFFFF
   │
   └ jump_to_app() 关中断 > 停SysTick > 设MSP > 设VTOR > 跳转
                    ▼
              App 启动 (0x0800C000)
```

---

## 八、目录结构

```
 IoT/
 ├── App/                           应用层
 │   ├── freertos_task.c / .h        FreeRTOS 6任务 + IPC + 低功耗Hook
 │   ├── fw_info.c                   固件版本信息 (固定Flash地址)
 │   ├── fw_version.h                版本号宏 (升级时修改此处)
 │   ├── Onenet/
 │   │   └── onenet.c / .h           OneNET MQTT 平台适配
 │   └── ota/
 │       └── ota_download.c / .h     OTA 下载模块 (队列化+挂起MQTT+提权)
 │
 ├── Bootloader/                     裸机 Bootloader (独立Keil工程)
 │   ├── main.c                      入口
 │   ├── bootloader.c / .h           主逻辑: Flag检查>CRC校验>搬运>跳转
 │   ├── bootloader_flash.c / .h     Flash 逐页擦写 + 字节验证
 │   ├── bootloader_crc.c / .h       硬件 CRC32 封装
 │   ├── bootloader_led.c / .h       LED 状态机 (慢闪/快闪/SOS)
 │   ├── bootloader_lcd.c / .h       LCD 交互界面 (版本确认+进度条)
 │   ├── bootloader_debug.c / .h     串口日志 (编译开关可控)
 │   └── bootloader.sct              分散加载文件
 │
 ├── BSP/                            板级支持包
 │   ├── Inc/  (w5500, socket, transport, lcd, led, key, dht11, spi 等)
 │   └── Src/  (对应驱动实现)
 │
 ├── Core/                           内核配置
 │   ├── Inc/
 │   │   FreeRTOSConfig.h            FreeRTOS 配置 (堆25KB, 优先级32)
 │   │   ota_partition.h             Flash分区+Flag结构+FW信息 (单一真相源)
 │   │   stm32f1xx_hal_conf.h        HAL 模块开关
 │   └── Src/
 │       main.c                      App入口 (VTOR偏移+外设初始化)
 │       stm32f1xx_it.c              中断服务函数
 │
 ├── Drivers/                        驱动层
 │   ├── CMSIS/                      ARM Cortex-M3 标准接口
 │   ├── STM32F1xx_HAL_Driver/       ST HAL 库
 │   └── SYSTEM/                     正点原子系统驱动 (sys/delay/usart)
 │
 ├── FreeRTOS/                       FreeRTOS 内核 (heap_4, ARM_CM3 port)
 │
 ├── Middlewares/
 │   └── MQTT/                       自研 MQTT 3.1.1 协议栈
 │       ├── mqtt_client.c / .h      报文编解码 + 解析状态机
 │       └── mqtt_wrapper.c / .h     客户端状态机 (连接/保活/TCP流式重组)
 │
 ├── Projects/MDK-ARM/               Keil MDK 工程文件
 │   ├── app.uvprojx                 App 工程 (ROM: 0x0800C000, 232KB)
 │   └── bootloader.uvprojx          Bootloader 工程 (ROM: 0x08000000, 48KB)
 │
 ├── Tools/
 │   └── ota_push.py                 OTA 固件推送脚本 (MQTT + CRC32)
 │
 ├── Documents/                      项目文档
 │   ├── Architecture.md             项目架构图 (本文件)
 │   ├── OTA_Implementation_Summary.md    OTA 实现总结
 │   ├── Debug_Log.md                调试记录 (11 bugs)
 │   ├── Interview_QA.txt            面试问答集 (23 题)
 │   ├── MQTT_Call_Table.md          MQTT 调用关系表
 │   └── GPIO_Pinout.md              IO 分配表
 │
 ├── Output/                         Keil 编译产物
 └── README.md
```
