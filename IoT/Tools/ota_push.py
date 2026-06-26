#!/usr/bin/env python3
"""OTA 固件推送工具 — 通过 MQTT 推送 .bin/.hex 到 STM32"""

import sys, os, struct, time, json
import paho.mqtt.client as mqtt

# ============== 配置 ==============
MQTT_BROKER   = "test.mosquitto.org"
MQTT_PORT     = 1883
MQTT_USERNAME = None
MQTT_PASSWORD = None
MQTT_CLIENT_ID = "ota_pusher_test"

PUB_TOPIC = "stm32/ota"
CHUNK_SIZE = 1800  # TCP 流式重组已修复, 大块也可靠
INTER_CHUNK_DELAY = 0.02

# ============== CRC32 (STM32 硬件一致) ==============
CRC_POLY = 0x04C11DB7

def crc32_mpeg2(data: bytes) -> int:
    crc = 0xFFFFFFFF
    nw = len(data) // 4
    for i in range(nw):
        w = struct.unpack_from('<I', data, i * 4)[0]
        crc ^= w
        for _ in range(32):
            crc = ((crc << 1) ^ CRC_POLY) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    rem = len(data) % 4
    if rem:
        tail = bytearray(4)
        for i in range(rem): tail[i] = data[nw * 4 + i]
        w = struct.unpack_from('<I', tail, 0)[0]
        crc ^= w
        for _ in range(32):
            crc = ((crc << 1) ^ CRC_POLY) & 0xFFFFFFFF if crc & 0x80000000 else (crc << 1) & 0xFFFFFFFF
    return crc

# ============== HEX -> BIN ==============
def hex2bin(path: str) -> bytes:
    base = 0
    lo, hi = None, 0
    recs = []
    with open(path) as f:
        for ln in f:
            ln = ln.strip()
            if not ln.startswith(':'): continue
            n = int(ln[1:3], 16)
            a = int(ln[3:7], 16)
            t = int(ln[7:9], 16)
            d = bytes.fromhex(ln[9:9+n*2])
            if t == 0x04:
                base = struct.unpack('>H', d)[0] << 16
            elif t == 0x00:
                addr = base + a
                if lo is None or addr < lo: lo = addr
                if addr + n > hi: hi = addr + n
                recs.append((addr, d))
            elif t == 0x01: break
    if lo is None: return b''
    buf = bytearray(b'\xFF') * (hi - lo)
    for addr, d in recs: buf[addr - lo:addr - lo + len(d)] = d
    print(f"[FW] HEX: 0x{lo:08X}~0x{hi:08X}, bin={len(buf)} bytes")
    return bytes(buf)

# ============== MQTT ==============
def on_connect(client, userdata, flags, rc):
    if rc == 0: print(f"[MQTT] Connected to {MQTT_BROKER}")
    else: print(f"[MQTT] Connection failed rc={rc}"); sys.exit(1)

def on_message(client, userdata, msg):
    print(f"\n[MQTT] <<< received on '{msg.topic}': {msg.payload[:80]}")

# ============== 主流程 ==============
def main():
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} <firmware.hex|.bin>")
        sys.exit(1)

    fw_path = sys.argv[1]
    if not os.path.exists(fw_path):
        print(f"ERROR: not found: {fw_path}"); sys.exit(1)

    if fw_path.lower().endswith('.hex'):
        firmware = hex2bin(fw_path)
    else:
        with open(fw_path, 'rb') as f: firmware = f.read()

    fw_size = len(firmware)
    print(f"[FW] Size: {fw_size} bytes ({fw_size/1024:.1f} KB)")
    if fw_size == 0 or fw_size > 237568:
        print(f"ERROR: size {fw_size}, max 237568"); sys.exit(1)

    fw_crc = crc32_mpeg2(firmware)
    print(f"[FW] CRC32: 0x{fw_crc:08X}")

    client = mqtt.Client(client_id=MQTT_CLIENT_ID)
    if MQTT_USERNAME: client.username_pw_set(MQTT_USERNAME, MQTT_PASSWORD)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect(MQTT_BROKER, MQTT_PORT, 60)
    client.loop_start()
    time.sleep(1)

    # 订阅设备上报 Topic, 验证双向通信
    client.subscribe("stm32/sensor")
    print("[MQTT] Subscribed to stm32/sensor (waiting for device data...)")
    time.sleep(3)  # 等几秒看是否收到传感器数据

    # start
    msg = json.dumps({"cmd":"ota_start","size":fw_size,"crc32":fw_crc})
    print(f"\n[OTA] start: {msg}")
    client.publish(PUB_TOPIC, msg, qos=1).wait_for_publish()
    print("[OTA] Waiting 6s for device to erase Download area...")
    time.sleep(6)

    # data
    chunks = (fw_size + CHUNK_SIZE - 1) // CHUNK_SIZE
    print(f"[OTA] sending {chunks} chunks...")
    for seq in range(chunks):
        off = seq * CHUNK_SIZE
        chunk = firmware[off:off+CHUNK_SIZE]
        frame = b'OTAD' + struct.pack('>H', seq) + chunk
        client.publish(PUB_TOPIC, frame, qos=0)
        pct = (seq+1)*100//chunks
        bar = '#'*(pct//2) + ' '*(50-pct//2)
        print(f"\r[OTA] [{bar}] {pct:3d}%  {seq+1}/{chunks}", end='', flush=True)
        time.sleep(INTER_CHUNK_DELAY)
    print()

    # end
    msg = json.dumps({"cmd":"ota_end"})
    print(f"[OTA] end: {msg}")
    client.publish(PUB_TOPIC, msg, qos=1).wait_for_publish()

    print(f"\n[OTA] Done!")
    client.loop_stop()
    client.disconnect()

if __name__ == '__main__':
    main()
