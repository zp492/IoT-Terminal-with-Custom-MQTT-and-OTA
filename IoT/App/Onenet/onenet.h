/**
 ****************************************************************************************************
 * @file        onenet.h
 * @author      zp492
 * @brief       OneNET 云平台 MQTT 适配层
 * @note        职责: 三元组认证 / Topic 拼装 / JSON 载荷构建
 *              不依赖 mqtt_wrapper 以外的任何模块, 纯 C
 ****************************************************************************************************
 */

#ifndef __ONENET_H
#define __ONENET_H

#include <stdint.h>
#include "mqtt_wrapper.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================================
 * 公共接口
 * ================================================================================ */

/**
 * @brief       配置 OneNET 三元组
 * @note        调用此函数后, onenet_fill_cfg() 将自动使用三元组填充 Broker 配置
 * @param       pid:   产品 ID (Product ID)
 * @param       did:   设备 ID (Device ID)
 * @param       token: 鉴权 Token
 */
void onenet_set_auth(const char *pid, const char *did, const char *token);

/**
 * @brief       构建 OneNET 数据上报 JSON 载荷
 * @note        OneNET 物模型格式:
 *              {"id":123,"dp":{"temp":[{"v":25}],"humi":[{"v":55}]}}
 * @param       buf:     输出缓冲区
 * @param       max_len: 缓冲区大小 (建议 ≥ 128)
 * @param       temp:    温度 (℃)
 * @param       humi:    湿度 (%)
 * @retval      写入的字节数 (不含 '\0')
 */
uint16_t onenet_build_payload(uint8_t *buf, uint16_t max_len,
                               uint8_t temp, uint8_t humi);

/**
 * @brief       将 Broker 配置填充为 OneNET MQTT 服务器参数
 * @note        参数含义见 mqtt_wrapper.h 中 mqtt_broker_cfg_t
 * @param       cfg:  输出参数, 指向待填充的 Broker 配置
 */
void onenet_fill_cfg(mqtt_broker_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* __ONENET_H */
