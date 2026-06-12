/**
 ****************************************************************************************************
 * @file        w5500_port.c
 * @brief       W5500 端口层实现 — 将WIZnet官方库与STM32F103硬件绑定
 * @note        本文件实现了官方库要求的4类回调函数 + W5500完整初始化流程:
 *              1. SPI字节读写 (w5500_spi_rb / w5500_spi_wb)
 *              2. SPI块读写   (w5500_spi_read_burst / w5500_spi_write_burst)
 *              3. 片选控制    (w5500_cs_sel / w5500_cs_desel)
 *              4. 临界区保护  (w5500_cris_enter / w5500_cris_exit)
 *
 * @note        数据流向:
 *              WIZnet官方库(WIZCHIP_READ/WRITE) → w5500.c → WIZCHIP.IF.SPI._xxx → 本文件的回调
 *              → spi2_write_read_byte / HAL_SPI → W5500芯片
 *
 * @note        硬件平台: 正点原子 STM32F103ZET6 + W5500模块
 ****************************************************************************************************
 */

#include "w5500_port.h"
#include "spi.h"
#include "./SYSTEM/delay/delay.h"

/* FreeRTOS */
#include "FreeRTOS.h"
#include "task.h"

/* ================================================================================
 * 第1类回调: SPI 单字节读写
 * --------------------------------------------------------------------------------
 * W5500 的 SPI 数据帧格式 (32位):
 *   高16位 → 地址段 (Address Phase)
 *   后续   → 数据段 (Data Phase)
 *   其中地址段由w5500.c中的WIZCHIP_READ/WIZCHIP_WRITE负责发送,
 *   本回调只负责收发数据段的单个字节
 * ================================================================================ */

/**
 * @brief       SPI读取一个字节 (W5500数据段)
 * @note        向W5500发送0xFF来产生8个SCK时钟, 同时读取MISO线上的数据
 *              W5500在SCK上升沿采样MOSI, 在SCK下降沿更新MISO
 * @retval      W5500返回的数据字节
 */
static uint8_t w5500_spi_rb(void)
{
    return spi2_write_read_byte(0xFF);
}

/**
 * @brief       SPI写入一个字节 (W5500数据段)
 * @param       wb: 要写入的数据字节
 */
static void w5500_spi_wb(uint8_t wb)
{
    spi2_write_read_byte(wb);
}

/* ================================================================================
 * 第2类回调: SPI 块读写 (Burst I/O)
 * --------------------------------------------------------------------------------
 * 当w5500.c检测到_burst函数指针非空时, 会自动使用burst模式,
 * 比逐字节读写效率更高 (减少片选/地址开销)
 * ================================================================================ */

/**
 * @brief       SPI连续读取多个字节
 * @param       pBuf: 接收缓冲区指针
 * @param       len:  要读取的字节数
 */
static void w5500_spi_read_burst(uint8_t* pBuf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        pBuf[i] = spi2_write_read_byte(0xFF);
    }
}

/**
 * @brief       SPI连续写入多个字节
 * @param       pBuf: 要发送的数据缓冲区指针
 * @param       len:  要写入的字节数
 */
static void w5500_spi_write_burst(uint8_t* pBuf, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++)
    {
        spi2_write_read_byte(pBuf[i]);
    }
}

/* ================================================================================
 * 第3类回调: 片选控制
 * --------------------------------------------------------------------------------
 * W5500的SPI协议: 每次操作前CS拉低, 操作完成后CS拉高
 * w5500.c中的WIZCHIP_READ/WRITE函数会自动调用这两个回调
 * ================================================================================ */

/**
 * @brief       W5500 片选选中 (CS = 低电平)
 */
static void w5500_cs_sel(void)
{
    W5500_SCS_LOW();
}

/**
 * @brief       W5500 片选释放 (CS = 高电平)
 */
static void w5500_cs_desel(void)
{
    W5500_SCS_HIGH();
}

/* ================================================================================
 * 第4类回调: 临界区保护
 * --------------------------------------------------------------------------------
 * W5500 SPI操作期间禁止被中断/任务切换打断, 保证寄存器访问的原子性
 * FreeRTOS环境下使用 taskENTER_CRITICAL / taskEXIT_CRITICAL
 * 裸机环境下使用 __disable_irq / __enable_irq
 * ================================================================================ */

static void w5500_cris_enter(void)
{
    taskENTER_CRITICAL();       /* FreeRTOS: 关中断 + 禁止任务调度 */
}

static void w5500_cris_exit(void)
{
    taskEXIT_CRITICAL();        /* FreeRTOS: 恢复中断 + 允许任务调度 */
}

/* ================================================================================
 * 回调注册函数
 * ================================================================================ */

/**
 * @brief       将上述所有回调函数注册到WIZnet官方库的全局WIZCHIP结构体中
 * @note        必须先调用此函数, 然后才能使用wizchip_init()等官方API
 */
void w5500_register_callbacks(void)
{
    /* SPI单字节读写 */
    reg_wizchip_spi_cbfunc(w5500_spi_rb, w5500_spi_wb);

    /* SPI块读写 (burst模式, 可选但推荐, 提高吞吐量) */
    reg_wizchip_spiburst_cbfunc(w5500_spi_read_burst, w5500_spi_write_burst);

    /* 片选控制 */
    reg_wizchip_cs_cbfunc(w5500_cs_sel, w5500_cs_desel);

    /* 临界区保护 */
    reg_wizchip_cris_cbfunc(w5500_cris_enter, w5500_cris_exit);
}

/* ================================================================================
 * W5500 硬件操作
 * ================================================================================ */

/**
 * @brief       W5500 硬件复位
 * @note        复位时序: RST拉低 ≥500μs → 拉高 → 等待至少1ms让W5500内部PLL锁定
 *              复位后W5500所有寄存器恢复默认值
 */
void w5500_hardware_reset(void)
{
    W5500_RST_LOW();                    /* 进入复位 */
    delay_us(600);                      /* 保持低电平 > 500μs (满足W5500数据手册要求) */
    W5500_RST_HIGH();                   /* 退出复位 */
    delay_ms(10);                       /* 等待W5500内部PLL锁定 + 寄存器初始化 */
}

/* ================================================================================
 * W5500 完整初始化
 * ================================================================================ */

/**
 * @brief       W5500 完整初始化流程
 * @note        执行顺序:
 *              1. 硬件层: 初始化SPI2 + 控制引脚
 *              2. 回调层: 注册所有回调函数到官方库
 *              3. 复位层: 硬件复位 → 软件复位(wizchip_init内部)
 *              4. 芯片层: 配置8个Socket的TX/RX缓冲区大小
 *              5. 网络层: 设置MAC/IP/子网掩码/网关
 *              6. PHY层:  设置自动协商模式
 *              7. 速度层: 将SPI时钟从默认低速切换到高速
 * @retval      0: Link Up (网线已连接)
 *              1: Link Down (网线未连接或网络故障, 但W5500芯片初始化成功)
 *              2: 初始化失败
 */
uint8_t w5500_init(void)
{
    uint8_t tx_buf_sizes[8];        /* Socket 0~7 发送缓冲区大小 (单位: KB) */
    uint8_t rx_buf_sizes[8];        /* Socket 0~7 接收缓冲区大小 (单位: KB) */
    wiz_NetInfo net_info;           /* 网络配置信息 */
    wiz_PhyConf phy_conf;           /* PHY配置 */
    int8_t ret;

    /* ----- 第1步: 硬件层初始化 ----- */
    spi2_init();                                    /* SPI2 + CS/RST/INT GPIO初始化 */

    /* ----- 第2步: 注册回调 ----- */
    w5500_register_callbacks();                     /* SPI读写/CS/临界区 回调注册 */

    /* ----- 第3步: 硬件复位 ----- */
    w5500_hardware_reset();                         /* 物理复位W5500芯片 */

    /* ----- 第4步: 芯片初始化 (内部含软件复位) ----- */
    /* W5500 共有 32KB 收发缓冲区, 分配给 8 个 Socket
     * 这里采用默认分配: 每个Socket 2KB TX + 2KB RX = 4KB × 8 = 32KB
     * 如果有特殊需求可以自定义, 例如:
     *   Socket 0 (TCP): 8KB TX + 8KB RX = 16KB
     *   Socket 1~7:     2KB TX + 2KB RX =  4KB each
     *   总和: 16KB + 4KB × 7 = 44KB → 超过32KB, 会初始化失败!
     *   所以要确保 tx_buf总和 ≤ 16KB, rx_buf总和 ≤ 16KB
     */
    for (uint8_t i = 0; i < 8; i++)
    {
        tx_buf_sizes[i] = 2;    /* 每个Socket TX = 2KB */
        rx_buf_sizes[i] = 2;    /* 每个Socket RX = 2KB */
    }
    ret = wizchip_init(tx_buf_sizes, rx_buf_sizes);  /* 内部会调用 wizchip_sw_reset() */
    if (ret != 0)
    {
        return 2;   /* 缓冲区大小配置失败 */
    }

    /* ----- 第5步: 设置网络信息 ----- */
    net_info.mac[0] = 0x00;
    net_info.mac[1] = 0x08;
    net_info.mac[2] = 0xDC;
    net_info.mac[3] = 0x12;
    net_info.mac[4] = 0x34;
    net_info.mac[5] = 0x56;                             /* MAC: 00:08:DC:12:34:56 */
    net_info.ip[0]  = 192; net_info.ip[1]  = 168;
    net_info.ip[2]  = 1;   net_info.ip[3]  = 100;       /* IP:  192.168.1.100 */
    net_info.sn[0]  = 255; net_info.sn[1]  = 255;
    net_info.sn[2]  = 255; net_info.sn[3]  = 0;         /* SN:  255.255.255.0 */
    net_info.gw[0]  = 192; net_info.gw[1]  = 168;
    net_info.gw[2]  = 1;   net_info.gw[3]  = 1;         /* GW:  192.168.1.1 */
    net_info.dns[0] = 8;   net_info.dns[1] = 8;
    net_info.dns[2] = 8;   net_info.dns[3] = 8;         /* DNS: 8.8.8.8 */
    net_info.dhcp    = NETINFO_STATIC;                   /* 静态IP */
    wizchip_setnetinfo(&net_info);

    /* ----- 第6步: PHY配置 (自动协商) ----- */
    /* 设置自动协商模式: W5500自动与对端协商速率(10/100M)和双工模式(半/全) */
    phy_conf.by     = PHY_CONFBY_SW;        /* 通过软件配置PHY */
    phy_conf.mode   = PHY_MODE_AUTONEGO;    /* 自动协商 */
    phy_conf.speed  = PHY_SPEED_100;        /* 默认100M (协商时优先) */
    phy_conf.duplex = PHY_DUPLEX_FULL;      /* 默认全双工 (协商时优先) */
    wizphy_setphyconf(&phy_conf);

    /* ----- 第7步: 切换SPI到高速 ----- */
    /* W5500最大SPI时钟80MHz, F103最高36MHz (APB1=36MHz, 分频/2 → 18MHz或不分频→36MHz)
     * SPI_SPEED_2: PCLK1/4 = 36MHz/4 = 9MHz, 稳定可靠
     * SPI_SPEED_4: PCLK1/8 = 36MHz/8 = 4.5MHz, 更保守
     * 建议先以SPI_SPEED_2(9MHz)运行, 如果通信不稳定则降低 */
    spi2_set_speed(SPI_SPEED_2);

    /* ----- 第8步: 等待PHY Link Up (超时5秒) ----- */
    for (uint16_t i = 0; i < 500; i++)
    {
        delay_ms(10);
        if (w5500_get_link_status() == PHY_LINK_ON)
        {
            return 0;   /* Link Up! 网线已连接, W5500完全就绪 */
        }
    }

    return 1;   /* 超时: W5500芯片正常但网线未检测到 */
}

/* ================================================================================
 * 网络配置 & PHY状态
 * ================================================================================ */

/**
 * @brief       设置网络信息 (MAC/IP/掩码/网关/DNS)
 * @param       mac[6]: MAC地址
 * @param       ip[4]:  IP地址
 * @param       sn[4]:  子网掩码
 * @param       gw[4]:  网关地址
 * @note        调用此函数前需要先完成 w5500_init()
 */
void w5500_set_network(uint8_t mac[6], uint8_t ip[4], uint8_t sn[4], uint8_t gw[4])
{
    wiz_NetInfo net_info;
    for (uint8_t i = 0; i < 6; i++) net_info.mac[i] = mac[i];
    for (uint8_t i = 0; i < 4; i++) net_info.ip[i]  = ip[i];
    for (uint8_t i = 0; i < 4; i++) net_info.sn[i]  = sn[i];
    for (uint8_t i = 0; i < 4; i++) net_info.gw[i]  = gw[i];
    net_info.dns[0] = 8; net_info.dns[1] = 8;
    net_info.dns[2] = 8; net_info.dns[3] = 8;
    net_info.dhcp    = NETINFO_STATIC;
    wizchip_setnetinfo(&net_info);
}

/**
 * @brief       获取PHY链路状态
 * @retval      PHY_LINK_ON (1): 网线已插入, 链路建立
 *              PHY_LINK_OFF (0): 网线未插入或链路断开
 */
uint8_t w5500_get_link_status(void)
{
    wiz_PhyConf phy_stat;
    wizphy_getphystat(&phy_stat);
    /* phy_stat.duplex: 0=Half, 1=Full, 仅在Link Up时有效 */
    /* wizphy_getphylink() 直接返回 PHY_LINK_ON 或 PHY_LINK_OFF 更简洁 */
    return (uint8_t)wizphy_getphylink();
}
