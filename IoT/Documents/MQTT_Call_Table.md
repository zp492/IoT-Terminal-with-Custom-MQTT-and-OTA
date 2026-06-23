# MQTT 协议栈 — 回调/函数调用关系表

> 更新时间：2026-06-20

## 一、Mqtt_parse 解析回调

| 回调函数 | 定义位置 | 注册位置 | Mqtt_parse 内调用行 | 触发报文 |
|---------|---------|---------|-------------------|---------|
| `_cb_connack` | `mqtt_wrapper.c:63` | `mqtt_wrapper.c:154` | `mqtt_client.c:424` | CONNACK |
| `_cb_publish` | `mqtt_wrapper.c:77` | `mqtt_wrapper.c:301` | `mqtt_client.c:384` | PUBLISH |
| `_cb_suback` | `mqtt_wrapper.c:83` | `mqtt_wrapper.c:198` | `mqtt_client.c:437` | SUBACK |

## 二、报文组装函数

| 函数 | 定义位置 | 调用位置 | 报文类型 |
|------|---------|---------|---------|
| `mqtt_build_connect` | `mqtt_client.c:168` | `mqtt_wrapper.c:133` | CONNECT |
| `mqtt_build_publish` | `mqtt_client.c:239` | `mqtt_wrapper.c:287` | PUBLISH |
| `mqtt_build_subscribe` | `mqtt_client.c:312` | `mqtt_wrapper.c:177` | SUBSCRIBE |
| `mqtt_build_pingreq` | `mqtt_client.c:343` | `mqtt_wrapper.c:269` | PINGREQ |
| `mqtt_build_disconnect` | `mqtt_client.c:357` | 未调用（保留） | DISCONNECT |

## 三、长度编解码

| 函数 | 定义位置 | 调用位置 |
|------|---------|---------|
| `mqtt_encode_length` | `mqtt_client.c:42` | `mqtt_put_fixed_header` (c:106) |
| `mqtt_decode_length` | `mqtt_client.c:64` | `mqtt_parse` (c:410) |

## 四、传输层

| 函数 | 定义位置 | 调用位置 |
|------|---------|---------|
| `transport_connect` | `transport.c:60` | `mqtt_wrapper.c:115` |
| `transport_disconnect` | `transport.c:116` | `mqtt_wrapper.c:112,137,163,206,316` |
| `transport_send` | `transport.c:141` | `mqtt_wrapper.c:141,178,261,287` |
| `transport_recv` | `transport.c:174` | `mqtt_wrapper.c:150,193,299` |

## 五、MQTT 状态机

| 函数 | 定义位置 | 调用位置 |
|------|---------|---------|
| `mqtt_full_connect` | `mqtt_wrapper.c:102` | `mqtt_wrapper.c:235` |
| `mqtt_task_run` | `mqtt_wrapper.c:220` | `freertos_task.c:301` |

## 六、OneNET 适配层

| 函数 | 定义位置 | 调用位置 |
|------|---------|---------|
| `onenet_set_auth` | `onenet.c:43` | `freertos_task.c:288` |
| `onenet_fill_cfg` | `onenet.c:89` | `freertos_task.c:294` |
| `onenet_build_payload` | `onenet.c:71` | `mqtt_wrapper.c:274`（通过函数指针 `cfg->build_payload`） |

## 七、数据流总览

```
sensor_task (1s)
  └─ xQueueSend(g_sensor_queue, &s)
       │
       └──→ mqtt_wrapper 主循环 (10s)
              ├─ xQueueReceive(sensor_queue) → 取温湿度
              ├─ cfg->build_payload() → onenet_build_payload()
              ├─ mqtt_build_publish() → MQTT PUBLISH 报文
              └─ transport_send() → W5500 → OneNET

OneNET 下发命令:
  transport_recv()
    → mqtt_parse(..., _cb_publish, NULL)
      → _cb_publish(topic, payload, len)
        → g_on_cmd(payload, len) → mqtt_on_cmd()
          → LED0(0/1)  LED1(0/1)
```
