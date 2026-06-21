/**
 ****************************************************************************************************
 * @file        mqtt_client.c
 * @author      zp492
 * @brief       MQTT 3.1.1 客户端 — 报文编解码 & 解析状态机 实现
 * @note        严格按照 MQTT 3.1.1 协议规范编写
 *              - 剩余长度编码 §2.2.3
 *              - CONNECT   §3.1
 *              - CONNACK   §3.2
 *              - PUBLISH   §3.3  (QoS 0)
 *              - SUBSCRIBE §3.8
 *              - SUBACK    §3.9
 *              - PINGREQ   §3.12
 *              - DISCONNECT §3.14
 * @author      MQTT v3.1.1 spec based implementation
 ****************************************************************************************************
 */

#include "mqtt_client.h"
#include <string.h>     /* strlen, memcpy */

/* ================================================================================
 * 剩余长度编解码  (MQTT 3.1.1 §2.2.3)
 * --------------------------------------------------------------------------------
 * 编码: 把 32 位无符号整数转为 1~4 字节的变长表示
 *       每字节低 7 位 = 数据 bit6~0
 *       每字节   bit7 = 1 → 后续还有字节; 0 → 最后一字节
 * 解码: 逆过程
 *
 * 示例: length=321 → 0xC1 0x02 (11000001 00000010)
 *       低字节 0xC1: 0x41(bit6~0)  + bit7=1
 *       高字节 0x02: 0x02(bit6~0)  + bit7=0
 *       解析: 0x41 + 0x02*128 = 65 + 256 = 321
 * ================================================================================ */

/**
 * @brief       剩余长度编码
 * @param       dst:    输出缓冲区 (至少 4 字节)
 * @param       length: 剩余长度 (0 ~ 268435455)
 * @retval      写入的字节数 (1~4)
 */
uint8_t mqtt_encode_length(uint8_t *dst, uint32_t length)
{
    uint8_t n = 0;

    do {
        uint8_t byte = (uint8_t)(length & 0x7F);    /* 取低 7 位 */
        length >>= 7;
        if (length > 0) {
            byte |= 0x80;                           /* bit7=1: 后续还有字节 */
        }
        dst[n++] = byte;
    } while (length > 0 && n < 4);

    return n;                                       /* 1~4 */
}

/**
 * @brief       剩余长度解码
 * @param       src:      输入字节序列
 * @param       consumed: 输出参数, 消耗的字节数 (可为 NULL)
 * @retval      解码后的剩余长度值
 */
uint32_t mqtt_decode_length(const uint8_t *src, uint8_t *consumed)
{
    uint32_t length = 0;
    uint8_t  n = 0;
    uint8_t  byte;

    do {
        if (n >= 4) break;                          /* 最多 4 字节 */

        byte = src[n];
        length |= (uint32_t)(byte & 0x7F) << (7 * n); /* 7 位一组, 逐组左移 */
        n++;
    } while ((byte & 0x80) != 0 && n < 4);          /* bit7=1 继续 */

    if (consumed) *consumed = n;
    return length;
}

/* ================================================================================
 * 固定头组装  (MQTT 3.1.1 §2.2)
 * --------------------------------------------------------------------------------
 * 固定头 = 1 字节控制字段 + 变长剩余长度
 * 控制字段: bit7~4 = 报文类型, bit3~0 = 特定标志 (见各报文)
 * ────────────────────────────────────────────────────────────────────────────────
 * 内部辅助函数: 把"固定头 + 剩余长度"写入 buf, 返回固定头占用的字节数
 * ================================================================================ */

/**
 * @brief       写入固定头: 控制字节 + 剩余长度
 * @param       buf:         输出缓冲区
 * @param       pkt_type:    报文类型 (见 mqtt_pkt_type 枚举)
 * @param       flags:       标志位 (bit3~0, 各报文不同)
 * @param       remaining:   剩余长度 (不含固定头的报文长度)
 * @retval      固定头占用的字节数 (1 控制字节 + 1~4 长度字节)
 */
static uint8_t mqtt_put_fixed_header(uint8_t *buf, uint8_t pkt_type,
                                     uint8_t flags, uint32_t remaining)
{
    /* 控制字节: bit7~4 = 类型, bit3~0 = 标志 */
    buf[0] = (uint8_t)((pkt_type << 4) | (flags & 0x0F));

    /* 变长剩余长度 */
    return 1 + mqtt_encode_length(buf + 1, remaining);
}

/* ================================================================================
 * 字符串组装 — MQTT 字符串格式: 2 字节大端长度 + UTF-8 数据 (§1.5.3)
 * ================================================================================ */

/**
 * @brief       写入 MQTT 字符串 (2 字节长度前缀 + 数据)
 * @param       dst:  输出位置
 * @param       str:  源字符串
 * @return      写入的总字节数 (2 + str_len)
 */
static uint16_t mqtt_put_string(uint8_t *dst, const char *str)
{
    uint16_t len = (uint16_t)strlen(str);

    dst[0] = (uint8_t)(len >> 8);                   /* 长度高字节 */
    dst[1] = (uint8_t)(len & 0xFF);                 /* 长度低字节 */
    memcpy(dst + 2, str, len);                      /* UTF-8 数据 */

    return 2 + len;
}

/* ================================================================================
 * 报文组装 函数
 * ================================================================================ */

/**
 * @brief       组装 CONNECT 报文  (MQTT 3.1.1 §3.1)
 * --------------------------------------------------------------------------------
 * 固定头:
 *   控制字节    0x10 (type=1, flags=0)
 *   剩余长度    (可变)
 *
 * 可变头:
 *   协议名称    "MQTT" (4 字节 UTF-8)
 *   协议级别    0x04   (MQTT 3.1.1)
 *   连接标志    1 字节 (Username / Password / Clean Session / Will / Will QoS / Will Retain)
 *   保活时间    2 字节 大端
 *
 * 载荷 (按顺序):
 *   客户端 ID    UTF-8 字符串
 *   遗嘱主题     UTF-8 字符串 (仅当 Will Flag=1)
 *   遗嘱消息     UTF-8 字符串 (仅当 Will Flag=1)
 *   用户名       UTF-8 字符串 (仅当 User Name Flag=1)
 *   密码         UTF-8 字符串 (仅当 Password Flag=1)
 *
 * 连接标志 bit 分配:
 *   bit7    = 用户名标志
 *   bit6    = 密码标志
 *   bit5    = 遗嘱保留
 *   bit4~3  = 遗嘱 QoS
 *   bit2    = 遗嘱标志
 *   bit1    = 清理会话 (Clean Session)
 *   bit0    = 保留 (必须为 0)
 *
 * @param       buf:  输出缓冲区 (建议 ≥ 256 字节)
 * @param       conn: 连接参数
 * @retval      >0: 报文总长度
 *              0:   缓冲区不足 / 参数错误
 */
uint16_t mqtt_build_connect(uint8_t *buf, const mqtt_conn_t *conn)
{
    uint8_t *p = buf;
    uint8_t  flags = 0;
    uint32_t remaining = 0;

    /* ---- 校验必填参数 ---- */
    if (!conn || !conn->client_id) return 0;

    /* ---- 计算载荷长度 ---- */
    /* 客户端 ID = 2 字节长度前缀 + 字符串 */
    remaining = 2 + (uint32_t)strlen(conn->client_id);

    /* 用户名 (如果有) */
    if (conn->username && conn->username[0] != '\0') {
        flags |= (1 << 7);                          /* User Name Flag = 1 */
        remaining += 2 + (uint32_t)strlen(conn->username);/*2字节长度前缀 + 字符串*/
    }

    /* 密码 (仅当用户名也存在时有效, MQTT 3.1.1 §3.1.3.6) */
    if (conn->password && conn->password[0] != '\0') {
        flags |= (1 << 6);                          /* Password Flag = 1 */
        remaining += 2 + (uint32_t)strlen(conn->password);/*2字节长度前缀 + 字符串*/
    }

    /* 清理会话 */
    if (conn->clean_session) {
        flags |= (1 << 1);                          /* Clean Session = 1 */
    }

    /* 可变头长度: 协议名称(2+"MQTT") + 协议级别(1) + 标志(1) + 保活(2) */
    remaining += 2 + 4 + 1 + 1 + 2;                 /* = 10 + payload_len */

    /* ---- 写入固定头 ---- */
    p += mqtt_put_fixed_header(p, MQTT_CONNECT, 0, remaining);

    /* ---- 写入可变头 ---- */

    /* 协议名称 "MQTT" */
    p += mqtt_put_string(p, "MQTT");

    /* 协议级别 = 4 (MQTT 3.1.1) */
    *p++ = 0x04;

    /* 连接标志 */
    *p++ = flags;

    /* 保活时间 (秒, 大端) */
    *p++ = (uint8_t)(conn->keep_alive >> 8);
    *p++ = (uint8_t)(conn->keep_alive & 0xFF);

    /* ---- 写入载荷 ---- */

    /* 客户端 ID */
    p += mqtt_put_string(p, conn->client_id);

    /* 用户名 */
    if (flags & (1 << 7)) {
        p += mqtt_put_string(p, conn->username);
    }

    /* 密码 */
    if (flags & (1 << 6)) {
        p += mqtt_put_string(p, conn->password);
    }

    return (uint16_t)(p - buf);
}

/**
 * @brief       组装 PUBLISH 报文 QoS 0  (MQTT 3.1.1 §3.3)
 * --------------------------------------------------------------------------------
 * 固定头:
 *   控制字节    0x30 (type=3, DUP=0, QoS=0, RETAIN=0)
 *   剩余长度    topic_len + 2 + payload_len
 *
 * 可变头:
 *   Topic 长度  2 字节 大端
 *   Topic 名称  UTF-8 字符串
 *
 * 载荷:
 *   消息体       (任意字节序列)
 *
 * @note        QoS 0: 无报文标识符, 无重传, 无确认
 *              如需 QoS 1, 需额外实现 PUBACK 逻辑
 *
 * @param       buf:     输出缓冲区 (≥ 128 字节, 视 topic + payload 而定)
 * @param       topic:   主题字符串
 * @param       payload: 消息体 (可为 NULL)
 * @param       payload_len: 消息体字节数
 * @retval      >0: 报文长度
 *              0:   参数错误
 */
uint16_t mqtt_build_publish(uint8_t *buf, const char *topic,
                            const uint8_t *payload, uint16_t payload_len)
{
    uint8_t  *p = buf;
    uint16_t  topic_len = (uint16_t)strlen(topic);
    uint32_t  remaining;

    if (!topic) return 0;

    /* 剩余长度 = 2(topic_len) + topic + payload */
    remaining = 2 + (uint32_t)topic_len + payload_len;

    /* ---- 固定头 ---- */
    /* QoS=0, RETAIN=0, DUP=0 → flags=0x00 */
    p += mqtt_put_fixed_header(p, MQTT_PUBLISH, 0x00, remaining);

    /* ---- Topic ---- */
    *p++ = (uint8_t)(topic_len >> 8);
    *p++ = (uint8_t)(topic_len & 0xFF);
    memcpy(p, topic, topic_len);//memcpy会自增复制，但p不会自增
    p += topic_len;

    /* ---- Payload (可选) ---- */
    if (payload && payload_len > 0) {
        memcpy(p, payload, payload_len);
        p += payload_len;
    }

    return (uint16_t)(p - buf);
}

/**
 * @brief       组装 SUBSCRIBE 报文 QoS 0  (MQTT 3.1.1 §3.8)
 * --------------------------------------------------------------------------------
 * 固定头:
 *   控制字节    0x82 (type=8, flags必须为 0x02)
 *   剩余长度    2(pkt_id) + topic_filter + 1(qos)
 *
 * 可变头:
 *   报文标识符  2 字节 大端
 *
 * 载荷:
 *   Topic Filter 长度  2 字节
 *   Topic Filter       UTF-8 字符串
 *   订阅 QoS           1 字节 (0/1/2)
 *
 * @param       buf:    输出缓冲区 (≥ 64 字节)
 * @param       topic:  主题过滤器
 * @param       pkt_id: 报文标识符 (1~65535)
 * @retval      >0: 报文长度
 *              0:   参数错误
 */
uint16_t mqtt_build_subscribe(uint8_t *buf, const char *topic,
                              uint16_t pkt_id)
{
    uint8_t  *p = buf;
    uint16_t  topic_len = (uint16_t)strlen(topic);
    uint32_t  remaining;

    if (!topic || pkt_id == 0) return 0;

    /* 剩余长度 = 2(pkt_id) + 2(topic_len) + topic + 1(qos) */
    remaining = 2 + 2 + (uint32_t)topic_len + 1;

    /* ---- 固定头 ---- */
    /* bit3~0 必须为 0010, 否则服务器断开连接 */
    p += mqtt_put_fixed_header(p, MQTT_SUBSCRIBE, 0x02, remaining);

    /* ---- 报文标识符 (大端) ---- */
    *p++ = (uint8_t)(pkt_id >> 8);
    *p++ = (uint8_t)(pkt_id & 0xFF);

    /* ---- Topic Filter ---- */
    *p++ = (uint8_t)(topic_len >> 8);
    *p++ = (uint8_t)(topic_len & 0xFF);
    memcpy(p, topic, topic_len);
    p += topic_len;

    /* ---- 请求 QoS = 0 ---- */
    *p++ = 0x00;                                    /* QoS 0 */

    return (uint16_t)(p - buf);
}

/**
 * @brief       组装 PINGREQ 报文  (MQTT 3.1.1 §3.12)
 * --------------------------------------------------------------------------------
 * 固定头: 0xC0 0x00
 * 无可变头, 无载荷
 *
 * @param       buf: 输出缓冲区 (≥ 2 字节)
 * @retval      固定值 2
 */
uint16_t mqtt_build_pingreq(uint8_t *buf)
{
    buf[0] = 0xC0;                                  /* type=12, flags=0 */
    buf[1] = 0x00;                                  /* remaining length = 0 */
    return 2;
}

/**
 * @brief       组装 DISCONNECT 报文  (MQTT 3.1.1 §3.14)
 * --------------------------------------------------------------------------------
 * 固定头: 0xE0 0x00
 * 无可变头, 无载荷
 *
 * @param       buf: 输出缓冲区 (≥ 2 字节)
 * @retval      固定值 2
 */
uint16_t mqtt_build_disconnect(uint8_t *buf)
{
    buf[0] = 0xE0;                                  /* type=14, flags=0 */
    buf[1] = 0x00;                                  /* remaining length = 0 */
    return 2;
}

/* ================================================================================
 * 解析状态机  (MQTT 3.1.1 §2.2)
 * --------------------------------------------------------------------------------
 * 根据固定头第 1 字节的报文类型分流:
 *   byte0 bit7~4 → pkt_type
 *   byte0 bit3~0 → 标志 (各报文含义不同)
 *   byte1~4      → 变长剩余长度
 *   剩余字节     → 可变头 + 载荷
 *
 * 仅处理 C→S 方向的响应报文:
 *   CONNACK (type=2), PUBLISH (type=3), SUBACK (type=9), PINGRESP (type=13)
 *
 * ================================================================================ */

/**
 * @brief       解析 MQTT 报文
 * @note        单包单次解析, 不维护跨包状态
 *              一个字节流中可能包含多个报文 (如 PUBLISH + PINGRESP 粘包),
 *              本函数只解析第一个, 返回消耗的字节数
 * @param       data:   收到的原始数据
 * @param       len:    数据长度
 * @param       on_connack:  CONNACK 回调
 * @param       on_publish:  PUBLISH 回调
 * @param       on_suback:   SUBACK 回调
 * @retval      >0: 成功解析, 返回消耗的字节数
 *              -1: 数据不完整 (需调用方再次 recv)
 *              -2: 协议错误
 *              -3: 未知报文类型
 */
int32_t mqtt_parse(const uint8_t *data, uint16_t len,
                   mqtt_on_connack_t  on_connack,
                   mqtt_on_publish_t  on_publish,
                   mqtt_on_suback_t   on_suback)
{
    uint8_t  pkt_type;
    uint8_t  pos = 0;
    uint8_t  len_consumed;
    uint32_t remaining;
    uint32_t total_len;

    if (!data || len < 2) return -1;                /* 至少需要 2 字节 */

    /* ---- ① 解析固定头第 1 字节 ---- */
    pkt_type = (data[0] >> 4) & 0x0F;               /* bit7~4 = 报文类型 */

    /* ---- ② 解码剩余长度 (从 byte1 开始) ---- */
    remaining = mqtt_decode_length(data + 1, &len_consumed);
    if (len_consumed == 0 || len_consumed > 4) {
        return -2;                                  /* 长度解码异常 */
    }

    /* ---- ③ 计算完整报文长度, 校验是否收全 ---- */
    total_len = 1 + (uint32_t)len_consumed + remaining;
    if ((uint32_t)len < total_len) {
        return -1;                                  /* 数据不全, 等待更多数据 */
    }

    /* ---- ④ 可变头起始位置 ---- */
    pos = 1 + len_consumed;                         /* 跳过 固定头 */

    /* ---- ⑤ 根据报文类型分流处理 ---- */
    switch (pkt_type) {

    /* ============================================================
     * CONNACK  (MQTT 3.1.1 §3.2)
     * ----------------------------------------------------------------------------
     * 可变头: 连接确认标志(1) + 返回码(1)
     *   返回码含义见 enum mqtt_connack_code
     * ============================================================ */
    case MQTT_CONNACK:
        if (remaining < 2) return -2;

        if (on_connack) {
            on_connack(data[pos + 1]);              /* byte2 = 返回码 */
        }
        break;

    /* ============================================================
     * PUBLISH  (MQTT 3.1.1 §3.3)  — 服务器下发到客户端
     * ----------------------------------------------------------------------------
     * 可变头:   Topic 长度(2) + Topic
     * 载荷:     Payload (剩余部分)
     * ============================================================ */
    case MQTT_PUBLISH: {
        uint16_t topic_len;
        const char *topic;
        const uint8_t *payload;
        uint16_t payload_len;

        if (remaining < 2) return -2;

        /* Topic 长度 (大端) */
        topic_len  = ((uint16_t)data[pos] << 8) | data[pos + 1];
        pos += 2;

        if (pos + topic_len > len) return -1;       /* 数据不足 */

        /* Topic 字符串指针 (原始数据中, 无 \0 结尾, 需要上层注意) */
        topic = (const char *)data + pos;
        pos += topic_len;

        /* Payload = 剩余部分 */
        payload_len = (uint16_t)((uint32_t)len - pos);
        payload = (payload_len > 0) ? (data + pos) : NULL;

        if (on_publish) {
            on_publish(topic, payload, payload_len);
        }
        break;
    }

    /* ============================================================
     * SUBACK  (MQTT 3.1.1 §3.9)
     * ----------------------------------------------------------------------------
     * 可变头:  报文标识符(2) + 返回码(1~N)
     * ============================================================ */
    case MQTT_SUBACK:
        if (remaining < 3) return -2;

        {
            uint16_t pkt_id = ((uint16_t)data[pos] << 8) | data[pos + 1];
            uint8_t  ret_code = data[pos + 2];

            if (on_suback) {
                on_suback(pkt_id, ret_code);
            }
        }
        break;

    /* ============================================================
     * PINGRESP  (MQTT 3.1.1 §3.13)
     * ----------------------------------------------------------------------------
     * 无可变头, 无载荷。收到即表示连接正常, 无需额外处理。
     * ============================================================ */
    case MQTT_PINGRESP:
        /* 收到心跳响应, 连接正常 — 上层可在此记录时间戳 */
        break;

    /* ============================================================
     * PUBACK / PUBREC / PUBREL / PUBCOMP / UNSUBACK
     * ----------------------------------------------------------------------------
     * 当前未实现 QoS 1/2 完整交互, 但合法报文不应报错,
     * 只跳过 (上层后续可扩展)
     * ============================================================ */
    case MQTT_PUBACK:
    case MQTT_PUBREC:
    case MQTT_PUBREL:
    case MQTT_PUBCOMP:
    case MQTT_UNSUBACK:
        break;

    /* ============================================================
     * 未知报文
     * ============================================================ */
    default:
        return -3;                                  /* 未知报文类型 */
    }

    return (int32_t)total_len;                      /* 返回消耗的字节数 */
}
