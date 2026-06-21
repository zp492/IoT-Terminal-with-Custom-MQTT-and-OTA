#include "freertos_task.h"
#include "./SYSTEM/usart/usart.h"
#include "./SYSTEM/delay/delay.h"
#include "led.h"
#include "key.h"
#include "./MALLOC/malloc.h"
#include "dht11.h"
#include "lcd.h"
#include "mqtt_wrapper.h"
#include "onenet.h"
#include "w5500_port.h"
/*FreeRTOS*********************************************************************************************/
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

/* ---- 网络就绪信号量 ---- */
SemaphoreHandle_t g_net_ready_sem = NULL;

/* ---- 网络状态 (mqtt_wrapper 更新, led_task 读取) ---- */
volatile uint8_t g_net_state = NET_STATE_DISCONNECTED;

/* ---- 传感器消息队列 (sensor_task → mqtt_task) ---- */
QueueHandle_t g_sensor_queue = NULL;

/*FreeRTOS配置*/

/* START_TASK 任务 配置
 * 包括: 堆栈大小 任务优先级 创建任务 任务句柄
 */
#define START_STACK_SIZE 128
#define START_TSAK_PROI 1
void start_task(void *pvParameters);
TaskHandle_t start_task_handler;

/* SENSOR 任务 配置
 * 堆栈 256 words (1KB) — LCD 字符绘制 + DHT11/ADC 读取
 */
#define SENSOR_STACK_SIZE 512
#define SENSOR_TSAK_PROI  3
TaskHandle_t sensor_task_handler;

/* MQTT 任务 配置
 * 堆栈 1024 words (4KB) — MQTT 状态机 + JSON 组装 + 收发缓冲区
 */
#define MQTT_STACK_SIZE 1024
#define MQTT_TSAK_PROI  4
TaskHandle_t mqtt_task_handler;

/* LED 任务 配置
 * 堆栈 96 words — 仅读状态 + 翻转 GPIO + vTaskDelay
 * 优先级最低 (4), 不影响传感器和 MQTT
 */
#define LED_STACK_SIZE 96
#define LED_TSAK_PROI  1
TaskHandle_t led_task_handler;

/* W5500 Monitor 任务 配置
 * 堆栈 128 words — 轮询 PHY 状态 + 维护信号量
 * 优先级低 (3), 仅比 LED 高, 不阻塞 MQTT/传感器
 */
#define MONITOR_STACK_SIZE 128
#define MONITOR_TSAK_PROI  4
TaskHandle_t w5500_monitor_task_handler;

/**
 * @brief       FreeRTOS例程入口函数
 * @param       无
 * @retval      无
 */
void freertos_demo(void)
{
    /* 在调度器启动前创建内核对象 (不依赖任何任务) */
    g_sensor_queue   = xQueueCreate(SENSOR_QUEUE_LEN, sizeof(sensor_data_t));
    g_net_ready_sem  = xSemaphoreCreateBinary();
    configASSERT(g_sensor_queue != NULL);
    configASSERT(g_net_ready_sem != NULL);

    xTaskCreate(start_task,
                "start_task",
                START_STACK_SIZE,
                NULL,
                START_TSAK_PROI,
                &start_task_handler);

    vTaskStartScheduler(); /*开启任务调度器*/
}

void start_task(void *pvParameters)
{
    taskENTER_CRITICAL(); // 进入临界区
    xTaskCreate(sensor_task,
                "sensor",
                SENSOR_STACK_SIZE,
                NULL,
                SENSOR_TSAK_PROI,
                &sensor_task_handler);
    xTaskCreate(mqtt_task,
                "mqtt",
                MQTT_STACK_SIZE,
                NULL,
                MQTT_TSAK_PROI,
                &mqtt_task_handler);
    xTaskCreate(led_task,
                "led",
                LED_STACK_SIZE,
                NULL,
                LED_TSAK_PROI,
                &led_task_handler);
    xTaskCreate(w5500_monitor_task,
                "w5500_mon",
                MONITOR_STACK_SIZE,
                NULL,
                MONITOR_TSAK_PROI,
                &w5500_monitor_task_handler);
    vTaskDelete(NULL);
    taskEXIT_CRITICAL(); // 退出临界区
}

/**
 * @brief       传感器采集 + LCD 显示任务
 * @note        每秒读取 DHT11 温湿度 + ADC 电压, 刷新 LCD
 */
void sensor_task(void *pvParameters)
{
    uint8_t temp, humi;
    char buf[32];

    (void)pvParameters;

    /* ---- 初始化传感器外设 ---- */
    lcd_init();                             /* LCD 屏初始化 (FSMC) */
    lcd_clear(WHITE);                       /* 清屏白色背景 */
    lcd_show_string(10, 10, 240, 24, 24, "Sensor Init...", BLUE);

    printf("[SENSOR] DHT11 init...\r\n");
    if (dht11_init() == 0)
    {
        printf("[SENSOR] DHT11 OK\r\n");
    }
    else
    {
        printf("[SENSOR] DHT11 not found! (check PG11)\r\n");
    }
    
    /* 等待传感器稳定 */
    vTaskDelay(pdMS_TO_TICKS(2000));

    lcd_clear(WHITE);

    /* ---- 主循环: 每秒刷新 ---- */
    while (1)
    {
        /* 读取 DHT11 (失败则显示 --) */
        if (dht11_read_data(&temp, &humi) == 0)
        {
            sensor_data_t s = { .temp = temp, .humi = humi };
            xQueueSend(g_sensor_queue, &s, 0);  /* 非阻塞发送 */

            snprintf(buf, sizeof(buf), "Temp: %d C    ", temp);
            lcd_show_string(10, 30, 240, 24, 24, buf, RED);

            snprintf(buf, sizeof(buf), "Humi: %d %%    ", humi);
            lcd_show_string(10, 70, 240, 24, 24, buf, BLUE);
        }
        else
        {
            lcd_show_string(10, 30, 240, 24, 24, "Temp: -- C    ", RED);
            lcd_show_string(10, 70, 240, 24, 24, "Humi: -- %    ", BLUE);
        }
        

        /* 串口同步输出 */
        printf("[SENSOR] Temp=%dC  Humi=%d%%  \r\n",
               temp, humi);

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/**
 * @brief       LED 状态指示 (最低优先级)
 * @note        LEDx(1)=灭, LEDx(0)=亮
 *              断线:   LED0 慢闪, LED1 灭
 *              连接中: LED1 快闪, LED0 灭
 *              已连接: LED1 常亮, LED0 灭
 */
void led_task(void *pvParameters)
{
    (void)pvParameters;

    while (1)
    {
        switch (g_net_state) {

        case NET_STATE_DISCONNECTED:
            /* LED1 灭, LED0 慢闪 (1s 周期) */
            LED1(1);
            LED0(0); vTaskDelay(pdMS_TO_TICKS(500));
            LED0(1); vTaskDelay(pdMS_TO_TICKS(500));
            break;

        case NET_STATE_CONNECTING:
            /* LED1 快闪 (200ms), LED0 灭 */
            LED0(1);
            LED1(0); vTaskDelay(pdMS_TO_TICKS(100));
            LED1(1); vTaskDelay(pdMS_TO_TICKS(100));
            break;

        case NET_STATE_CONNECTED:
            /* LED1 常亮, LED0 灭 */
            LED0(1);
            LED1(0);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;

        default:
            LED0(1); LED1(1);
            vTaskDelay(pdMS_TO_TICKS(500));
            break;
        }
    }
}

/**
 * @brief       MQTT 客户端任务 (FreeRTOS 任务入口)
 * @note        薄包装, 配置 Broker 参数后调用 mqtt_task_run()
 *              修改 server_ip / port / topic 即可适配不同平台
 */
/**
 * @brief       W5500 PHY 链路监控任务
 * @note        每 500ms 轮询 w5500_get_link_status()
 *              网线插入 → 给信号量 g_net_ready_sem, MQTT 任务恢复
 *              网线拔出 → 信号量保持 0, MQTT 任务阻塞等待
 */
void w5500_monitor_task(void *pvParameters)
{
    uint8_t last_link = 0xFF;  /* 初始哨兵值, 确保首次变化触发 */
    uint8_t curr_link;

    (void)pvParameters;

    while (1)
    {
        curr_link = (w5500_get_link_status() == PHY_LINK_ON) ? 1 : 0;

        if (curr_link) {
            /* 链路正常: 每轮都给信号量 (若已为 1 则为空操作)
               确保 MQTT 任务每次 take 后, 下一周期即可再次 take */
            xSemaphoreGive(g_net_ready_sem);
        }

        /* 网线拔出 (1→0 跳变) */
        if (!curr_link && last_link) {
            /* 消费信号量, 下次 MQTT 尝试连接时阻塞 */
            xSemaphoreTake(g_net_ready_sem, 0);     /* 非阻塞 take */
            g_net_state = NET_STATE_DISCONNECTED;   /* 通知 LED */
            printf("[MONITOR] Link DOWN -> net lost\r\n");
        }

        last_link = curr_link;
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/**
 * @brief       平台指令回调 (MQTT PUBLISH 下发)
 * @param       payload: 消息体
 * @param       len:     消息体长度
 */
static void mqtt_on_cmd(const uint8_t *payload, uint16_t len)
{
    printf("[MQTT] CMD: %.*s\r\n", len, payload);

    /* 简单 JSON 匹配: {"led0":1} → LED0亮, {"led1":0} → LED1灭 */
    if (strstr((char *)payload, "\"led0\":0")) LED0(1);
    if (strstr((char *)payload, "\"led0\":1")) LED0(0);
    if (strstr((char *)payload, "\"led1\":0")) LED1(1);
    if (strstr((char *)payload, "\"led1\":1")) LED1(0);
}

void mqtt_task(void *pvParameters)
{
    (void)pvParameters;

    /* ---- 等待 PHY 链路就绪 (网线插入) ---- */
    printf("[MQTT] waiting for PHY link up...\r\n");
    xSemaphoreTake(g_net_ready_sem, portMAX_DELAY); /* 阻塞直到 monitor 给信号量 */

    /* ---- 配置 OneNET 三元组 ---- */
    onenet_set_auth("507rVcegvD", "w5500",
        "version=2018-10-31&res=products%2F507rVcegvD%2Fdevices%2Fw5500&et=1865000000&method=md5&sign=Vgmx6XCq5rqBUERIzoV0zg%3D%3D");

    /* ---- 用 OneNET 填充 Broker 配置 ---- */
    static mqtt_broker_cfg_t cfg;              /* onenet_fill_cfg 填入静态 Topic 缓冲区 */
    onenet_fill_cfg(&cfg);

    cfg.build_payload = onenet_build_payload;  /* 注册 OneNET JSON 构建回调 */

    mqtt_task_run(&cfg, mqtt_on_cmd, g_sensor_queue);  /* 平台指令回调 + 传感器队列 */
}
