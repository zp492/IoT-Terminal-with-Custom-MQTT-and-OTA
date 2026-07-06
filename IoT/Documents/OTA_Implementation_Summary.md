# OTA 远程升级 — 实现总结

> 更新时间：2026-06-26（含联调踩坑）

## 项目概要

STM32F103ZET6 + W5500 + FreeRTOS + OneNET MQTT 平台上实现的完整 OTA（Over-The-Air）固件远程升级方案。

## 实现步骤

### 第 1 步：Flash 分区设计

**问题**：App 占据整个 512KB Flash，无空间容纳 Bootloader 和下载缓存。

**方案**：三分区布局：

```
0x08000000  Bootloader  (48KB)   — 上电最先执行，负责检查升级标志、搬运固件、跳转
0x0800C000  App 主区    (232KB)  — 当前运行固件
0x08046000  Download 区 (232KB)  — 新固件暂存地
0x0800B000  Flag 页     (2KB)    — Bootloader 与 App 之间的"留言板"
```

**关键工程变更**：
- 分散加载文件：App 起始地址从 0x08000000 改为 0x0800C000
- `main.c` 开头调用 `SCB->VTOR = 0x0800C000` 重定位向量表
- `system_stm32f1xx.c` 使能 `USER_VECT_TAB_ADDRESS` 作为双保险

**经验**：分区是所有 OTA 实现的地基。地址常量集中在一个头文件（`ota_partition.h`），Bootloader 和 App 通过 include 同一文件保证一致性。

---

### 第 2 步：Bootloader 编写

**问题**：需要一段代码在 App 运行之前判断是否升级、执行固件搬运。

**方案**：裸机 Bootloader（无 FreeRTOS），独立 Keil 工程，ROM 0x08000000/48KB。

**核心流程**：

```
上电 → 读 Flag 页 (0x0800B000)
  ├─ magic != OTA_FLAG_MAGIC → 无升级 → 跳转 App
  ├─ magic 有效 → CRC32 校验 Download 区
  │   ├─ CRC 不匹配 → 擦 Flag → 跳转旧 App（回退）
  │   └─ CRC 通过 → 逐页搬运 Download→App
  │       ├─ 每页: 读源 → 擦目标 → 写目标 → 逐字节验证
  │       └─ 全 116 页完成 → 全量回读校验 → 擦 Flag → 跳转新 App
```

**关键设计**：
- **裸机运行**：不依赖 FreeRTOS，SysTick 只用于 g_bl_tick 和 HAL_IncTick
- **逐页原子搬运**：每页独立完成"读→擦→写→验"四步，任意时刻断电最大损失 1 页
- **CRC32 硬件外设**：与 STM32F1 CRC 外设一致的多项式 (0x04C11DB7)
- **跳转 App 的标准 Cortex-M3 序列**：关中断→停SysTick→设MSP→设VTOR→跳转

**掉电恢复**：
- Flag 未清除期间断电 → 下次上电重新执行（操作幂等）
- 旧 App 完好时 CRC 不匹配 → 擦 Flag，回退旧 App
- App 区已擦除后断电 → Flag 有效，重试升级

---

### 第 3 步：App 端 OTA 下载

**问题**：App 运行时如何安全地接收固件、写入 Flash、触发 Bootloader。

**方案**：通过 OneNET MQTT 接收固件数据，独立 FreeRTOS 任务处理 Flash 操作。

**OTA 协议**：

```
启动:  {"cmd":"ota_start","size":237568,"crc32":3735928559}
数据:  [OTAD][seq:2B BE][chunk data...]   (二进制帧，每帧 ≤ 2048 bytes)
结束:  {"cmd":"ota_end"}
取消:  {"cmd":"ota_cancel"}
```

**任务架构（第 3 步重构重点）**：

```
mqtt_task (pri 4)              ota_task (pri 2)
     │                               │
     ├─ mqtt_on_cmd()                │
     │   ├─ 识别 OTA 包              │
     │   ├─ memcpy → 队列项          │
     │   ├─ xQueueSendToBack (非阻塞) │
     │   └─ 立即返回 (< 1ms)         │
     │                               │
     │                        ┌──────┘
     │                        │ xQueueReceive (阻塞等待)
     │                        │ ota_start  → 擦 Download 区 (4.5秒)
     │                        │ ota_data   → 写 Flash (10ms/包)
     │                        │ ota_end    → CRC32 校验 + 写 Flag
     │                        └─ NVIC_SystemReset()
```

**关键经验**：
- **回调里只做轻量操作**：Flash 擦一页需要 40ms，擦 116 页 4.5 秒。如果在 MQTT 回调里做，整个 MQTT 循环卡死。独立任务 + 消息队列解耦。
- **队列深度 4、单包 2048 字节**：足够缓冲 Flash 写入期间的网络收包。
- **OTA 任务优先级 3，低于 MQTT(4)（激活时升最高 + 挂起 MQTT）**：平常不争 CPU，升级时独占。

---

### 第 4 步：固件版本管理

**方案**：在固件固定偏移处嵌入版本信息结构体，Bootloader 读取用于 LCD 显示。

```
App 固件 0x0800C200 处 → fw_info_t { magic, major, minor, patch, version_str, build_date }
```

使用 ARMCC5 的 `__attribute__((at(0x0800C200)))` 放置在 Flash 固定地址。

**经验**：版本信息从代码中读取而非硬编码，升级时只需改 `fw_version.h`。

---

### 第 5 步：Python 下发工具

**方案**：Python 脚本读取 .bin 文件，计算 CRC32（与 STM32 硬件完全一致），通过 MQTT 分包推送到设备。

**关键**：CRC32 算法必须和 STM32 硬件 CRC 完全一致（多项式 0x04C11DB7，初始值 0xFFFFFFFF，无反转/无 XOR，尾部 0x00 填充 LSB）。

---

### 第 6 步：联调关键问题

**6.1 TCP 流式数据重组**

MQTT over TCP 是字节流，W5500 一次 `recv` 可能返回多个 MQTT 包的部分数据。原代码假设每次 `recv` 恰好返回一个完整包，导致多包丢失和载荷污染。

**解决**：引入 `g_rx_pending` 保留不完整数据。每次 `recv` 追加到已有尾部，循环 `mqtt_parse` 直到全部消费或遇不完整帧。不完整部分 `memmove` 到 buffer 头部等待下次拼接。

**6.2 MQTT 载荷长度错算**

`mqtt_client.c` 中 `payload_len = len - pos` 使用总接收长度，当 buffer 含多个包时载荷被污染。改为 `payload_len = total_len - pos`，限制在当前 MQTT 包范围内。

**6.3 JSON 解析器不处理空格** ✅ 已根治

paho-mqtt 生成的 JSON 中冒号后有空格：`"size": 62492`。`strstr` + 固定偏移跳过 key 后指向空格而非数字，导致解析值恒为 0。临时修复为跳过 key 后主动越过空格再解析数字。**最终方案：移植 cJSON 库**，用 `cJSON_ParseWithLength` + `cJSON_GetObjectItem` 彻底替换所有 `strstr` 解析，同时处理 LED 控制命令和 OTA 指令。

**6.4 擦除期间队列溢出**

`ota_start` 后擦除 Download 区需 4.5 秒，期间数据块涌入队列（仅 4 槽）被丢弃。脚本 `ota_start` 后增加 6 秒延迟，待设备擦除完毕再发数据。

**6.5 FreeRTOS 堆不足静默崩溃**

`configASSERT` 仅定义为 `printf`，不停止 CPU。队列创建失败后 NULL 指针操作导致 HardFault。堆从 20KB 扩至 25KB，同时认识到生产代码中 ASSERT 必须导致停机。

**6.6 LCD 共享冲突**

`sensor_task`（每 1s 刷新温湿度）与 OTA 下载进度条同时写 LCD 导致重叠。修复：`sensor_task` 中检查 `ota_get_state() != OTA_IDLE` 时跳过 LCD 写入。

---

## 端到端测试流程

```
1. 烧录 Bootloader (0x08000000) + App v1.0 (0x0800C000)
2. 上电 → Bootloader 无 Flag → 跳转 App → 正常运行
3. 修改 fw_version.h → 编译 App v1.1
4. python ota_push.py Output/f103_zj.hex
      │
   MQTT Broker ←─ 脚本发布 ota_start (size + crc32)
      │           等待 6s (设备擦除 Download 区)
      │           发布 35 个 OTAD 数据块
      │           发布 ota_end
      ▼
   设备 MQTT 任务 → 入队 OTA 包 → OTA 任务写 Flash
      │
   CRC 校验通过 → 写 Flag → NVIC_SystemReset()
      │
   Bootloader 启动 → 读 Flag → CRC 校验 → 31 页逐页搬运
      │           逐页: 读源→擦目标→写目标→逐字节验证
      │           全量回读校验 (499936 bits)
      │           擦 Flag → jump_to_app()
      ▼
   新固件 v1.1 启动 ✓
```

---

## 架构全景

```
┌─────────────────────────────────────────────────────────────────┐
│                        512KB Flash                              │
├────────────┬────────────────────┬───────────────────────────────┤
│ Bootloader │    App (232KB)     │     Download (232KB)          │
│  48KB      │  当前运行固件       │    OTA 下载暂存               │
│ 0x08000000 │  0x0800C000        │    0x08046000                 │
└────────────┴────────────────────┴───────────────────────────────┘
       ↑              ↑                       ↑
  从不自更新      上电后由 BL              App OTA 任务
  出厂烧录        决定是否跳转              写入新固件

Flag 页 (0x0800B000): [magic][fw_size][fw_crc32][status]
                      Bootloader 读 ← → OTA 任务写
```

## 关键技术点

| 技术点 | 解决方案 |
|--------|----------|
| Flash 分区一致性 | `ota_partition.h` 单一真相源 |
| 向量表偏移 | `SCB->VTOR = 0x0800C000` |
| CRC32 一致性 | STM32 硬件 CRC = Python 软件 CRC (CRC-32/MPEG2) |
| 掉电保护 | Flag 在全部验证通过后才擦除；逐页原子操作 |
| MQTT 不阻塞 | 独立 OTA 任务 + FreeRTOS 队列 |
| TCP 流式重组 | `g_rx_pending` 保留不完整帧, 循环解析 + 下次拼接 |
| MQTT 载荷对齐 | `payload_len = total_len - pos` 而非 `len - pos` |
| JSON 格式兼容 | 移植 cJSON 库，`cJSON_ParseWithLength` + `cJSON_GetObjectItem` 精准解析 |
| 队列流控 | 脚本 `ota_start` 后延时 6s 等设备擦除完成 |
| FreeRTOS 堆安全 | `configASSERT` 应停机; 堆从 20KB→25KB |
| LCD 互斥 | `ota_get_state() != OTA_IDLE` 时 sensor task 跳写 |
| ARMCC5/C89 兼容 | 无 inline, 无 // 注释, 无 LL 库, 变量在块首声明 |
| Bootloader 裸机 | 无 FreeRTOS, 自实现 SysTick 延时, 显式 `__enable_irq()` |
