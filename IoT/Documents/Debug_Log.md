# IoT 项目总调试记录

> 测试平台：STM32F103ZET6 + W5500 + 正点原子 800×480 TFT-LCD (ILI9341, FSMC)
>
> 覆盖范围：FreeRTOS 基础架构 → MQTT 通信 → Bootloader 裸机 → OTA 联调

---

## 阶段一：FreeRTOS 基础架构

### Bug 1.1：FreeRTOS 堆不足导致信号量崩溃

#### 现象

串口打印 `Error: ..\..\FreeRTOS\queue.c, 1666` + `Error: ..\..\FreeRTOS\queue.c, 1670`，程序卡死。

#### 根因

`queue.c` 第 1666/1670 行在 `xQueueSemaphoreTake()` 中：

```c
configASSERT( pxQueue );                    // line 1666: 句柄为 NULL
configASSERT( pxQueue->uxItemSize == 0 );   // line 1670: 不是信号量 (item size ≠ 0)
```

**调用链分析**：

1. `xSemaphoreCreateBinary()` 返回 NULL（FreeRTOS 堆耗尽，无法分配信号量控制块）
2. 后续 `xSemaphoreTake(NULL)` 触发 configASSERT

**深层原因**：

- `configTOTAL_HEAP_SIZE = 10KB` — 不足
- 传感器任务栈 256 words (1KB)、MQTT 任务栈 768 words (3KB) — 偏小，栈溢出写坏相邻内存
- 被破坏的内存恰好是信号量控制块的 `uxItemSize` 字段，导致误判"不是信号量"

#### 修复

```c
// FreeRTOSConfig.h
configTOTAL_HEAP_SIZE:  10KB → 20KB

// freertos_task.c
SENSOR_STACK_SIZE:      256  → 512 words
MQTT_STACK_SIZE:        768  → 1024 words
```

**附加规则**：所有内核对象（队列、信号量）必须在 `vTaskStartScheduler()` 之前创建，不能放在任务内部或临界区内。已创建的句柄也要在调度器启动前验证（`configASSERT(x != NULL)`）。

#### 教训

- `configASSERT` 的行号直接指向 FreeRTOS 内核源码，阅读对应函数即可定位
- 栈溢出是最隐蔽的 bug——症状出现在被破坏的"邻居"内存上，而非溢出的任务本身
- 堆不足和栈溢出往往同时出现，先扩大堆再逐个调栈

---

## 阶段二：MQTT 通信

### Bug 2.1：MQTT 数据上报 OneNET 无数据 — 缓冲区重叠

#### 现象

W5500 通过 MQTT 连接 OneNET 成功（`connack=0`），PUBLISH 报 `sent 90/90 bytes`，但平台始终显示 null 无数据。MQTT.fx 桌面客户端用完全相同的 Topic 和 JSON 格式发送却有数据。

#### 排查过程

1. 尝试多种 JSON 格式：`{"temp":24,"humi":67}`、`{"id":123,"dp":{...}}`、`{"params":{...}}` — 均无效
2. 尝试不同 Topic：`dp/post/json`、`thing/property/post` — 均无效
3. 尝试 OneNET 数据流协议、OneJSON 协议 — 均无效
4. 更换产品、设备、Token 多次 — 均无效
5. MQTT.fx 桌面客户端用完全相同格式发 → 平台有数据

#### 根因

`mqtt_wrapper.c` 的 PUBLISH 段存在 **输出缓冲区和输入缓冲区重叠**的问题：

```c
// 步骤 1: 将 JSON 写到 g_tx_buf 开头
json_len = cfg->build_payload(g_tx_buf, MQTT_SEND_BUF_SIZE, ...);
// g_tx_buf = {"id":1,"dp":{"temp":[{"v":27}],...}}

// 步骤 2: mqtt_build_publish 也往 g_tx_buf 开头写固定头 + Topic
len = mqtt_build_publish(g_tx_buf, topic, g_tx_buf, json_len);
//                    ^^^^^^^^         ^^^^^^^^
//                 输出 = g_tx_buf   输入也 = g_tx_buf  ← 重叠！
```

`mqtt_build_publish` 从 `buf[0]` 开始写 MQTT 固定头（2~5 字节）+ Topic（2+len 字节），这直接覆盖了之前写在同一位置的 JSON payload。后续 `memcpy(p, payload, payload_len)` 虽然试图从原始 `g_tx_buf` 读，但前 50+ 字节已被覆盖为 MQTT 协议头。

**实际上发送到 OneNET 的是：MQTT 固定头 + Topic + 被截断的 JSON 后半段 + 越界内存垃圾。**

串口 `printf("[MQTT] pub: %s", g_tx_buf)` 在 `mqtt_build_publish` 之前执行，所以 **打印的是完整 JSON，但发出的已不是**。这是典型的"printf 调试盲区"。

#### 修复

用独立缓冲区存放 JSON payload，与 MQTT 协议组装输出分离：

```c
// 用独立缓冲区存放 JSON payload
uint8_t json_payload[128];
int32_t json_len = cfg->build_payload(json_payload, sizeof(json_payload), ...);

// mqtt_build_publish 输出到 g_tx_buf，payload 从 json_payload 读，不再冲突
len = mqtt_build_publish(g_tx_buf, topic, json_payload, (uint16_t)json_len);
```

#### 教训

1. **永远不要让输出 buffer 和输入 buffer 重叠**——尤其是同一个函数同时充当生产者和消费者时
2. **printf 调试有盲区**——打印的是操作前的值，操作本身可能破坏数据
3. **MQTT.fx 对比测试是有效的隔离手段**——能证明端侧格式正确，把问题范围缩小到代码实现
4. **OneNET 平台不给错误反馈**——消息格式不对时服务器静默丢弃，不会返回任何错误信息

---

## 阶段三：Bootloader 裸机

### Bug 3.1：LCD 初始化导致 App 启动失败

#### 现象

Bootloader 上电后串口输出正常，执行到 `jump_to_app()`，但 App 没有任何输出，LCD 不亮，设备无响应。

```
[BL] No upgrade flag, booting App...
[BL] Preparing to jump to App...
[BL]   App MSP  = 0x20006798
[BL]   App Entry= 0x0800C2F1
（之后无任何输出，App 未启动）
```

如果注释掉 `bl_lcd_init()`，Bootloader 能正常跳转，App 正常启动。

#### 根因

`bl_lcd_init()` 原本放在 `init_hw()` 中，每次上电都调用，不论是否需要升级。

`lcd_init()` 会初始化 FSMC（Flexible Static Memory Controller）总线和大量 GPIO：

```
FSMC Bank 1 NE4:  PG12 (CS), PG0 (RS)
FSMC 数据总线:    PD14, PD15, PD0, PD1, PE7~PE15 (16 位)
FSMC 控制信号:    PD4 (RD), PD5 (WR)
LCD 背光:         PB0
```

这些 GPIO 被配置为 FSMC 复用功能后，如果没有在跳转 App 前恢复为默认状态，App 启动时重新初始化这些外设会产生冲突。此外，Bootloader 代码中包含了 `stm32f1xx_hal_fsmc.c`，但 FSMC 的时钟在 Bootloader 最小化 HAL 配置中没有被使能，直接调用 `lcd_init()` 可能触发 HardFault。

更深层的原因：Bootloader 的职责是"检查升级 → 搬运固件 → 跳转"，它不应该触碰 App 需要的外设。LCD 只在需要与用户交互时才需要。

#### 修复

**将 LCD/FSMC 初始化从 `init_hw()` 延迟到真正需要时才执行**——即 CRC 校验通过、确认存在有效升级标志之后。

```c
// init_hw() 中删除：
// bl_lcd_init();

// 在检测到有效升级标志后才初始化：
if (CRC 校验通过) {
    __HAL_RCC_FSMC_CLK_ENABLE();   // 手动使能 FSMC 时钟
    bl_lcd_init();                  // 初始化 LCD
    bl_lcd_show_new_firmware(...);  // 显示版本确认画面
}
```

**原则**：无升级时，Bootloader 不触碰任何 App 外设。跳转前硬件状态尽可能接近芯片复位状态。

#### 教训

- Bootloader 是"过渡者"而非"使用者"——不要初始化任何 App 会用到但 Bootloader 自身不需要的外设
- 如果确实需要（如 LCD 显示升级进度），用完确保在跳转前恢复或确保 App 能容忍

---

### Bug 3.2：SysTick LOAD 寄存器溢出

#### 现象

Bootloader 在 `w5500_hardware_reset()` 中调用 `delay_us(600)` 后卡死，串口输出停在：

```
[BL]   Resetting W5500...
[BL]     W5500: step4 RST low, before delay
（之后无输出，卡死）
```

#### 根因

Bootloader 为了不依赖 FreeRTOS 而自实现了 SysTick 延时函数 `bl_delay_init()`。调用时传参使用了错误的单位：

```c
// 错误：传入 MHz 数值，函数内部按 Hz 计算
bl_delay_init(72);   // 本意是 72MHz，但函数内当作 72Hz
```

函数内部：

```c
static void bl_delay_init(uint32_t sysclk)
{
    g_fac_us = sysclk / 8;                       // = 72 / 8 = 9
    SysTick->LOAD = (sysclk / 8) / 1000 - 1;     // = 9 / 1000 - 1 = 0xFFFFFFFF !!!
}
```

`(72 / 8) / 1000 = 0`，减 1 后发生 **32 位无符号整数下溢**，`SysTick->LOAD = 0xFFFFFFFF`。

SysTick 是 24 位递减计数器（Cortex-M3 限制）。LOAD 写入 0xFFFFFFFF 后实际截断为 `0xFFFFFF`（16777215），从该值倒数到 0 在 9MHz 时钟下约需 1.86 秒。但更关键的是，旧的 `bl_delay_us` 轮询逻辑依赖 SysTick_VAL 的倒数特性计算已流逝滴答数，LOAD 的值异常导致流逝时间计算完全错误，循环几乎不会终止。

连带注意：`g_fac_us = 9`（意外正确，因为 72/8=9 和 72000000/8/1000000=9 恰好相同），但这个巧合掩盖了真实问题。

#### 修复

将参数改为 Hz，并修正 `g_fac_us` 的含义为"每微秒 SysTick 滴答数"：

```c
// 正确：传入 Hz
bl_delay_init(SystemCoreClock);  // SystemCoreClock = 72000000
```

函数内部：

```c
static void bl_delay_init(uint32_t sysclk)
{
    // sysclk = 72000000
    // SysTick 时钟 = HCLK/8 = 72000000/8 = 9000000 Hz
    // 每微秒滴答数 = 9000000 / 1000000 = 9
    g_fac_us = (sysclk / 8) / 1000000;   // = 9
    // 1ms 重载值 = 9000000 / 1000 - 1 = 8999
    SysTick->LOAD = (sysclk / 8) / 1000 - 1;  // = 8999
}
```

同时将 `bl_delay_us` 改为简单的 NOP 忙等循环，避免依赖 SysTick 计数器边界条件：

```c
static void bl_delay_us(uint32_t nus)
{
    uint32_t i;
    for (i = 0; i < nus * 9; i++) {
        __NOP();
    }
}
```

#### 教训

- 延时函数的参数单位必须在注释和命名中明确标注
- `delay_init(72)` 在正点原子例程中约定 `72` 表示 72MHz，但自实现的函数没有兼容这个隐式约定
- 移植代码时，不要假设参数单位与原实现相同

---

### Bug 3.3：裸机环境未开启全局中断

#### 现象

Bug 3.2 修复后（`delay_us` 改为 NOP 忙等，正常工作），但 `delay_ms(10)` 仍然卡死：

```
[BL]     W5500: step6 RST high, before 10ms
（之后无输出，卡死）
```

#### 根因

`bl_delay_ms()` 依赖 SysTick 中断递增 `g_bl_tick`：

```c
static void bl_delay_ms(uint32_t nms)
{
    uint32_t target = g_bl_tick + nms;
    while (g_bl_tick < target) {}   // 等 SysTick_Handler 递增 g_bl_tick
}

void SysTick_Handler(void)
{
    g_bl_tick++;
    HAL_IncTick();
}
```

在裸机环境中，虽然 SysTick 被正确配置（CTRL.ENABLE=1, CTRL.TICKINT=1, LOAD=8999），但 **全局中断从未被开启**。

在有 FreeRTOS 的环境中，`vTaskStartScheduler()` 内部会调用 `__enable_irq()` 开启全局中断。Bootloader 是裸机，没有这一步：

- SysTick 计数器正常工作，倒计时到 0 后置位 COUNTFLAG
- 但中断不被响应（在 ARM 架构中具体取决于 PRIMASK/FAULTMASK 状态）
- `SysTick_Handler` 从未被调用
- `g_bl_tick` 永远为 0 → `bl_delay_ms(10)` 死循环

**连带影响**：`HAL_IncTick()` 也从未被调用，所有依赖 `HAL_GetTick()` 的超时检测（`HAL_FLASHEx_Erase`、`HAL_FLASH_Program` 等 Flash 操作）都会永久超时。这意味着即便 Bootloader 进入到升级流程，Flash 擦写也会在第一步就卡死。

#### 修复

在 `init_hw()` 末尾显式开启全局中断：

```c
static void init_hw(void)
{
    HAL_Init();
    sys_stm32_clock_init(RCC_PLL_MUL9);
    bl_delay_init(SystemCoreClock);
    bl_led_init();
    bootloader_set_state(BL_STATE_INIT);
    bl_debug_init();
    bl_crc32_init();

    /* 裸机环境: 手动开全局中断 */
    __enable_irq();
}
```

同时修正了 SysTick 时钟源被覆写的问题——原来的代码中 `CTRL = CLKSOURCE_Msk | ...` 把时钟源设为 HCLK（覆盖了 `HAL_SYSTICK_CLKSourceConfig` 的 HCLK/8 设置）：

```c
// 修复前：
SysTick->CTRL = SysTick_CTRL_CLKSOURCE_Msk | SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
// CLKSOURCE=1 → HCLK (72MHz), 但 LOAD 按 HCLK/8 (9MHz) 计算 → 周期 125μs 而非 1ms

// 修复后：
SysTick->CTRL = SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
// CLKSOURCE=0 → HCLK/8 (9MHz), 与 LOAD 的计算一致 → 精确 1ms
```

#### 教训

- **裸机程序必须在硬件初始化完成后显式 `__enable_irq()`**
- FreeRTOS 的 `vTaskStartScheduler()` 替开发者做了这一步，从 RTOS 环境切到裸机环境时这是最容易被遗忘的差异
- 不仅是延时，所有依赖中断的功能（Flash 超时、UART 接收等）都会受影响

---

## 阶段四：OTA 联调

### Bug 4.1：MQTT 载荷长度计算错误 — 包含下一个包的数据

#### 现象

OTA 数据持续写入但 `g_fw_received` 增长快于预期，最终触发 `overflow: 61980 + 2028 > 62488`，OTA 中断为 SIZE_MISMATCH。

#### 根因

`mqtt_client.c` PUBLISH 解析器中，`payload_len` 使用总接收长度 `len` 而非当前包预期长度 `total_len`：

```c
// Bug: len 可能包含 TCP 缓冲区中下一个 MQTT 包的数据
payload_len = (uint16_t)((uint32_t)len - pos);   // 错误

// 正确: 只用当前 PUBLISH 包内的剩余字节
payload_len = (uint16_t)(total_len - pos);
```

`total_len = 1 + len_consumed + remaining` 是当前 MQTT 包的完整长度。当 W5500 一次 `recv` 读到多个 MQTT 包时，`len` > `total_len`，多余字节被当作当前包载荷写入 Flash → 数据膨胀 → 溢出。

#### 修复

```c
payload_len = (uint16_t)(total_len - pos);   // 限制在当前包范围内
```

#### 教训

TCP 是流式协议，应用层必须自己切分报文边界。`len` 是"这次读到了多少"，`total_len` 是"当前报文有多长"——两者不是一回事。

---

### Bug 4.2：JSON 解析器不处理冒号后的空格

#### 现象

`ota_start` JSON `{"cmd":"ota_start","size":62492}` 解析结果 `parsed_size = 0`，触发 `OTA_ERR_INVALID_CMD`。

#### 根因

`strstr` 定位 `"size":` 后指针前进 7 字节（`"size":` 长度），但 `paho-mqtt` 生成的 JSON 在冒号后有空格：`"size": 62492`。空格不是数字，`while (*s >= '0' ...)` 一次都不执行，`parsed_size` 保持初始值 0。

同样的问题也存在于 `"crc32":` 解析。

```c
// Bug: 直接跳过 key 长度, 没考虑 ": " 中的空格
s = strstr(payload, "\"size\":");
s += 7;            // 跳过 `"size":`
// s 现在指向 ' ', 不是数字!
while (*s >= '0' && *s <= '9') { ... }   // 循环体从未执行
```

#### 修复

跳过 key 后主动越过空格：

```c
s += 7;                              // 跳过 `"size":`
while (*s == ' ' || *s == '\t') s++; // 跳过空格
while (*s >= '0' && *s <= '9') { ... }
```

#### 教训

解析 JSON 不能假设格式——标准 JSON 允许 key 和 value 之间有任意空白。简单的 `strstr` + 固定偏移在企业场景不够健壮，生产代码应使用正式 JSON 解析库。

---

### Bug 4.3：W5500 `getsockopt(SO_RECVBUF)` 在特定条件下返回 0

#### 现象

主循环中 `getsockopt(sn, SO_RECVBUF, &rx_size)` 始终返回 0，但 W5500 内部确有数据（`getSn_RX_RSR(sn)` 直接读寄存器返回非零）。

#### 根因

`getsockopt` 和 `recv` 在 WIZnet 库中是配对使用的。`recv` 内部调用 `getSn_RX_RSR` 并更新读指针 `Sn_RX_RD`。如果在 `getsockopt` 之前直接调用 `recv`，读指针可能未正确更新，导致后续 `getsockopt` 读取的值异常。

#### 修复

保持原有的 `getsockopt → recv` 配对顺序，同时增加 `getSn_RX_RSR` 直接读取作为回退。缩短轮询间隔（50ms）也有帮助。

#### 教训

芯片厂商的 socket API 有隐含调用顺序约束——`getsockopt` → `recv` 是 WIZnet 库的契约，违反会导致数据通路失效。

---

### Bug 4.4：OTA 队列满 — 擦除期间数据块涌入

#### 现象

首次运行时 `[OTA] WARN: queue full, dropping packet` 持续出现，所有 OTA 包丢失。`ota_start` 本身也被丢弃。

#### 根因

OTA 任务收到 `ota_start` 后需擦除 Download 区（232KB，约 4.5 秒）。此期间 MQTT 任务持续接收数据块并入队，队列仅有 4 个槽位，瞬间填满。擦除完成后 OTA 任务能快速消费（~10ms/块），但已丢失的块无法恢复。

#### 修复

脚本侧 `ota_start` 发布后等待 6 秒再发送数据块，确保设备擦除完毕进入接收状态。

```python
client.publish(PUB_TOPIC, start_msg, qos=1).wait_for_publish()
time.sleep(6)  # 等待设备擦除 Download 区
```

#### 教训

异步任务之间必须考虑生产者速率与消费者初始延迟的匹配。队列缓冲的是短期抖动，不能替代端到端流控。更完善的方案是让设备在擦除完成后主动上报 "ready"，服务端再开始下发数据。

---

### Bug 4.5：FreeRTOS 堆不足导致 OTA 队列创建失败（静默）

#### 现象

将 `OTA_QUEUE_LEN` 从 4 增加到 8 后，设备上电直接卡死——App 无任何输出，无 LCD 显示。

#### 根因

`configASSERT` 仅定义为 `printf` + 继续执行，不会停止 CPU。`xQueueCreate` 返回 NULL，后续 `xQueueSendToBack(NULL, ...)` 访问空指针导致 HardFault。

计算：`8 × (2 + 2048) = 16.4KB` 仅队列存储，加上任务栈 ~12KB，总计 ~28KB，远超 `configTOTAL_HEAP_SIZE = 20KB`。

#### 修复

堆从 20KB 增至 25KB，队列保持 4 深度（够用，因脚本已加 6 秒延迟）。

```c
#define configTOTAL_HEAP_SIZE   ((size_t)(25 * 1024))
```

#### 教训

`configASSERT` 默认实现不是 panic！FreeRTOS 的 `configASSERT` 需要用户自己定义，如果只打印不停止，内存分配失败会变成难以调试的静默崩溃。生产代码应该让 `vAssertCalled` 进入死循环或触发 HardFault。

---

### Bug 4.6：TCP 流式重组 — `recv` 返回的字节与 MQTT 包不对齐

#### 现象

MQTT 数据到达速率很高时，一次 `recv` 可能返回多个 PUBLISH 包的部分数据。旧的解析逻辑只调用一次 `mqtt_parse`，剩余数据被丢弃（下次 `recv` 覆盖 `g_rx_buf`）。

#### 根因

MQTT over TCP 是流式协议，`recv` 返回的字节边界与 MQTT 包边界无关。原代码假设每次 `recv` 恰好返回一个完整的 MQTT 包。

#### 修复

引入 `g_rx_pending` 变量保存未解析完的字节。每次 `recv` 追加到已有数据后，循环 `mqtt_parse` 直到所有包被消费或遇到不完整包。不完整包的数据保留到下次 `recv` 拼接。

```c
static uint16_t g_rx_pending = 0;  // 上次未解析完的字节数

// recv 追加到 pending 数据后
rx_ret = transport_recv(..., g_rx_buf + g_rx_pending,
                        MQTT_RECV_BUF_SIZE - g_rx_pending, ...);
total = g_rx_pending + rx_ret;

// 循环解析
while (pos < total) {
    consumed = mqtt_parse(g_rx_buf + pos, total - pos, ...);
    if (consumed > 0) pos += consumed;
    else if (consumed == -1) {
        // 不完整 → 保留到下次
        memmove(g_rx_buf, g_rx_buf + pos, total - pos);
        g_rx_pending = total - pos;
        break;
    }
}
```

#### 教训

TCP 应用层必须自己处理分包/粘包。每次 `recv` 后应循环解析直到数据耗尽或遇到不完整帧，不完整部分必须保留到下次拼接。

---

### Bug 4.7：LCD 显示重叠 — sensor_task 与 OTA 任务竞争

#### 现象

OTA 下载期间，LCD 上温度读数与 "Download Complete!" 文字互相覆盖。

#### 根因

`sensor_task`（优先级 4）和 `ota_task`（优先级 2）同时向 LCD 写入。sensor 任务每 1 秒刷新温度显示，与 OTA 进度条重叠。

#### 修复

`sensor_task` 中，当 `ota_get_state() != OTA_IDLE` 时跳过 LCD 写入。OTA 期间 LCD 由 OTA 模块独占。

```c
if (ota_get_state() == OTA_IDLE) {
    lcd_show_string(..., "Temp: %d C", temp);
}
```

#### 教训

共享硬件资源（LCD、串口等）必须有互斥机制。最简单的方案是让低优先级使用者检查高优先级使用者的状态，主动避让。

---

## 总结

| 阶段 | Bug | 现象 | 根因类别 | 核心教训 |
|------|-----|------|----------|----------|
| 1 | FreeRTOS 堆不足 | 信号量创建失败，程序卡死 | 资源配置不足 | 堆和栈分配要留裕量；栈溢出症状出现在邻居内存 |
| 2 | MQTT 缓冲区重叠 | OneNET 显示 null，无数据 | 缓冲区管理 | 不要让输出 buffer 和输入 buffer 重叠；printf 调试有盲区 |
| 3.1 | LCD 抢占 | Bootloader 跳转后 App 不启动 | 外设冲突 | Bootloader 只触碰自己需要的外设 |
| 3.2 | LOAD 溢出 | delay_us 死循环 | 单位混淆 | 延时函数参数必须明确标注单位 |
| 3.3 | 中断未开 | delay_ms 死循环 | 裸机陷阱 | 裸机程序必须显式 `__enable_irq()` |
| 4.1 | MQTT 载荷错算 | OTA 数据膨胀→溢出 | TCP 流式 | `total_len` vs `len`：TCP 缓冲 ≠ 包边界 |
| 4.2 | JSON 空格 | ota_start 解析 size=0 | 格式假设 | 不能假设 JSON 无空格；生产代码用 JSON 库 |
| 4.3 | getsockopt 返回 0 | 主循环收不到数据 | WIZnet 约束 | `getsockopt→recv` 是芯片 API 契约 |
| 4.4 | 队列满 | OTA 包全部丢失 | 流控缺失 | 队列缓冲抖动，不能替代端到端流控 |
| 4.5 | 堆不足 | App 静默卡死 | configASSERT 软弱 | 断言必须停止 CPU，否则 NULL 指针延迟崩溃 |
| 4.6 | TCP 分包 | MQTT 解析失败丢包 | 流式重组 | TCP 应用必须循环解析 + 保留不完整帧 |
| 4.7 | LCD 重叠 | 温度和进度条互相覆盖 | 资源共享 | 共享硬件必须互斥或主动避让 |

**跨阶段的共同主题**：**环境假设差异**。FreeRTOS 与裸机之间、不同库之间、TCP 流式与 MQTT 帧之间、WIZnet 硬件与标准 socket API 之间——每一个抽象层边界都隐藏着环境前提。当这些前提不成立时，bug 往往表现隐蔽、定位困难。主动识别并显式记录环境依赖，是减少这类 bug 的最有效手段。
