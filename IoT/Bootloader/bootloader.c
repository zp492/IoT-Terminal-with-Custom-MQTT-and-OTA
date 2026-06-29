/**
 ****************************************************************************************************
 * @file        bootloader.c
 * @author      zp492
 * @brief       Bootloader 核心流程 — 检查 flag → 校验 CRC → 擦除/拷贝/验证 → 清除 flag → 跳转
 * @note        裸机运行, 不依赖 FreeRTOS
 *
 *              流程:
 *              bootloader_run()
 *                ├─ init_hw()          硬件初始化 (时钟/LED/串口/CRC)
 *                ├─ check_flag()       读 flag 页, 判断是否需要升级
 *                ├─ verify_firmware()  CRC32 校验 Download 区固件
 *                ├─ do_upgrade()       擦除 App → 拷贝 → 验证
 *                ├─ clear_flag()       擦除 flag 页
 *                └─ jump_to_app()      清理外设 → 跳转
 *
 *              错误处理: 任何步骤失败 → SOS LED + 串口输出 → 死循环
 *              (Bootloader 不自动回滚到旧 App — 擦除是不可逆的)
 ****************************************************************************************************
 */

#include "bootloader.h"
#include "bootloader_debug.h"
#include "bootloader_led.h"
#include "bootloader_crc.h"
#include "bootloader_flash.h"
#include "bootloader_lcd.h"

/* ---- 外部依赖 ---- */
#include "./SYSTEM/sys/sys.h"

/* ---- BSP W5500 RST 引脚定义 (最小化: 仅复位, 不初始化 SPI) ---- */
#define W5500_RST_PORT     GPIOD
#define W5500_RST_PIN      GPIO_PIN_6

/* ================================================================================
 * 全局状态
 * ================================================================================ */

volatile uint32_t g_bl_tick = 0;        /* SysTick 1ms 滴答 */
static bl_state_t  g_state  = BL_STATE_INIT;

/* ================================================================================
 * 最小化延时实现 (不依赖 delay.c, 避免 FreeRTOS 依赖和 SysTick 冲突)
 * ================================================================================ */

/**
 * @brief       初始化 SysTick 延时 (替代 delay_init)
 * @param       sysclk: 系统时钟频率 Hz (如 72000000)
 */
static void bl_delay_init(uint32_t sysclk)
{
    SysTick->CTRL = 0;                                          /* 先关闭 SysTick */
    HAL_SYSTICK_CLKSourceConfig(SYSTICK_CLKSOURCE_HCLK_DIV8);   /* HCLK/8 */
    SysTick->LOAD = (sysclk / 8) / 1000 - 1;                    /* 1ms 重载值 */
    SysTick->VAL  = 0;
    /* CLKSOURCE=0: HCLK/8 (ST 实现), TICKINT=1, ENABLE=1 */
    SysTick->CTRL = SysTick_CTRL_TICKINT_Msk | SysTick_CTRL_ENABLE_Msk;
}

/**
 * @brief       微秒级延时 (NOP 忙等, 72MHz 下 ~8 周期/循环 → 9 次/μs)
 */
static void bl_delay_us(uint32_t nus)
{
    uint32_t i;
    for (i = 0; i < nus * 9; i++) {
        __NOP();
    }
}

/**
 * @brief       毫秒级延时 (基于 SysTick 中断 + g_bl_tick)
 */
static void bl_delay_ms(uint32_t nms)
{
    uint32_t target = g_bl_tick + nms;
    while (g_bl_tick < target) {
        /* 等待 SysTick_Handler 递增 g_bl_tick */
    }
}

/* ---- 兼容别名 (lcd.c 等 BSP 驱动调用 delay_us / delay_ms) ---- */
void delay_us(uint32_t nus)   { bl_delay_us(nus); }
void delay_ms(uint32_t nms)   { bl_delay_ms(nms); }

/* ================================================================================
 * SysTick 中断处理
 * ================================================================================ */

void SysTick_Handler(void)
{
    g_bl_tick++;
    HAL_IncTick();  /* Flash HAL 超时依赖 HAL_GetTick() */
}

/* ================================================================================
 * 内部函数声明
 * ================================================================================ */

static void init_hw(void);
static int  check_flag(uint32_t *out_size, uint32_t *out_crc);
static int  verify_firmware(uint32_t fw_size, uint32_t expected_crc);
static int  do_upgrade(uint32_t fw_size);
static void clear_flag(void);
static void jump_to_app(void) __attribute__((noreturn));
static void error_halt(bl_status_t err);
static void w5500_hardware_reset(void);
static void bl_read_fw_version(uint32_t base_addr, char *ver_buf, uint8_t buf_len);

/* ================================================================================
 * 公共 API
 * ================================================================================ */

void bootloader_set_state(bl_state_t state)
{
    g_state = state;
    bl_led_set_state(state);
}

bl_state_t bootloader_get_state(void)
{
    return g_state;
}

const char *bootloader_errstr(bl_status_t err)
{
    switch (err) {
    case BL_OK:                 return "OK";
    case BL_UPGRADING:          return "UPGRADING";
    case BL_UPGRADE_DONE:       return "UPGRADE_DONE";
    case BL_ERR_FLASH_ERASE:    return "FLASH_ERASE_FAILED";
    case BL_ERR_FLASH_PROGRAM:  return "FLASH_PROGRAM_FAILED";
    case BL_ERR_FLASH_VERIFY:   return "FLASH_VERIFY_FAILED";
    case BL_ERR_CRC_MISMATCH:   return "CRC_MISMATCH";
    case BL_ERR_INVALID_SIZE:   return "INVALID_FW_SIZE";
    case BL_ERR_APP_INVALID:    return "APP_INVALID";
    case BL_ERR_NO_FLAG:        return "NO_FLAG";
    default:                    return "UNKNOWN";
    }
}

/* ================================================================================
 * 主入口
 * ================================================================================ */

void bootloader_run(void)
{
    uint32_t fw_size, fw_crc;
    int ret;

    /* ---- Step 0: 硬件初始化 ---- */
    init_hw();

    BL_LOG("Bootloader v%d.%d starting...", BL_VERSION_MAJOR, BL_VERSION_MINOR);
    BL_LOG("Partition: BL=0x%08X(%uKB) App=0x%08X(%uKB) DL=0x%08X(%uKB)",
           (unsigned)BOOTLOADER_BASE_ADDR, (unsigned)(BOOTLOADER_SIZE / 1024),
           (unsigned)APP_BASE_ADDR, (unsigned)(APP_SIZE / 1024),
           (unsigned)DOWNLOAD_BASE_ADDR, (unsigned)(DOWNLOAD_SIZE / 1024));
    BL_LOG("Flag page: 0x%08X", (unsigned)FLAG_PAGE_BASE_ADDR);

    /* ---- Step 1: 检查升级标志 ---- */
    bootloader_set_state(BL_STATE_CHECK_FLAG);
    ret = check_flag(&fw_size, &fw_crc);
    if (ret == BL_ERR_NO_FLAG) {
        /* 无升级 → 直接跳转 App */
        BL_LOG("No upgrade flag, booting App...");
        bootloader_set_state(BL_STATE_JUMP_TO_APP);
        jump_to_app();
        /* NOTREACHED */
    }
    if (ret < 0) {
        /* Flag 参数非法 (例如 size 越界) → 擦除 Flag → 启动旧 App
         * 此时 App 区完好, 无理由停机, 旧固件仍然可用 */
        BL_LOG("Flag invalid (%s), erasing and booting old App...",
               bootloader_errstr((bl_status_t)ret));
        clear_flag();
        bootloader_set_state(BL_STATE_JUMP_TO_APP);
        jump_to_app();
        /* NOTREACHED */
    }

    /* ---- Step 2: CRC32 校验 Download 区 ---- */
    BL_LOG("Flag valid: size=%u CRC32=0x%08X",
           (unsigned)fw_size, (unsigned)fw_crc);
    ret = verify_firmware(fw_size, fw_crc);
    if (ret < 0) {
        BL_LOG("Firmware verification failed (%s), erasing flag and falling back...",
               bootloader_errstr((bl_status_t)ret));
        __HAL_RCC_FSMC_CLK_ENABLE();
        bl_lcd_init();
        bl_lcd_show_upgrade_error(-4);
        clear_flag();
        bl_lcd_wait_any_key();
        bootloader_set_state(BL_STATE_JUMP_TO_APP);
        jump_to_app();
        /* NOTREACHED */
    }

    /* ---- 发现新固件, 初始化 LCD 并显示版本 ---- */
    {
        char cur_ver[16], new_ver[16];
        bl_confirm_t choice;

        /* 此时才使能 FSMC + 初始化 LCD, 不干扰正常 App 启动 */
        __HAL_RCC_FSMC_CLK_ENABLE();
        bl_lcd_init();

        /* 从 App 区和 Download 区读取版本号 */
        bl_read_fw_version(APP_BASE_ADDR,       cur_ver, sizeof(cur_ver));
        bl_read_fw_version(DOWNLOAD_BASE_ADDR,  new_ver, sizeof(new_ver));

        bl_lcd_show_new_firmware(cur_ver, new_ver);
        choice = bl_lcd_confirm_upgrade(10000);

        if (choice == BL_CONFIRM_SKIP) {
            BL_LOG("User cancelled upgrade, erasing flag and booting old App...");
            bl_lcd_show_upgrade_cancelled();
            bl_delay_ms(2000);
            clear_flag();
            bootloader_set_state(BL_STATE_JUMP_TO_APP);
            jump_to_app();
            /* NOTREACHED */
        }
    }

    /* ---- Step 3: 执行升级 (擦 App + 拷贝 + 验证) ---- */
    bootloader_set_state(BL_STATE_UPGRADING);
    BL_LOG("Starting firmware upgrade...");
    ret = do_upgrade(fw_size);
    if (ret < 0) {
        BL_LOG("Upgrade FAILED! code=%d (%s)", ret, bootloader_errstr((bl_status_t)ret));
        bl_lcd_show_upgrade_error(ret);
        bl_lcd_wait_any_key();
        BL_LOG("Rebooting to retry...");
        bl_delay_ms(500);
        NVIC_SystemReset();
        /* NOTREACHED */
    }

    /* ---- Step 4: 清除 Flag ---- */
    bootloader_set_state(BL_STATE_UPGRADE_DONE);
    BL_LOG("Upgrade complete! Erasing flag page...");
    bl_lcd_show_upgrade_done();
    clear_flag();

    bl_delay_ms(3000);

    /* ---- Step 5: 跳转 App ---- */
    BL_LOG("Jumping to App at 0x%08X...", (unsigned)APP_BASE_ADDR);
    jump_to_app();
    /* NOTREACHED */
}

/* ================================================================================
 * Step 0: 硬件初始化
 * ================================================================================ */

static void init_hw(void)
{
    /* 系统时钟: 72MHz (HSE + PLL x9, HSI 降级保护) */
    HAL_Init();
    sys_stm32_clock_init(RCC_PLL_MUL9);
    bl_delay_init(SystemCoreClock);  /* 单位: Hz, 72MHz = 72000000 */

    /* 注意: HAL_Init() 内部调用了 HAL_InitTick() 将 SysTick 配置为 1ms 中断
     * 优先级为最低 (15), 符合 Cortex-M3 NVIC 规范 */

    /* LED 指示 */
    bl_led_init();
    bootloader_set_state(BL_STATE_INIT);

    /* 串口日志 */
    bl_debug_init();

    /* 硬件 CRC32 */
    bl_crc32_init();

    /* 裸机环境: 手动开全局中断 (SysTick_Handler 依赖此位) */
    __enable_irq();

    /* LCD 延迟到检测到升级标志后才初始化,
       避免 FSMC/GPIO 抢占影响 App 正常启动 */
}

/* ================================================================================
 * Step 1: 检查升级标志
 * ================================================================================ */

static int check_flag(uint32_t *out_size, uint32_t *out_crc)
{
    volatile ota_flag_t *flag = OTA_FLAG;

    BL_LOG("Flag: magic=0x%08X size=%u CRC=0x%08X",
           (unsigned)flag->magic, (unsigned)flag->fw_size,
           (unsigned)flag->fw_crc32);

    /* Magic 不匹配 → 无升级 */
    if (flag->magic != OTA_FLAG_MAGIC) {
        BL_LOG("Flag magic mismatch (expected 0x%08X)", (unsigned)OTA_FLAG_MAGIC);
        return BL_ERR_NO_FLAG;
    }

    /* 大小校验 */
    if (flag->fw_size == 0 || flag->fw_size > DOWNLOAD_SIZE) {
        BL_LOG("Invalid firmware size: %u (max %u)",
               (unsigned)flag->fw_size, (unsigned)DOWNLOAD_SIZE);
        return BL_ERR_INVALID_SIZE;
    }

    /* 不允许升级状态为 ERROR 的固件 */
    *out_size = flag->fw_size;
    *out_crc  = flag->fw_crc32;
    return 0;
}

/* ================================================================================
 * Step 2: CRC32 校验 Download 区固件
 * ================================================================================ */

static int verify_firmware(uint32_t fw_size, uint32_t expected_crc)
{
    uint32_t computed_crc;

    BL_LOG("Computing CRC32 of Download area...");
    BL_LOG("  Range: 0x%08X ~ 0x%08X (%u bytes)",
           (unsigned)DOWNLOAD_BASE_ADDR,
           (unsigned)(DOWNLOAD_BASE_ADDR + fw_size),
           (unsigned)fw_size);

    computed_crc = bl_crc32_calculate(DOWNLOAD_BASE_ADDR, fw_size);

    BL_LOG("  Expected: 0x%08X", (unsigned)expected_crc);
    BL_LOG("  Computed: 0x%08X", (unsigned)computed_crc);

    if (computed_crc != expected_crc) {
        BL_LOG("CRC MISMATCH!");
        return BL_ERR_CRC_MISMATCH;
    }

    BL_LOG("CRC32 verification PASSED");
    return 0;
}

/* ================================================================================
 * LCD 进度回调 (由 bl_flash_copy_region 每完成一页调用)
 * ================================================================================ */

static void upgrade_progress_cb(uint32_t page, uint32_t total)
{
    bl_lcd_update_progress(page, total);
}

/* ================================================================================
 * Step 3: 执行升级 (擦除 + 拷贝 + 验证)
 * ================================================================================ */

static int do_upgrade(uint32_t fw_size)
{
    int ret;

    bl_lcd_show_upgrading_start();

    /* ----------------------------------------------------------------
     * Step 3a: 逐页搬运 (每页: 读暂存区→擦App→写App→页内字节比对)
     * ---------------------------------------------------------------- */
    ret = bl_flash_copy_region(DOWNLOAD_BASE_ADDR, APP_BASE_ADDR, fw_size,
                               upgrade_progress_cb);
    if (ret != 0) {
        return ret;
    }

    /* ----------------------------------------------------------------
     * Step 3b: 回读校验 — App 区全量逐字节比对 Download 暂存区
     *
     *   从 App 区首字节 (0x0800C000) 到末尾, 逐一回读,
     *   与 Download 区 (0x08046000) 对应位置的字节比对.
     *   只有全部 232KB 的每一个比特都一致, 才返回成功.
     *
     *   这是 Flag 擦除前的最后一道关卡:
     *   - 验证失败 → Flag 保留 → 软复位 → 下次上电重试
     *   - 验证通过 → clear_flag() → jump_to_app()
     * ---------------------------------------------------------------- */
    BL_LOG("UPGRADE: starting full-image read-back verify...");
    BL_LOG("  App:      0x%08X", (unsigned)APP_BASE_ADDR);
    BL_LOG("  Download: 0x%08X", (unsigned)DOWNLOAD_BASE_ADDR);
    BL_LOG("  Size:     %u bytes", (unsigned)fw_size);

    ret = bl_flash_verify_region(DOWNLOAD_BASE_ADDR, APP_BASE_ADDR, fw_size);
    if (ret != 0) {
        BL_LOG("UPGRADE: read-back MISMATCH at offset %d", ret);
        BL_LOG("  Expected (Download): 0x%02X",
               *(volatile uint8_t *)(DOWNLOAD_BASE_ADDR + (uint32_t)ret));
        BL_LOG("  Actual   (App):      0x%02X",
               *(volatile uint8_t *)(APP_BASE_ADDR + (uint32_t)ret));
        return BL_ERR_FLASH_VERIFY;
    }

    BL_LOG("UPGRADE: read-back verify PASSED - all %u bits match", (unsigned)(fw_size * 8));
    return 0;
}

/* ================================================================================
 * Step 4: 清除 Flag
 * ================================================================================ */

static void clear_flag(void)
{
    int ret = bl_flash_erase_flag_page();
    if (ret != 0) {
        /* Flag 擦除失败不是致命错误 — 固件已正确写入 App 区
         * 但下次上电 Bootloader 会再次尝试升级 (重复操作)
         * 这是安全的, 因为升级操作是幂等的 */
        BL_LOG("WARNING: flag erase failed, will re-upgrade on next boot");
        BL_LOG("         (upgrade is idempotent, no risk of bricking)");
    } else {
        BL_LOG("Flag page erased successfully");
    }
}

/* ================================================================================
 * Step 5: 跳转 App
 * ================================================================================ */

static void jump_to_app(void)
{
    uint32_t app_msp;
    uint32_t app_reset;
    void (*app_entry)(void);

    BL_LOG("Preparing to jump to App...");

    /* 读取 App 向量表的前两个 32-bit 字 */
    app_msp   = *(volatile uint32_t *)(APP_BASE_ADDR + 0);
    app_reset = *(volatile uint32_t *)(APP_BASE_ADDR + 4);

    BL_LOG("  App MSP  = 0x%08X", (unsigned)app_msp);
    BL_LOG("  App Entry= 0x%08X", (unsigned)app_reset);

    /* ---- 安全校验 ---- */

    /* MSP 必须在 SRAM 范围内 (0x20000000 ~ 0x20010000) */
    if (app_msp < 0x20000000UL || app_msp > 0x20010000UL) {
        BL_LOG("INVALID App MSP: 0x%08X (not in SRAM)", (unsigned)app_msp);
        error_halt(BL_ERR_APP_INVALID);
        /* NOTREACHED */
    }

    /* Reset_Handler 必须在 Flash 范围内且 thumb 位为 1 */
    if ((app_reset & 0x2FFE0000UL) != 0x08000000UL) {
        BL_LOG("INVALID App Reset_Handler: 0x%08X", (unsigned)app_reset);
        error_halt(BL_ERR_APP_INVALID);
        /* NOTREACHED */
    }

    /* ---- 外设清理 ---- */

    /* W5500 硬件复位: 确保 App 启动时 W5500 处于已知状态 */
    BL_LOG("  Resetting W5500...");
    w5500_hardware_reset();
    BL_LOG("  W5500 reset done");

    /* 关全局中断 */
    __disable_irq();

    /* 停止 SysTick (App 会重新初始化) */
    SysTick->CTRL = 0;

    /* ---- 设置 App 运行环境 ---- */

    /* 设主栈指针为 App 的初始 SP */
    __set_MSP(app_msp);

    /* 设向量表偏移到 App 区 */
    sys_nvic_set_vector_table(FLASH_BASE_ADDR, APP_VECT_TAB_OFFSET);

    /* 清除所有挂起的中断 */
    {
        uint32_t irq;
        for (irq = 0; irq < 8; irq++) {
            NVIC->ICPR[irq] = 0xFFFFFFFF;
        }
    }

    BL_LOG("Jumping now...");

    /* ---- 跳转 ---- */
    app_entry = (void (*)(void))app_reset;

    /* 跳转前最后一口气: 确保串口缓冲区清空 */
    {
        volatile uint32_t d = 0;
        while (d < 100000) d++;  /* 等待 TX 完成 */
    }

    app_entry();

    /* 如果执行到这里, 跳转失败 */
    BL_LOG("FATAL: jump returned! Halting...");
    while (1);
}

/* ================================================================================
 * 错误处理
 * ================================================================================ */

static void error_halt(bl_status_t err)
{
    BL_LOG("========================================");
    BL_LOG("BOOTLOADER ERROR: %s (code=%d)", bootloader_errstr(err), (int)err);
    BL_LOG("System halted. Recovery options:");
    BL_LOG("  1. Power-cycle to retry");
    BL_LOG("  2. Re-flash via serial (USART1) or SWD");
    BL_LOG("========================================");

    bootloader_set_state(BL_STATE_ERROR);
    bl_led_set_error((int8_t)err);

    /* 死循环, LED 持续 SOS */
    while (1) {
        bl_led_update();
        /* 简单忙等待 (LED 状态机需要 g_bl_tick 递增, SysTick 仍在运行) */
    }
}

/* ================================================================================
 * W5500 硬件复位 (最小化实现, 不依赖 w5500_port.c / FreeRTOS)
 * ================================================================================ */

/* ================================================================================
 * 读取固件版本信息
 * ================================================================================ */

static void bl_read_fw_version(uint32_t base_addr, char *ver_buf, uint8_t buf_len)
{
    const fw_info_t *info;
    uint32_t info_addr;

    if (buf_len == 0) return;
    ver_buf[0] = '\0';

    info_addr = base_addr + FW_INFO_OFFSET;
    info = (const fw_info_t *)(uintptr_t)info_addr;

    /* 检查 magic 是否有效 */
    if (info->magic == FW_INFO_MAGIC) {
        /* 优先使用 version_str */
        uint8_t i;
        for (i = 0; i < buf_len - 1 && i < (FW_INFO_VERSION_STR_LEN - 1); i++) {
            if (info->version_str[i] == '\0') break;
            ver_buf[i] = info->version_str[i];
        }
        ver_buf[i] = '\0';

        /* 如果 version_str 为空, 用数字拼接 */
        if (ver_buf[0] == '\0') {
            snprintf(ver_buf, buf_len, "%u.%u.%u",
                     (unsigned)info->ver_major,
                     (unsigned)info->ver_minor,
                     (unsigned)info->ver_patch);
        }
    } else {
        /* magic 不匹配 → 旧版固件或未写入, 显示 "?" */
        snprintf(ver_buf, buf_len, "?");
    }

    BL_LOG("FW info at 0x%08X: magic=0x%08X ver=%s",
           (unsigned)info_addr, (unsigned)info->magic, ver_buf);
}

/* ================================================================================
 * W5500 硬件复位
 * ================================================================================ */

static void w5500_hardware_reset(void)
{
    GPIO_InitTypeDef gpio_init;

    /* GPIOD 时钟 (PD6 = W5500 RST) */
    __HAL_RCC_GPIOD_CLK_ENABLE();

    /* PD6 推挽输出 + 上拉 */
    gpio_init.Pin   = W5500_RST_PIN;
    gpio_init.Mode  = GPIO_MODE_OUTPUT_PP;
    gpio_init.Pull  = GPIO_PULLUP;
    gpio_init.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(W5500_RST_PORT, &gpio_init);

    /* RST 脉冲 */
    HAL_GPIO_WritePin(W5500_RST_PORT, W5500_RST_PIN, GPIO_PIN_RESET);
    bl_delay_us(600);
    HAL_GPIO_WritePin(W5500_RST_PORT, W5500_RST_PIN, GPIO_PIN_SET);
    bl_delay_ms(10);
}
