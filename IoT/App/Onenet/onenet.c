/**
 ****************************************************************************************************
 * @file        onenet.c
 * @brief       OneNET 云平台 MQTT 适配层 实现
 * @note        OneNET MQTT 接入文档:
 *              https://iot.10086.cn/doc/aiot/fuse/detail/920
 *
 *              三元组: Product ID / Device ID / Auth Token
 *
 *              MQTT 连接参数:
 *              服务器:  183.230.40.96 : 1883
 *              ClientID = Device ID
 *              Username = Product ID
 *              Password = Token (平台工具生成)
 *
 *              数据上报 Topic:
 *              $sys/{pid}/{did}/dp/post/json
 *
 *              指令接收 Topic:
 *              $sys/{pid}/{did}/#
 ****************************************************************************************************
 */

#include "onenet.h"
#include "mqtt_wrapper.h"
#include <string.h>
#include <stdio.h>                              /* snprintf */

/* ================================================================================
 * 三元组 (静态存储, 由 onenet_set_auth 写入)
 * ================================================================================ */
static const char *g_pid   = NULL;
static const char *g_did   = NULL;
static const char *g_token = NULL;
static uint32_t    g_msg_id = 0;

/* ================================================================================
 * 公开接口 实现
 * ================================================================================ */

/**
 * @brief       配置 OneNET 三元组
 */
void onenet_set_auth(const char *pid, const char *did, const char *token)
{
    g_pid   = pid;
    g_did   = did;
    g_token = token;
}

/**
 * @brief       构建 OneNET 数据上报 JSON
 * --------------------------------------------------------------------------------
 * OneNET 物模型 JSON 格式:
 * {
 *   "id": 123,
 *   "dp": {
 *     "temp": [{"v": 25}],
 *     "humi": [{"v": 55}]
 *   }
 * }
 *
 * id 字段为消息序号, 这里用固定值 123 (平台不校验该字段)
 * dp 下每个数据流的名称需与 OneNET 平台上创建的 "数据流模板" 一致
 *
 * @param       buf:     输出缓冲区
 * @param       max_len: 缓冲区容量
 * @param       temp:    温度 (℃)
 * @param       humi:    湿度 (%)
 * @retval      写入字节数
 */
uint16_t onenet_build_payload(uint8_t *buf, uint16_t max_len,
                               uint8_t temp, uint8_t humi)
{
    int len = snprintf((char *)buf, max_len,
        "{\"id\":%lu,"
        "\"dp\":{"
          "\"temp\":[{\"v\":%d}],"
          "\"humi\":[{\"v\":%d}]"
        "}}",
        (unsigned long)++g_msg_id, temp, humi);
    return (len > 0) ? (uint16_t)len : 0;
}

/**
 * @brief       填充 Broker 配置 → OneNET MQTT 服务器
 * @note        调用前需先 onenet_set_auth() 设置三元组
 *              服务器 IP / Port 为 OneNET 公开接入点
 *              Topic 格式按 OneNET 规范拼装
 */
void onenet_fill_cfg(mqtt_broker_cfg_t *cfg)
{
    if (!cfg || !g_pid || !g_did) return;

    /* ---- 服务器地址 ---- */
    cfg->server_ip[0] = 183;
    cfg->server_ip[1] = 230;
    cfg->server_ip[2] = 40;
    cfg->server_ip[3] = 96;
    cfg->server_port  = 1883;                   /* OneNET IoT Suite */

    /* ---- MQTT 认证 ---- */
    cfg->client_id = g_did;                     /* Device Name */
    cfg->username  = g_pid;                     /* Product ID */
    cfg->password  = g_token;                   /* Auth Token */
    cfg->keep_alive = 60;                       /* 保活 60s */

    /* ---- Topic (使用静态缓冲区拼装, 调用方需保证不释放) ---- */
    static char pub_topic[64];
    static char sub_topic[64];

    snprintf(pub_topic, sizeof(pub_topic),
             "$sys/%s/%s/dp/post/json", g_pid, g_did);
    snprintf(sub_topic, sizeof(sub_topic),
             "$sys/%s/%s/#", g_pid, g_did);

    cfg->pub_topic = pub_topic;
    cfg->sub_topic = sub_topic;
}
