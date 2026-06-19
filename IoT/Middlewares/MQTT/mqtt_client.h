/**
 ****************************************************************************************************
 * @file        mqtt_client.h
 * @brief       MQTT 3.1.1 客户端 — 报文编解码 & 解析状态机
 * @note        与传输层解耦: 只负责报文组装/解析, 不感知底层 TCP 实现
 *              组装函数  → 调用方提供缓冲区, 返回写入的字节数
 *              解析函数  → 调用方传入收到的数据, 回调驱动处理结果
 * @author      MQTT v3.1.1 spec based implementation
 ****************************************************************************************************
 */

#ifndef __MQTT_CLIENT_H
#define __MQTT_CLIENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================================
 * 报文类型 (MQTT 3.1.1 §2.2)
 * --------------------------------------------------------------------------------
 * 固定头第 1 字节 bit7~4 = 报文类型, bit3~0 = 标志
 * ================================================================================ */
enum mqtt_pkt_type {
    MQTT_CONNECT       = 1,         /* 连接请求       (C→S) */
    MQTT_CONNACK       = 2,         /* 连接确认       (S→C) */
    MQTT_PUBLISH       = 3,         /* 发布消息       (双向) */
    MQTT_PUBACK        = 4,         /* QoS1 发布确认  (双向) */
    MQTT_PUBREC        = 5,         /* QoS2 发布收到  (双向) */
    MQTT_PUBREL        = 6,         /* QoS2 发布释放  (双向) */
    MQTT_PUBCOMP       = 7,         /* QoS2 发布完成  (双向) */
    MQTT_SUBSCRIBE     = 8,         /* 订阅请求       (C→S) */
    MQTT_SUBACK        = 9,         /* 订阅确认       (S→C) */
    MQTT_UNSUBSCRIBE   = 10,        /* 取消订阅       (C→S) */
    MQTT_UNSUBACK      = 11,        /* 取消订阅确认   (S→C) */
    MQTT_PINGREQ       = 12,        /* 心跳请求       (C→S) */
    MQTT_PINGRESP      = 13,        /* 心跳响应       (S→C) */
    MQTT_DISCONNECT    = 14,        /* 断开连接       (C→S) */
};

/* ================================================================================
 * CONNACK 返回码 (MQTT 3.1.1 §3.2.2.3)
 * ================================================================================ */
enum mqtt_connack_code {
    MQTT_CONN_ACCEPTED          = 0, /* 连接已接受 */
    MQTT_CONN_REFUSED_PROTOCOL  = 1, /* 协议版本不支持 */
    MQTT_CONN_REFUSED_ID        = 2, /* ClientID 被拒绝 */
    MQTT_CONN_REFUSED_SERVER    = 3, /* 服务器不可用 */
    MQTT_CONN_REFUSED_USER_PASS = 4, /* 用户名或密码错误 */
    MQTT_CONN_REFUSED_AUTH      = 5, /* 未授权 */
};

/* ================================================================================
 * SUBACK 返回码 (MQTT 3.1.1 §3.9.3)
 * ================================================================================ */
enum mqtt_qos_code {
    MQTT_QOS0_MAX = 0x00,               /* QoS 0 订阅成功 */
    MQTT_QOS1_MAX = 0x01,               /* QoS 1 订阅成功 */
    MQTT_QOS2_MAX = 0x02,               /* QoS 2 订阅成功 */
    MQTT_SUB_FAIL  = 0x80,              /* 订阅失败 */
};

/* ================================================================================
 * CONNECT 报文参数 (MQTT 3.1.1 §3.1)
 * --------------------------------------------------------------------------------
 * 必填: client_id
 * 选填: username / password (OneNET 需要)
 * keep_alive: 保活间隔 (秒), 0 = 关闭保活
 * clean_session: 1 = 服务器丢弃断线期间的消息 (建议 1)
 * ================================================================================ */
typedef struct {
    const char *client_id;              /* 客户端 ID (必填, 最大 65535 字节) */
    const char *username;               /* 用户名   (可选, 为 NULL 则不发送) */
    const char *password;               /* 密码     (可选, 为 NULL 则不发送) */
    uint16_t    keep_alive;             /* 保活间隔 (秒, OneNET 建议 30~120) */
    uint8_t     clean_session;          /* 0/1, 清理会话 */
} mqtt_conn_t;

/* ================================================================================
 * 解析回调函数类型
 * --------------------------------------------------------------------------------
 * 当解析状态机收到完整报文后, 通过回调通知上层
 * - on_connack : CONNACK 返回码
 * - on_publish : 收到服务器下发的消息 (topic + payload)
 * - on_suback  : 订阅确认 (pkt_id + 返回码)
 * ================================================================================ */
typedef void (*mqtt_on_connack_t)(uint8_t ret_code);
typedef void (*mqtt_on_publish_t)(const char *topic,
                                  const uint8_t *payload, uint16_t len);
typedef void (*mqtt_on_suback_t)(uint16_t pkt_id, uint8_t ret_code);

/* ================================================================================
 * 剩余长度编解码 (MQTT 3.1.1 §2.2.3)
 * --------------------------------------------------------------------------------
 * 变长编码, 每字节低 7 位是数据, bit7=1 表示后续还有字节
 * 最大 4 字节, 可表示 0 ~ 268,435,455 (256MB)
 * ================================================================================ */

/**
 * @brief       编码剩余长度 → 变长字节序列
 * @param       dst:    输出缓冲区 (至少 4 字节)
 * @param       length: 剩余长度值
 * @retval      写入的字节数 (1~4)
 */
uint8_t mqtt_encode_length(uint8_t *dst, uint32_t length);

/**
 * @brief       解码变长字节序列 → 剩余长度
 * @param       src:     输入字节序列
 * @param       consumed: 输出参数, 返回消耗的字节数 (1~4), 可为 NULL
 * @retval      解码后的剩余长度值
 */
uint32_t mqtt_decode_length(const uint8_t *src, uint8_t *consumed);

/* ================================================================================
 * 报文组装 (写入调用方提供的缓冲区, 返回实际写入字节数)
 * ================================================================================ */

/**
 * @brief       组装 CONNECT 报文 (MQTT 3.1.1 §3.1)
 * @param       buf:  输出缓冲区 (建议 ≥ 256 字节, 视 client_id 长度而定)
 * @param       conn: 连接参数
 * @retval      >0: 报文长度 (字节)
 *              0:   缓冲区不足
 */
uint16_t mqtt_build_connect(uint8_t *buf, const mqtt_conn_t *conn);

/**
 * @brief       组装 PUBLISH 报文 QoS 0 (MQTT 3.1.1 §3.3)
 * @note        当前仅实现 QoS 0 (不需 pkt_id, 不需 PUBACK 确认)
 * @param       buf:     输出缓冲区 (建议 ≥ 128 字节)
 * @param       topic:   主题字符串
 * @param       payload: 消息体 (可为 NULL, 表示空消息)
 * @param       payload_len: 消息体长度
 * @retval      >0: 报文长度
 *              0:   缓冲区不足
 */
uint16_t mqtt_build_publish(uint8_t *buf, const char *topic,
                            const uint8_t *payload, uint16_t payload_len);

/**
 * @brief       组装 SUBSCRIBE 报文 QoS 0 (MQTT 3.1.1 §3.8)
 * @param       buf:    输出缓冲区 (建议 ≥ 64 字节)
 * @param       topic:  主题过滤器
 * @param       pkt_id: 报文标识符 (1~65535)
 * @retval      >0: 报文长度
 *              0:   缓冲区不足
 */
uint16_t mqtt_build_subscribe(uint8_t *buf, const char *topic,
                              uint16_t pkt_id);

/**
 * @brief       组装 PINGREQ 报文 (MQTT 3.1.1 §3.12)
 * @param       buf: 输出缓冲区 (≥ 2 字节)
 * @retval      固定值 2 (固定头 0xC0 + 剩余长度 0x00)
 */
uint16_t mqtt_build_pingreq(uint8_t *buf);

/**
 * @brief       组装 DISCONNECT 报文 (MQTT 3.1.1 §3.14)
 * @param       buf: 输出缓冲区 (≥ 2 字节)
 * @retval      固定值 2 (固定头 0xE0 + 剩余长度 0x00)
 */
uint16_t mqtt_build_disconnect(uint8_t *buf);

/* ================================================================================
 * 解析状态机 (调用方从 TCP 收到数据后调用)
 * ================================================================================ */

/**
 * @brief       解析 MQTT 报文, 回调驱动
 * @note        单包解析, 不缓存跨包状态
 *              调用方需保证 data 包含完整报文 (TCP 保证字节流顺序)
 * @param       data:   收到的原始数据
 * @param       len:    数据长度
 * @param       on_connack:  CONNACK 回调 (NULL 忽略)
 * @param       on_publish:  PUBLISH 回调 (NULL 忽略)
 * @param       on_suback:   SUBACK 回调  (NULL 忽略)
 * @retval      >0: 成功解析, 返回消耗的字节数
 *              -1: 数据不完整 (需要更多数据)
 *              -2: 协议错误
 *              -3: 未知报文类型
 */
int32_t mqtt_parse(const uint8_t *data, uint16_t len,
                   mqtt_on_connack_t  on_connack,
                   mqtt_on_publish_t  on_publish,
                   mqtt_on_suback_t   on_suback);

#ifdef __cplusplus
}
#endif

#endif /* __MQTT_CLIENT_H */
