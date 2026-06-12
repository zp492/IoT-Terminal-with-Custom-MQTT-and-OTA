#ifndef __W5500_PORT_H
#define __W5500_PORT_H

#include "./SYSTEM/sys/sys.h"
#include "wizchip_conf.h"

/* ================================================================================
 * W5500 端口层 - 胶水代码
 * --------------------------------------------------------------------------------
 * 将WIZnet官方库的回调函数指针与STM32F103的SPI2硬件绑定,
 * 并完成W5500芯片初始化/网络配置/PHY状态检测
 * ================================================================================ */

/* ---- 回调注册函数 (注册到WIZnet官方库) ---- */
void w5500_register_callbacks(void);

/* ---- W5500初始化 ---- */
uint8_t w5500_init(void);

/* ---- W5500硬件复位 ---- */
void w5500_hardware_reset(void);

/* ---- 网络信息设置 ---- */
void w5500_set_network(uint8_t mac[6], uint8_t ip[4], uint8_t sn[4], uint8_t gw[4]);

/* ---- PHY状态查询 ---- */
uint8_t w5500_get_link_status(void);

#endif
