#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
OTA 固件推送工具 — 通过 MQTT 将 .bin 固件下发到 STM32 设备

用法:
    python ota_push.py app_v1.2.0.bin

依赖:
    pip install paho-mqtt

协议:
    启动:  {"cmd":"ota_start","size":N,"crc32":N}
    数据:  [OTAD][seq:2B BE][chunk data...]    每帧 ≤ 2048 字节
    结束:  {"cmd":"ota_end"}
"""

import sys
import os
import struct
import time
import json
import paho.mqtt.client as mqtt

# ============================================================================
# 配置 — 根据实际环境修改
# ============================================================================

# OneNET MQTT Broker (中国移动物联网平台)
# 平台侧下发指令通常通过 OneNET 控制台或 HTTP API,
# 这里使用直接 MQTT 接入方式用于本地测试或自建 Broker.
MQTT_BROKER   = "183.230.40.96"   # OneNET MQTT 地址
MQTT_PORT     = 1883
MQTT_USERNAME = "507rVcegvD"      # 产品 ID
MQTT_PASSWORD = "version=2018-10-31&res=products%2F507rVcegvD%2Fdevices%2Fw5500&et=1865000000&method=md5&sign=Vgmx6XCq5rqBUERIzoV0zg%3D%3D"
MQTT_CLIENT_ID = "ota_pusher"     # 脚本的 Client ID (不要与设备重名)

# OneNET 设备信息
PRODUCT_ID = "507rVcegvD"
DEVICE_NAME = "w5500"

# 下发 Topic: OneNET 平台通过此 Topic 向设备发指令
# 设备侧订阅了 $sys/{pid}/{did}/#, 会收到此消息
PUB_TOPIC = f"$sys/{PRODUCT_ID}/{DEVICE_NAME}/cmd/ota"

# 每帧数据最大字节数 (不含 6 字节 OTAD 帧头)
# 设备 MQTT 接收缓冲区 2048 字节, 帧头 6 字节, 留余量
CHUNK_SIZE = 2042

# 帧间延迟 (秒) — 给设备 Flash 写入时间
INTER_CHUNK_DELAY = 0.02

# ============================================================================
# CRC32 — 与 STM32 硬件 CRC 外设一致 (CRC-32/MPEG2)
# ============================================================================
# 参数: 多项式 0x04C11DB7, 初始值 0xFFFFFFFF,
#       无输入位反转, 无输出位反转, 无最终 XOR,
#       尾部不足 4 字节用 0x00 填充 LSB

CRC_POLY = 0x04C11DB7

def crc32_mpeg2(data: bytes) -> int:
    """计算 CRC-32/MPEG2 (与 STM32F1 硬件 CRC 一致)"""
    crc = 0xFFFFFFFF
    num_words = len(data) // 4

    # 逐 32-bit 字送入 (小端序)
    for i in range(num_words):
        word = struct.unpack_from('<I', data, i * 4)[0]
        crc ^= word
        for _ in range(32):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ CRC_POLY) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF

    # 处理尾部不足 4 字节: 0x00 填充 LSB
    remainder = len(data) % 4
    if remainder != 0:
        tail = bytearray(4)
        for i in range(remainder):
            tail[i] = data[num_words * 4 + i]
        word = struct.unpack_from('<I', tail, 0)[0]
        crc ^= word
        for _ in range(32):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ CRC_POLY) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF

    return crc

# ============================================================================
# MQTT 回调
# ============================================================================

def on_connect(client, userdata, flags, rc):
    if rc == 0:
        print(f"[MQTT] Connected to {MQTT_BROKER}")
    else:
        print(f"[MQTT] Connection failed, rc={rc}")
        sys.exit(1)

def on_publish(client, userdata, mid):
    pass  # 静默

# ============================================================================
# 主流程
# ============================================================================

def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <firmware.bin>")
        print(f"Example: {sys.argv[0]} app_v1.2.0.bin")
        sys.exit(1)

    fw_path = sys.argv[1]
    if not os.path.exists(fw_path):
        print(f"ERROR: File not found: {fw_path}")
        sys.exit(1)

    # ---- 1. 读取固件 ---- #
    with open(fw_path, 'rb') as f:
        firmware = f.read()

    fw_size = len(firmware)
    print(f"[FW] File: {fw_path}")
    print(f"[FW] Size: {fw_size} bytes ({fw_size / 1024:.1f} KB)")

    if fw_size == 0 or fw_size > 237568:  # 232KB = 237568
        print(f"ERROR: Invalid firmware size ({fw_size}), max 237568 bytes")
        sys.exit(1)

    # ---- 2. 计算 CRC32 ---- #
    fw_crc32 = crc32_mpeg2(firmware)
    print(f"[FW] CRC32: 0x{fw_crc32:08X} ({fw_crc32})")

    # ---- 3. 连接 MQTT ---- #
    print(f"[MQTT] Connecting to {MQTT_BROKER}:{MQTT_PORT}...")
    client = mqtt.Client(client_id=MQTT_CLIENT_ID)
    client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    client.on_connect = on_connect
    client.on_publish = on_publish

    try:
        client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
    except Exception as e:
        print(f"[MQTT] Connection error: {e}")
        print("[MQTT] Hint: Check network / broker address / firewall")
        sys.exit(1)

    client.loop_start()
    time.sleep(1)  # 等待连接确认

    # ---- 4. 发送 ota_start ---- #
    start_msg = json.dumps({
        "cmd":  "ota_start",
        "size": fw_size,
        "crc32": fw_crc32
    })
    print(f"\n[OTA] --- Phase 1: START ---")
    print(f"[OTA] Sending: {start_msg}")
    client.publish(PUB_TOPIC, start_msg, qos=1).wait_for_publish()
    time.sleep(0.5)

    # ---- 5. 分包发送数据 ---- #
    num_chunks = (fw_size + CHUNK_SIZE - 1) // CHUNK_SIZE
    print(f"\n[OTA] --- Phase 2: DATA ({num_chunks} chunk(s)) ---")

    for seq in range(num_chunks):
        offset = seq * CHUNK_SIZE
        chunk = firmware[offset : offset + CHUNK_SIZE]

        # 组装 OTA 帧: [OTAD][seq:2B BE][data]
        frame = b'OTAD' + struct.pack('>H', seq) + chunk

        client.publish(PUB_TOPIC, frame, qos=0)

        progress = (seq + 1) * 100 // num_chunks
        bar_len = progress // 2
        bar = '#' * bar_len + ' ' * (50 - bar_len)
        print(f"\r[OTA] [{bar}] {progress:3d}%  chunk {seq+1}/{num_chunks}", end='', flush=True)

        time.sleep(INTER_CHUNK_DELAY)

    print()  # 换行

    # ---- 6. 发送 ota_end ---- #
    end_msg = json.dumps({"cmd": "ota_end"})
    print(f"\n[OTA] --- Phase 3: END ---")
    print(f"[OTA] Sending: {end_msg}")
    client.publish(PUB_TOPIC, end_msg, qos=1).wait_for_publish()
    time.sleep(0.5)

    # ---- 完成 ---- #
    print(f"\n[OTA] ==============================")
    print(f"[OTA] 固件推送完成!")
    print(f"[OTA] 设备将自动校验并重启升级.")
    print(f"[OTA] ==============================")

    client.loop_stop()
    client.disconnect()

if __name__ == '__main__':
    main()
