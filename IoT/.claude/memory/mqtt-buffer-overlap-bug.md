---
name: mqtt-buffer-overlap-bug
description: MQTT数据上报OneNET无数据—根因是buffer重用覆盖JSON载荷
metadata:
  type: project
---

## 现象

W5500 通过 MQTT 连接 OneNET 成功（connack=0），PUBLISH 报 `sent 90/90 bytes`，但平台始终显示 null 无数据。MQTT.fx 用同样格式发送却有数据。

## 排查过程

1. 尝试多种 JSON 格式：`{"temp":24,"humi":67}`、`{"id":123,"dp":{...}}`、`{"params":{...}}` — 均无效
2. 尝试不同 Topic：`dp/post/json`、`thing/property/post` — 均无效
3. 尝试 OneNET 数据流协议、OneJSON 协议 — 均无效
4. 更换产品、设备、Token 多次 — 均无效
5. 更换数据协议（数据流↔OneJson）多次 — 均无效
6. MQTT.fx 桌面客户端用完全相同的格式发 → 平台有数据

## 根因

`mqtt_wrapper.c` PUBLISH 段存在**缓冲区重叠写入**：

```c
// 步骤 1: 将 JSON 写到 g_tx_buf 开头
json_len = cfg->build_payload(g_tx_buf, MQTT_SEND_BUF_SIZE, ...);  
// g_tx_buf = {"id":1,"dp":{"temp":[{"v":27}],...}}

// 步骤 2: mqtt_build_publish 也往 g_tx_buf 开头写固定头 + Topic
len = mqtt_build_publish(g_tx_buf, topic, g_tx_buf, json_len);
//                    ^^^^^^^^         ^^^^^^^^
//                 输出 = g_tx_buf   输入也 = g_tx_buf  ← 重叠！
```

`mqtt_build_publish` 从 `buf[0]` 开始写：`固定头(2~5B) + Topic(2+len)`，直接覆盖了之前写在同一位置的 JSON。随后 `memcpy(p, payload, payload_len)` 虽然从原始 `g_tx_buf` 读，但前 50+ 字节已被覆盖为 MQTT 协议头。

**实际上发送到 OneNET 的是：MQTT 固定头 + Topic + 被截断的 JSON 后半段 + 越界内存垃圾。**

串口打印 `printf("[MQTT] pub: %s", g_tx_buf)` 在 `mqtt_build_publish` 之前执行，所以**打印的是完整 JSON，但发出的已不是**。

## 修复

```c
// 用独立缓冲区存放 JSON payload
uint8_t json_payload[128];
int32_t json_len = cfg->build_payload(json_payload, sizeof(json_payload), ...);
// mqtt_build_publish 输出到 g_tx_buf，payload 从 json_payload 读，不冲突
len = mqtt_build_publish(g_tx_buf, topic, json_payload, (uint16_t)json_len);
```

## 教训

1. **永远不要让输出 buffer 和输入 buffer 重叠**——尤其是调用方同时充当生产者和消费者时
2. **printf 调试有盲区**——打印的是操作前的值，操作本身可能破坏数据
3. **MQTT.fx 对比测试是有效手段**——它能证明格式正确，把问题范围缩小到代码实现
4. **OneNET 平台不给错误反馈**——消息发错格式服务器静默丢弃，不会返回错误

## 相关记忆

- [[freertos-heap-semaphore-crash]] — FreeRTOS 堆不足导致信号量 crash
- [[require-approval-before-edit]] — 修改文件前需用户同意
