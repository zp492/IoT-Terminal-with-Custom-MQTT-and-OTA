/**
 ****************************************************************************************************
 * @file        transport.h
 * @brief       MQTT 传输层 — 基于 W5500 Socket API 的收发封装
 * @note        为上层 MQTT 协议栈提供与平台无关的 TCP 传输接口
 *              实现位于 transport.c
 *
 *              接口设计原则:
 *              - 上层不需要感知 SPI / W5500 / Socket 号
 *              - send 阻塞 (MQTT 报文小, 不易阻塞)
 *              - recv 非阻塞轮询 (带超时, 不卡死任务)
 *              - connect 非阻塞 (带超时自动重试)
 ****************************************************************************************************
 */

#ifndef __TRANSPORT_H
#define __TRANSPORT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief       建立 TCP 连接到远端服务器 (非阻塞, 带超时)
 * @param       sn:    W5500 Socket 号 (0~7)
 * @param       ip:    服务器 IPv4 地址 (4 字节大端, 如 {192,168,1,4})
 * @param       port:  服务器端口号
 * @retval      0: 连接成功
 *             -1: 超时
 *             -2: 参数/Socket 错误
 */
int8_t transport_connect(uint8_t sn, uint8_t ip[4], uint16_t port);

/**
 * @brief       断开 TCP 连接并关闭 Socket
 * @param       sn: Socket 号
 */
void transport_disconnect(uint8_t sn);

/**
 * @brief       发送数据 (阻塞, 直到全部发出或出错)
 * @param       sn:   Socket 号
 * @param       buf:  发送缓冲区
 * @param       len:  发送字节数
 * @retval      >=0:  实际发送字节数
 *              <0:   发送失败
 */
int32_t transport_send(uint8_t sn, const uint8_t *buf, uint16_t len);

/**
 * @brief       接收数据 (非阻塞轮询, 带超时)
 * @param       sn:         Socket 号
 * @param       buf:        接收缓冲区
 * @param       len:        期望接收的最大字节数
 * @param       timeout_ms: 超时时间 (ms)
 * @retval      >0:  实际收到字节数
 *              0:   超时无数据
 *              <0:  连接断开 / 错误
 */
int32_t transport_recv(uint8_t sn, uint8_t *buf, uint16_t len,
                       uint32_t timeout_ms);

/**
 * @brief       检查 Socket 是否处于连接状态
 * @param       sn: Socket 号
 * @retval      1: 已连接
 *              0: 未连接
 */
uint8_t transport_is_connected(uint8_t sn);

#ifdef __cplusplus
}
#endif

#endif /* __TRANSPORT_H */
