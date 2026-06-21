/**
 ****************************************************************************************************
 * @file        mqtt_wrapper.c
 * @author      zp492
 * @brief       MQTT 客户端状态机 — 连接 / 保活 / 上报 / 收指令
 * @note        基于 mqtt_client 编解码 + transport 传输层
 *              使用 Socket 1, 与 TCP 测试客户端 (Socket 0) 隔离
 *
 *              传感器数据通过消息队列 g_sensor_queue 获取
 *              sensor_data_t 结构体 (定义在 freertos_task.h)
 ****************************************************************************************************
 */

#include "mqtt_wrapper.h"
#include "mqtt_client.h"
#include "transport.h"
#include "freertos_task.h"                      /* sensor_data_t */
#include <string.h>
#include <stdio.h>                              /* snprintf */

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"

/* ---- 网络状态 (led_task 读取) ---- */
extern volatile uint8_t g_net_state;

/* ---- 网络就绪信号量 (monitor_task 给) ---- */
extern SemaphoreHandle_t g_net_ready_sem;

/* ================================================================================
 * 配置常量
 * ================================================================================ */

#define MQTT_SOCKET            1               /* 独占 Socket 1 */
#define MQTT_RECV_BUF_SIZE     512             /* 接收缓冲区 */
#define MQTT_SEND_BUF_SIZE     512             /* 发送缓冲区 (CONNECT ~300B / PUBLISH ~200B) */

/* 时序参数 (ms) */
#define MQTT_RECONNECT_DELAY   3000            /* 断线后重连间隔 */
#define MQTT_CONNACK_TIMEOUT   5000            /* 等待 CONNACK 超时 */
#define MQTT_SUBACK_TIMEOUT    5000            /* 等待 SUBACK 超时 */
#define MQTT_LOOP_RECV_TO      500             /* 主循环 recv 轮询间隔 */
#define MQTT_PING_INTERVAL_MS  40000           /* PING 间隔 (不可超过 keep_alive) */
#define MQTT_PUB_INTERVAL_MS   10000           /* 上报间隔 (10s) */

/* ================================================================================
 * 内部状态
 * ================================================================================ */

/* 连接状态 */
enum {
    STATE_DISCONNECTED,
    STATE_TCP_CONNECTING,
    STATE_WAIT_CONNACK,
    STATE_WAIT_SUBACK,
    STATE_CONNECTED,
};

static uint8_t  g_state = STATE_DISCONNECTED;
static uint8_t  g_rx_buf[MQTT_RECV_BUF_SIZE];
static uint8_t  g_tx_buf[MQTT_SEND_BUF_SIZE];

static TickType_t g_last_ping;
static TickType_t g_last_pub;

/* 报文标识符 (+1 每次, 回绕到 1) */
static uint16_t g_pkt_id = 0;

/* CONNACK / SUBACK 回调结果暂存 */
static uint8_t  g_connack_rc;
static uint8_t  g_suback_rc;

/* 平台指令回调 (外部注入) */
static mqtt_on_cmd_t g_on_cmd = NULL;

/* ================================================================================
 * MQTT 解析 回调 (static)
 * ================================================================================ */

static void _cb_connack(uint8_t ret_code) { g_connack_rc = ret_code; }

static void _cb_suback(uint16_t pkt_id, uint8_t ret_code) {
    (void)pkt_id;
    g_suback_rc = ret_code;
}

/* PUBLISH 回调 → 丢 topic, 只传 payload 给上层 */
static void _cb_publish(const char *topic, const uint8_t *payload, uint16_t len)
{
    (void)topic;
    if (g_on_cmd) {
        g_on_cmd(payload, len);
    }
}

/* ================================================================================
 * 连接流程: TCP → MQTT CONNECT → 等 CONNACK → SUBSCRIBE → 等 SUBACK
 * ================================================================================ */

/**
 * @brief       完整连接: TCP + MQTT CONNECT + 等 CONNACK + SUBSCRIBE + 等 SUBACK
 * @param       cfg: Broker 配置 (静态存储)
 * @retval      0:  成功
 *             -1:  TCP 失败
 *             -2:  CONNACK 失败/超时
 *             -3:  SUBSCRIBE 失败/超时
 */
static int8_t mqtt_full_connect(const mqtt_broker_cfg_t *cfg)
{
    uint16_t  len;
    int32_t   rx_ret;

    /* ---- ① TCP 连接 ---- */
    g_state = STATE_TCP_CONNECTING;
    g_net_state = NET_STATE_CONNECTING;
    if (transport_connect(MQTT_SOCKET, (uint8_t *)cfg->server_ip,
                          cfg->server_port) != 0) {
        transport_disconnect(MQTT_SOCKET);
        g_state = STATE_DISCONNECTED;
        return -1;
    }

    printf("[MQTT] TCP connected, sending CONNECT...\r\n");

    /* ---- ② 组装并发送 CONNECT ---- */
    {
        mqtt_conn_t conn = {0};
        conn.client_id     = cfg->client_id;
        conn.username      = cfg->username;
        conn.password      = cfg->password;
        conn.keep_alive    = cfg->keep_alive;
        conn.clean_session = 1;

        len = mqtt_build_connect(g_tx_buf, &conn);
    }
    if (len == 0) {
        transport_disconnect(MQTT_SOCKET);
        g_state = STATE_DISCONNECTED;
        return -2;
    }
    if (transport_send(MQTT_SOCKET, g_tx_buf, len) < 0) {
        goto conn_fail;
    }

    /* ---- ③ 等待 CONNACK (轮询 5s) ---- */
    g_state = STATE_WAIT_CONNACK;
    g_connack_rc = 0xFF;

    {
        uint32_t elapsed = 0;
        while (elapsed < MQTT_CONNACK_TIMEOUT) {
            rx_ret = transport_recv(MQTT_SOCKET, g_rx_buf,
                                    MQTT_RECV_BUF_SIZE, 500);
            if (rx_ret > 0) {
                printf("[MQTT] recv %ld bytes, parse...\r\n", rx_ret);
                int32_t p = mqtt_parse(g_rx_buf, (uint16_t)rx_ret,
                           _cb_connack, NULL, NULL);
                printf("[MQTT] parse ret=%ld connack=%d\r\n", p, g_connack_rc);
                if (g_connack_rc != 0xFF) break;   /* 已收到 */
            } else if (rx_ret < 0) {
                goto conn_fail;                     /* 连接断开 */
            }
            elapsed += 500;
        }
    }

    if (g_connack_rc != MQTT_CONN_ACCEPTED) {
        printf("[MQTT] CONNACK rejected, rc=%d\r\n", g_connack_rc);
        goto conn_fail;
    }

    /* ---- ④ 订阅指令主题 (如果配置) ---- */
    if (cfg->sub_topic && cfg->sub_topic[0]) {
        g_pkt_id++;
        if (g_pkt_id == 0) g_pkt_id = 1;           /* 0 是保留值 */

        len = mqtt_build_subscribe(g_tx_buf, cfg->sub_topic, g_pkt_id);
        if (len == 0) goto conn_fail;

        if (transport_send(MQTT_SOCKET, g_tx_buf, len) < 0) {
            goto conn_fail;
        }

        /* ---- ⑤ 等待 SUBACK (轮询 5s) ---- */
        g_state = STATE_WAIT_SUBACK;
        g_suback_rc = 0xFF;

        {
            uint32_t elapsed = 0;
            while (elapsed < MQTT_SUBACK_TIMEOUT) {
                rx_ret = transport_recv(MQTT_SOCKET, g_rx_buf,
                                        MQTT_RECV_BUF_SIZE, 500);
                if (rx_ret > 0) {
                    mqtt_parse(g_rx_buf, (uint16_t)rx_ret,
                               NULL, NULL, _cb_suback);
                    if (g_suback_rc != 0xFF) break;
                } else if (rx_ret < 0) {
                    goto conn_fail;
                }
                elapsed += 500;
            }
        }
    }

    /* SUBACK 拒绝 (0x80) → 订阅失败, 但主连接正常, 继续 */
    if (g_suback_rc == 0x80) {
        printf("[MQTT] SUBACK rejected (0x80)\r\n");
    }

    g_state = STATE_CONNECTED;
    g_net_state = NET_STATE_CONNECTED;
    g_last_ping = xTaskGetTickCount();
    g_last_pub  = xTaskGetTickCount();
    return 0;

conn_fail:
    transport_disconnect(MQTT_SOCKET);
    g_state = STATE_DISCONNECTED;
    g_net_state = NET_STATE_DISCONNECTED;
    return -2;
}

/* ================================================================================
 * 公开接口 — mqtt_task_run
 * ================================================================================ */

/**
 * @brief       MQTT 客户端主循环 (FreeRTOS 任务入口)
 * @note        永不返回, 内部无限重连
 *              每 10s 自动读取 g_sensor_temp / g_sensor_humi 上报
 */
void mqtt_task_run(const mqtt_broker_cfg_t *cfg, mqtt_on_cmd_t on_cmd,
                   QueueHandle_t sensor_queue)
{
    int32_t  rx_ret;
    uint16_t len;

    if (!cfg) return;

    g_on_cmd = on_cmd;                              /* 注册指令回调 */

    while (1)
    {
        /* ================================================================
         * ① 建连: TCP → MQTT CONNECT → SUBSCRIBE
         * ================================================================ */
        while (mqtt_full_connect(cfg) != 0) {
            printf("[MQTT] connect to %d.%d.%d.%d:%d failed\r\n",
                   cfg->server_ip[0], cfg->server_ip[1],
                   cfg->server_ip[2], cfg->server_ip[3],
                   cfg->server_port);
            /* 等待 PHY 链路恢复 (monitor_task 给信号量) */
            if (g_net_ready_sem) {
                xSemaphoreTake(g_net_ready_sem, pdMS_TO_TICKS(5000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(MQTT_RECONNECT_DELAY));
            }
        }

        /* ================================================================
         * ② 主收发循环
         * ================================================================ */
        while (g_state == STATE_CONNECTED)
        {
            /* ---- PINGREQ 保活 (keep_alive / 1.5 间隔) ---- */
            if ((xTaskGetTickCount() - g_last_ping) >=
                pdMS_TO_TICKS(MQTT_PING_INTERVAL_MS))
            {
                len = mqtt_build_pingreq(g_tx_buf);
                if (transport_send(MQTT_SOCKET, g_tx_buf, len) < 0) {
                    break;                              /* 发送失败 → 重连 */
                }
                g_last_ping = xTaskGetTickCount();
            }

            /* ---- PUBLISH 上报温湿度 ---- */
            if ((xTaskGetTickCount() - g_last_pub) >=
                pdMS_TO_TICKS(MQTT_PUB_INTERVAL_MS))
            {
                sensor_data_t sensor = {0, 0};
                /* 取最新一条, 丢弃积压的旧数据 */
                while (xQueueReceive(sensor_queue, &sensor, 0) == pdTRUE) {}

                if (cfg->build_payload) {
                    uint8_t json_payload[128];
                    int32_t json_len = cfg->build_payload(
                        json_payload, sizeof(json_payload),
                        sensor.temp, sensor.humi);
                    if (json_len > 0) {
                        printf("[MQTT] pub: %s\r\n", json_payload);
                        /* mqtt_build_publish 向 g_tx_buf 写固定头,
                           payload 用独立缓冲区 json_payload, 避免覆盖 */
                        len = mqtt_build_publish(g_tx_buf, cfg->pub_topic,
                                                 json_payload,
                                                 (uint16_t)json_len);
                        int32_t snd = transport_send(MQTT_SOCKET, g_tx_buf, len);
                        printf("[MQTT] sent %ld/%d bytes to %s\r\n",
                               snd, len, cfg->pub_topic);
                    }
                }
                g_last_pub = xTaskGetTickCount();
            }

            /* ---- 接收服务器下发 (非阻塞 500ms) ---- */
            rx_ret = transport_recv(MQTT_SOCKET, g_rx_buf,
                                    MQTT_RECV_BUF_SIZE, MQTT_LOOP_RECV_TO);

            if (rx_ret > 0) {
                /* 解析 → CONNACK/SUBACK/PUBLISH/PINGRESP 均在此处理 */
                mqtt_parse(g_rx_buf, (uint16_t)rx_ret,
                           NULL, _cb_publish, NULL);
            } else if (rx_ret < 0) {
                break;                              /* 连接断开 → 重连 */
            }
            /* rx_ret == 0: 超时, 无数据 → 继续 */
        }

        /* ================================================================
         * ③ 断线清理, 等一会再重连
         * ================================================================ */
        transport_disconnect(MQTT_SOCKET);
        g_state = STATE_DISCONNECTED;
        g_net_state = NET_STATE_DISCONNECTED;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
