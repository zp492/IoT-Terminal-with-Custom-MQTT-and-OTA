# 自研MQTT协议栈与OTA远程升级的物联网终端

## 项目简介
基于 STM32F103 + W5500 + FreeRTOS 的温湿度监控终端，自研轻量 MQTT 协议栈，支持远程 OTA 升级，通过 OneNET 云平台实现数据可视化和反向控制。

## 硬件平台
- MCU: STM32F103C8T6
- 以太网: W5500 (硬件 TCP/IP)
- 传感器: DHT22
- 其他: 板载 LED

## 软件架构
- 操作系统: FreeRTOS (多任务调度)
- 自研 MQTT 客户端 (QoS 0/1, 心跳, 重连)
- 自研 Bootloader (Flash 分区, 固件校验, 安全跳转)
- OTA: HTTP 下载 + 校验 + 自动切换

## 目录结构
- `Application/` : 应用任务
- `Middleware/` : 自研 MQTT 库
- `BSP/` : 板级驱动
- `Documents/` : 设计文档与图表

## 快速开始
1. 使用 Keil MDK 打开 `Build/IoT-Terminal.uvprojx`
2. 编译下载到 STM32F103 开发板
3. 连接 W5500 网线，复位设备
4. 登录 OneNET 查看数据上报

## 设计亮点
- [ ] 自研 MQTT 协议栈，支持 QoS 0/1
- [ ] 硬件 Socket 直驱，内存占用降低 40%
- [ ] OTA 远程升级，断网断电保护
- [ ] FreeRTOS 任务间通信：队列 + 信号量
- [ ] 完善文档，架构图、时序图齐全

## 演示视频
[点击观看](链接)

## 联系方式
...
