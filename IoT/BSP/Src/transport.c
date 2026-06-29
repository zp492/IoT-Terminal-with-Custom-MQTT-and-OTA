/**
 ****************************************************************************************************
 * @file        transport.c
 * @author      zp492
 * @brief       MQTT 传输层 — W5500 Socket API 封装
 * @note        基于 WIZnet 官方 socket.h, 与 MQTT 协议层完全解耦
 *
 *              数据流向:
 *              MQTT 层 → transport_send() → socket.send() → W5500 → 网络
 *              网络  → W5500 → socket.recv() → transport_recv() → MQTT 层
 *
 *              Socket 约定: MQTT 使用 Socket 1, 与 TCP 测试端 (Socket 0) 隔离
 ****************************************************************************************************
 */

#include "transport.h"
#include "socket.h"
#include "w5500.h"           /* getSn_RX_RSR() */
#include "./SYSTEM/delay/delay.h"
#include "./SYSTEM/usart/usart.h"                   /* printf */

/* FreeRTOS — 非阻塞接收需要 task delay */
#include "FreeRTOS.h"
#include "task.h"

/* ================================================================================
 * 配置常量
 * ================================================================================ */
#define TRANSPORT_SOCKET_NUM    1               /* MQTT 独占 Socket 1 */

/* 连接超时参数 */
#define TRANSPORT_CONNECT_TIMEOUT_MS   5000     /* 连接总超时 (ms) */
#define TRANSPORT_CONNECT_POLL_MS      100      /* 连接状态轮询间隔 (ms) */

/* recv 轮询间隔 */
#define TRANSPORT_RECV_POLL_MS         50       /* SO_RECVBUF 检查间隔 (ms) */

/* 传输层内部缓冲区 — 作为 socket.send/recv 与 MQTT 报文缓冲区之间的桥梁 */
#define TRANSPORT_BUF_SIZE             512

/* ================================================================================
 * 公开接口 实现
 * ================================================================================ */

/**
 * @brief       建立 TCP 连接 (非阻塞, 带超时)
 * --------------------------------------------------------------------------------
 * 流程:
 *   1. 创建 TCP Socket
 *   2. 发起 connect (阻塞模式, 但 W5500 内部有超时机制)
 *   3. 轮询 SO_STATUS 等待 SOCK_ESTABLISHED
 *   4. 超时未连接 → close socket → 返回 -1
 *
 * @param       sn:    Socket 号
 * @param       ip:    服务器 IPv4 地址
 * @param       port:  服务器端口
 * @retval      0:  连接成功
 *             -1:  超时
 *             -2:  参数/Socket 错误
 */
int8_t transport_connect(uint8_t sn, uint8_t ip[4], uint16_t port)
{
    int8_t   sock_ret;
    uint8_t  status = 0;
    uint32_t elapsed = 0;

    /* ---- 1. 参数校验 ---- */
    if (!ip || port == 0 || sn > 7) return -2;

    /* ---- 2. 打开 TCP Socket (无延迟 ACK, 提高小报文响应速度) ---- */
    sock_ret = socket(sn, Sn_MR_TCP, 0, SF_TCP_NODELAY);
    if (sock_ret != (int8_t)sn) {
        return -2;
    }

    /* ---- 3. 发起连接 (W5x00 connect 为阻塞模式, 但由 W5500 芯片内部处理) ---- */
    sock_ret = connect(sn, ip, port);
    if (sock_ret != SOCK_OK) {
        printf("[TRANSPORT] connect %d.%d.%d.%d:%d err=%d\r\n",
               ip[0], ip[1], ip[2], ip[3], port, sock_ret);
        close(sn);
        return -2;
    }

    /* ---- 4. 轮询等待连接建立 (非阻塞) ---- */
    while (elapsed < TRANSPORT_CONNECT_TIMEOUT_MS) {
        getsockopt(sn, SO_STATUS, &status);

        if (status == SOCK_ESTABLISHED) {
            return 0;                               /* 连接成功 */
        }

        if (status == SOCK_CLOSED) {
            close(sn);
            return -2;                              /* 服务器拒绝 / 网络不可达 */
        }

        vTaskDelay(pdMS_TO_TICKS(TRANSPORT_CONNECT_POLL_MS));
        elapsed += TRANSPORT_CONNECT_POLL_MS;
    }

    /* ---- 5. 超时 ---- */
    close(sn);
    return -1;
}

/**
 * @brief       断开 TCP 连接并关闭 Socket
 * --------------------------------------------------------------------------------
 * 流程:
 *   1. 查询当前状态
 *   2. 如果已连接 → 发送 FIN (disconnect)
 *   3. close socket 释放 W5500 资源
 *
 * @param       sn: Socket 号
 */
void transport_disconnect(uint8_t sn)
{
    uint8_t status;

    getsockopt(sn, SO_STATUS, &status);

    /* 已连接 / 半关闭状态 → 主动断开 */
    if (status == SOCK_ESTABLISHED || status == SOCK_CLOSE_WAIT) {
        disconnect(sn);                             /* 发送 FIN */
    }

    close(sn);                                      /* 关闭 Socket, 释放缓冲区 */
}

/**
 * @brief       发送数据 (阻塞)
 * --------------------------------------------------------------------------------
 * 内部循环调用 socket.send(), 直到全部字节送完或出错
 *
 * @param       sn:   Socket 号
 * @param       buf:  发送缓冲区
 * @param       len:  发送字节数
 * @retval      >=0: 实际发送字节数
 *              <0:  发送失败
 */
int32_t transport_send(uint8_t sn, const uint8_t *buf, uint16_t len)
{
    int32_t ret;

    if (!buf || len == 0) return 0;

    ret = send(sn, (uint8_t *)buf, len);            /* WIZnet send() 不修改 buf */

    if (ret < 0) {
        /* 负值 = SOCKERR_xxx */
        return ret;
    }

    return ret;                                     /* 实际发送字节数 */
}

/**
 * @brief       接收数据 (非阻塞轮询, 带超时)
 * --------------------------------------------------------------------------------
 * 流程:
 *   1. 检查连接状态, 断开则立即返回错误
 *   2. 轮询 SO_RECVBUF (可读字节数)
 *   3. 有数据 → recv 读取
 *   4. 超时 → 返回 0
 *
 * @param       sn:         Socket 号
 * @param       buf:        接收缓冲区
 * @param       len:        期望接收的最大字节数
 * @param       timeout_ms: 超时 (ms)
 * @retval      >0:  实际接收字节数
 *              0:   超时无数据
 *              <0:  连接断开 / 错误
 */
int32_t transport_recv(uint8_t sn, uint8_t *buf, uint16_t len,
                       uint32_t timeout_ms)
{
    uint8_t  status;
    uint16_t rx_size = 0;
    uint32_t elapsed = 0;
    int32_t  ret;

    if (!buf || len == 0) return 0;

    /* ---- 1. 检查连接状态 ---- */
    getsockopt(sn, SO_STATUS, &status);
    if (status != SOCK_ESTABLISHED) {
        return -1;                                  /* 连接已断开 */
    }

    /* ---- 2. 轮询 SO_RECVBUF ---- */
    while (elapsed < timeout_ms) {
        getsockopt(sn, SO_RECVBUF, &rx_size);
        uint16_t raw = getSn_RX_RSR(sn);
        if (raw > 0) {
            if (rx_size == 0) {
                rx_size = raw;
            }
            break;
        }
        if (rx_size > 0) break;

        vTaskDelay(pdMS_TO_TICKS(TRANSPORT_RECV_POLL_MS));
        elapsed += TRANSPORT_RECV_POLL_MS;
    }

    if (rx_size == 0) return 0;

    if (rx_size < len) len = rx_size;
    ret = recv(sn, buf, len);
    return ret;
}

/**
 * @brief       检查 Socket 连接状态
 * @param       sn: Socket 号
 * @retval      1: 已连接
 *              0: 未连接 / 其他状态
 */
uint8_t transport_is_connected(uint8_t sn)
{
    uint8_t status;
    getsockopt(sn, SO_STATUS, &status);
    return (uint8_t)(status == SOCK_ESTABLISHED);
}
