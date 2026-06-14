/**
 ****************************************************************************************************
 * @file        tcp_client.c
 * @brief       W5500 TCP 客户端 — 阻塞模式, 断线自动重连
 * @note        使用 WIZnet 官方 socket API (socket.h)
 *              Socket 0, 阻塞 IO, TCP 协议
 *              在 FreeRTOS 任务中运行, 定期上报心跳并处理服务器下发
 ****************************************************************************************************
 */

#include "tcp_client.h"
#include "socket.h"
#include "w5500_port.h"
#include "./SYSTEM/delay/delay.h"
#include "./SYSTEM/usart/usart.h"

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"

/* ================================================================================
 * 配置宏
 * ================================================================================ */

#define TCP_SOCKET_NUM 0         /* 使用 Socket 0 */
#define TCP_LOCAL_PORT 5000      /* 本地端口 (客户端源端口, 任意) */
#define TCP_SEND_INTERVAL 1000   /* 心跳上报间隔 (ms) */
#define TCP_RECV_TIMEOUT 3000    /* recv 超时 (ms) */
#define TCP_RECONNECT_DELAY 3000 /* 断线后重连间隔 (ms) */
#define TCP_BUF_SIZE 512         /* 收发缓冲区 */

/* ================================================================================
 * 静态变量
 * ================================================================================ */

static uint8_t g_server_ip[4] = {192, 168,1, 4}; /* 默认服务器: 用户PC */
static uint16_t g_server_port = 8080;               /* 默认端口 */
static uint8_t g_tx_buf[TCP_BUF_SIZE];              /* 发送缓冲区 */
static uint8_t g_rx_buf[TCP_BUF_SIZE];              /* 接收缓冲区 */
static uint32_t g_send_count = 0;                   /* 发包计数 */

/* ================================================================================
 * 公开接口
 * ================================================================================ */

/**
 * @brief       设置目标服务器·
 */
void tcp_client_set_server(uint8_t ip[4], uint16_t port)
{
    for (uint8_t i = 0; i < 4; i++)
        g_server_ip[i] = ip[i];
    g_server_port = port;
}

/* ================================================================================
 * 内部函数
 * ================================================================================ */

/**
 * @brief       TCP 连接 (阻塞, 带超时)
 * @retval      0: 成功 / -1: 失败
 */
static int8_t tcp_client_connect(void)
{
    int8_t ret;
    uint8_t sock_status;

    /* 1. 打开 TCP Socket */
    ret = socket(TCP_SOCKET_NUM, Sn_MR_TCP, TCP_LOCAL_PORT, SF_TCP_NODELAY);
    if (ret != TCP_SOCKET_NUM)
    {
        printf("[TCP] socket() failed, ret=%d\r\n", ret);
        return -1;
    }
    printf("[TCP] socket opened (sn=%d, local_port=%d)\r\n",
           TCP_SOCKET_NUM, TCP_LOCAL_PORT);

    /* 诊断: 打印本机网络配置 */
    {
        wiz_NetInfo net_info;
        wizchip_getnetinfo(&net_info);
        printf("[TCP] self: MAC=%02X:%02X:%02X:%02X:%02X:%02X  IP=%d.%d.%d.%d  GW=%d.%d.%d.%d\r\n",
               net_info.mac[0], net_info.mac[1], net_info.mac[2],
               net_info.mac[3], net_info.mac[4], net_info.mac[5],
               net_info.ip[0], net_info.ip[1], net_info.ip[2], net_info.ip[3],
               net_info.gw[0], net_info.gw[1], net_info.gw[2], net_info.gw[3]);
    }

    /* 2. 连接服务器 (阻塞, W5x00版 connect 接受3个参数) */
    ret = connect(TCP_SOCKET_NUM, g_server_ip, g_server_port);

    /* 连接后检查socket状态, 提供更精确的错误信息 */
    getsockopt(TCP_SOCKET_NUM, SO_STATUS, &sock_status);

    if (ret != SOCK_OK)
    {
        printf("[TCP] connect() failed, ret=%d (", ret);
        switch (ret) {
            case SOCKERR_SOCKCLOSED:
                printf("SOCK_CLOSED: server unreachable or port closed");
                break;
            case SOCKERR_TIMEOUT:
                printf("TIMEOUT: no response from server");
                break;
            case SOCKERR_IPINVALID:
                printf("IP_INVALID: bad server address");
                break;
            case SOCKERR_SOCKINIT:
                printf("SOCK_INIT: SIPR=0.0.0.0, network not configured");
                break;
            default:
                printf("code=%d", ret);
                break;
        }
        printf("). server=%d.%d.%d.%d:%d, sock_status=0x%02X\r\n",
               g_server_ip[0], g_server_ip[1], g_server_ip[2], g_server_ip[3],
               g_server_port, sock_status);
        close(TCP_SOCKET_NUM);
        return -1;
    }

    printf("[TCP] connected to %d.%d.%d.%d:%d\r\n",
           g_server_ip[0], g_server_ip[1], g_server_ip[2], g_server_ip[3],
           g_server_port);
    return 0;
}

/**
 * @brief       TCP 断开 (主动 FIN)
 */
static void tcp_client_disconnect(void)
{
    uint8_t status;
    getsockopt(TCP_SOCKET_NUM, SO_STATUS, &status);

    if (status == SOCK_ESTABLISHED || status == SOCK_CLOSE_WAIT) // 已连接或关闭等待状态
    {
        disconnect(TCP_SOCKET_NUM); /* 发送 FIN */
    }
    close(TCP_SOCKET_NUM);
    printf("[TCP] disconnected\r\n");
}

/**
 * @brief       发送一条消息
 * @retval      实际发送字节数 / -1: 失败
 */
static int32_t tcp_client_send(uint8_t *data, uint16_t len)
{
    int32_t ret;

    ret = send(TCP_SOCKET_NUM, data, len);

    if (ret < 0)
    {
        printf("[TCP] send() error, ret=%ld\r\n", ret);
    }
    return ret;
}

/**
 * @brief       接收数据 (阻塞, 带超时)
 * @param       buf:   接收缓冲区
 * @param       len:   期望最大长度
 * @param       timeout_ms: 超时时间 (ms)
 * @retval      >0: 实际接收字节数 / 0: 超时无数据 / <0: 错误
 */
static int32_t tcp_client_recv(uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    int32_t ret;
    uint16_t rx_size;
    uint32_t elapsed = 0;

    /* 轮询等待数据到达, 每 100ms 检查一次 */
    while (elapsed < timeout_ms)
    {
        getsockopt(TCP_SOCKET_NUM, SO_RECVBUF, &rx_size);
        if (rx_size > 0)
        {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        elapsed += 100;
    }

    if (elapsed >= timeout_ms)
    {
        return 0; /* 超时, 无数据 */
    }

    /* 限制读取长度不超过接收缓冲区大小 */
    if (rx_size < len)
        len = rx_size;
    ret = recv(TCP_SOCKET_NUM, buf, len);
    return ret;
}

/**
 * @brief       检查 socket 是否处于连接状态
 * @retval      1: 已连接 / 0: 未连接
 */
static uint8_t tcp_client_is_connected(void)
{
    uint8_t status;
    getsockopt(TCP_SOCKET_NUM, SO_STATUS, &status);
    return (status == SOCK_ESTABLISHED);
}

/* ================================================================================
 * 主循环 (供 FreeRTOS 任务调用)
 * ================================================================================ */

/**
 * @brief       TCP 客户端主循环
 * @note        供 FreeRTOS 任务函数调用, 内部为无限循环
 *              完整流程:
 *              1. W5500 Link Up 检测
 *              2. socket → connect
 *              3. 进入 发送/接收 循环
 *              4. 断线 → close → 延时 → 重连
 */
void tcp_client_run(void *pvParameters)
{
    int32_t rx_len;
    uint8_t connected;
    uint32_t last_send_tick = 0;

    /* 消除未使用参数警告 */
    (void)pvParameters;

    /* ---- 等待 W5500 Link Up ---- */
    printf("[TCP] waiting for W5500 link up...\r\n");
    while (w5500_get_link_status() != PHY_LINK_ON)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    printf("[TCP] W5500 link up! server = %d.%d.%d.%d:%d\r\n",
           g_server_ip[0], g_server_ip[1], g_server_ip[2], g_server_ip[3],
           g_server_port);

    /* ---- 主循环 ---- */
    while (1)
    {
        /* 第1步: 连接服务器 */
        printf("[TCP] connecting...\r\n");
        while (tcp_client_connect() != 0)
        {
            printf("[TCP] reconnect in %d ms...\r\n", TCP_RECONNECT_DELAY);
            vTaskDelay(pdMS_TO_TICKS(TCP_RECONNECT_DELAY));
        }

        connected = 1;
        last_send_tick = xTaskGetTickCount();

        /* 第2步: 收发循环 */
        while (connected)
        {
            /* ---- 检查连接状态 ---- */
            if (!tcp_client_is_connected())
            {
                printf("[TCP] connection lost!\r\n");
                connected = 0;
                break;
            }

            /* ---- 定时发送心跳 ---- */
            if ((xTaskGetTickCount() - last_send_tick) >=
                pdMS_TO_TICKS(TCP_SEND_INTERVAL))
            {
                last_send_tick = xTaskGetTickCount();

                /* 构造心跳消息 */
                int32_t msg_len = snprintf((char *)g_tx_buf, TCP_BUF_SIZE,
                                           "W5500 TCP Heartbeat #%lu\r\n",
                                           (unsigned long)g_send_count++);

                int32_t ret = tcp_client_send(g_tx_buf, (uint16_t)msg_len);
                if (ret > 0)
                {
                    printf("[TCP] sent: %s", g_tx_buf);
                }
            }

            /* ---- 接收处理 ---- */
            rx_len = tcp_client_recv(g_rx_buf, TCP_BUF_SIZE - 1, 100);
            if (rx_len > 0)
            {
                g_rx_buf[rx_len] = '\0'; /* 添加字符串结尾 */
                printf("[TCP] recv (%ld bytes): %s\r\n", rx_len, g_rx_buf);

                /* TODO: 在此解析服务器下发的指令 */
            }
            else if (rx_len < 0)
            {
                printf("[TCP] recv error, ret=%ld\r\n", rx_len);
                connected = 0;
                break;
            }
            /* rx_len == 0: 超时无数据, 继续循环 */
        }

        /* 第3步: 清理并等待重连 */
        tcp_client_disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
