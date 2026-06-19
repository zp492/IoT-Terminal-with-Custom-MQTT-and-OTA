/**
 ****************************************************************************************************
 * @file        mqtt_wrapper.h
 * @brief       MQTT 客户端状态机 — 连接/保活/发布/订阅 调度
 * @note        基于 mqtt_client 编解码 + transport 传输层, 对上层任务暴露
 *              单一入口 mqtt_task_run()
 *              与具体 Broker 平台解耦 (OneNET / 私有 Broker 均适用)
 ****************************************************************************************************
 */

#ifndef __MQTT_WRAPPER_H
#define __MQTT_WRAPPER_H

#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================================
 * 网络状态 (mqtt_wrapper 更新, led_task 读取)
 * ================================================================================ */
#define NET_STATE_DISCONNECTED  0   /* 断线 */
#define NET_STATE_CONNECTING    1   /* TCP/MQTT 连接中 */
#define NET_STATE_CONNECTED     2   /* MQTT 已连接 */

/* ================================================================================
 * Payload 构建回调 (由应用层提供, 如 OneNET JSON 组装)
 * @param buf:     输出缓冲区
 * @param max_len: 缓冲区大小
 * @param temp:    温度 (℃)
 * @param humi:    湿度 (%)
 * @retval         写入字节数
 * ================================================================================ */
typedef uint16_t (*mqtt_payload_build_t)(uint8_t *buf, uint16_t max_len,
                                          uint8_t temp, uint8_t humi);

/* ================================================================================
 * Broker 连接参数
 * ================================================================================ */
typedef struct {
    uint8_t     server_ip[4];       /* Broker IPv4 地址 (如 192.168.1.4) */
    uint16_t    server_port;        /* MQTT 端口 (默认 1883) */
    const char *client_id;          /* 客户端 ID (必填) */
    const char *username;           /* 用户名   (NULL = 匿名) */
    const char *password;           /* 密码     (NULL = 匿名) */
    uint16_t    keep_alive;         /* 保活间隔 (秒, 建议 60) */
    const char *pub_topic;          /* 上报数据主题 */
    const char *sub_topic;          /* 订阅指令主题 (NULL = 不订阅) */
    mqtt_payload_build_t build_payload; /* Payload 构建回调 (NULL = 不上报) */
} mqtt_broker_cfg_t;

/* ================================================================================
 * 平台指令回调 (服务器下发 PUBLISH 时调用)
 * @param payload:  消息体
 * @param len:      消息体长度
 * ================================================================================ */
typedef void (*mqtt_on_cmd_t)(const uint8_t *payload, uint16_t len);

/* ================================================================================
 * 入口函数 (由 FreeRTOS 任务调用)
 * ================================================================================ */

/**
 * @brief       MQTT 客户端主循环 (供 FreeRTOS 任务入口调用)
 * @note        内部为无限循环, 自动处理:
 *              ① TCP 连接 (transport) → ② MQTT CONNECT → ③ SUBSCRIBE
 *              → ④ 主循环 (PING + PUBLISH + 收指令)
 *              断线自动重连
 * @param       cfg:     Broker 连接参数 (需全局/静态存储, 不可释放)
 * @param       on_cmd:  平台指令回调 (可为 NULL)
 */
void mqtt_task_run(const mqtt_broker_cfg_t *cfg, mqtt_on_cmd_t on_cmd,
                   QueueHandle_t sensor_queue);

#ifdef __cplusplus
}
#endif

#endif /* __MQTT_WRAPPER_H */
