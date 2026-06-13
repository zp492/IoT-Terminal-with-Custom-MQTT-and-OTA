/**
 ****************************************************************************************************
 * @file        tcp_client.h
 * @brief       W5500 TCP 客户端模块
 * @note        基于 WIZnet 官方 socket API 实现:
 *              1. 阻塞模式连接 / 发送 / 接收
 *              2. 断线自动重连
 *              3. 在 FreeRTOS 任务中运行
 ****************************************************************************************************
 */

#ifndef __TCP_CLIENT_H
#define __TCP_CLIENT_H

#include <stdint.h>

/**
 * @brief       设置目标服务器地址和端口
 * @param       ip[4]:  服务器 IPv4 地址 (如 192.168.1.4)
 * @param       port:   服务器端口号
 * @note        必须在 tcp_client_task() 运行前调用
 */
void tcp_client_set_server(uint8_t ip[4], uint16_t port);

/**
 * @brief       TCP 客户端主循环 (供 FreeRTOS 任务调用)
 * @note        内部为无限循环, 自动完成:
 *              socket → connect → send/recv 循环 → 断线重连
 *              占用 Socket 0, 阻塞 IO 模式
 * @param       pvParameters: 任务参数 (未使用, 兼容 FreeRTOS 任务签名)
 */
void tcp_client_run(void *pvParameters);

#endif /* __TCP_CLIENT_H */
