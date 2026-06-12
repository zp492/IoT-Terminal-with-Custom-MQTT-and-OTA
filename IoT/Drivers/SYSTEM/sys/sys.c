/**
 ****************************************************************************************************
 * @file        sys.c
 * @version     V2.0
 * @date        2026-06-12
 * @brief       系统底层驱动代码(包括时钟配置/中断管理/低功耗/复位等)
 * @details     该文件提供了系统底层的初始化和管理函数, 包括:
 *              - 系统时钟配置 (HSE + PLL, 带HSI降级保护)
 *              - 中断向量表偏移设置 (用于IAP/Bootloader)
 *              - 低功耗模式 (WFI / 待机)
 *              - 系统软复位
 *              - 全局中断开关
 *              - 栈顶地址设置 (用于RTOS任务切换)
 *
 * @note        硬件平台: 正点原子 STM32F103ZET6 开发板
 *              - MCU:    STM32F103ZET6 (Cortex-M3, 72MHz主频, 512KB Flash, 64KB SRAM)
 *              - HSE:    8MHz 外部晶振 (定义于 stm32f1xx_hal_conf.h 中的 HSE_VALUE = 8000000U)
 *              - HSI:    8MHz 内部RC振荡器 (精度较低 ±1%, 用于HSE故障时的降级时钟源)
 *              - LSE:    32.768KHz 外部低速晶振 (用于RTC)
 *
 * @note        时钟树概览 (F103):
 *              HSE(8MHz) ──→ PLL(×9) ──→ PLLCLK(72MHz) ──→ SYSCLK ──→ HCLK(AHB总线,72MHz)
 *                                               │                        ├──→ PCLK1(APB1,≤36MHz)
 *                                               │                        └──→ PCLK2(APB2,≤72MHz)
 *                                               │
 *              降级路径:  HSI(8MHz) ──→ 直通 ──→ SYSCLK(8MHz)
 *
 * @note        修改历史
 *              V1.0 20211103  正点原子团队: 第一次发布, 原始版本
 *              V2.0 20260601  zp492: 重写时钟初始化, 新增HSI降级保护
 *              V2.1 20260612  zp492: 新增自适应分频/Flash等待周期
 ****************************************************************************************************
 */

#include "./SYSTEM/sys/sys.h"

/**
 * @brief       设置中断向量表偏移地址
 * @param       baseaddr: 基址 (通常为 FLASH_BASE 0x08000000 或 SRAM_BASE 0x20000000)
 * @param       offset: 偏移量(必须是0, 或者0X100(256字节)的倍数)
 * @note        VTOR寄存器低9位保留([8:0]必须为0), 因此偏移量必须256字节对齐
 *              典型应用: IAP/Bootloader将用户程序的向量表重定位到 Flash 偏移处
 *              例如: sys_nvic_set_vector_table(0x08000000, 0x10000); // 偏移64KB
 * @retval      无
 */
void sys_nvic_set_vector_table(uint32_t baseaddr, uint32_t offset)
{
    /* 设置NVIC的向量表偏移寄存器,VTOR低9位保留,即[8:0]保留 */
    SCB->VTOR = baseaddr | (offset & (uint32_t)0xFFFFFE00);
}

/**
 * @brief       执行 WFI (Wait For Interrupt) 指令
 * @note        WFI 是 Cortex-M3 的低功耗指令:
 *              - CPU进入Sleep模式, 停止执行指令, 但外设和NVIC继续运行
 *              - 任意中断(包括SysTick)或调试事件均可唤醒CPU
 *              - 唤醒后从中断服务函数开始执行, 中断返回后继续WFI的下一条指令
 *              - 常用于RTOS的idle任务中, 在没有任务运行时降低功耗
 * @param       无
 * @retval      无
 */
void sys_wfi_set(void)
{
    __ASM volatile("wfi");
}

/**
 * @brief       全局关闭所有可屏蔽中断
 * @note        执行 CPSID I (Change Processor State - Disable Interrupts)
 *              - 设置 PRIMASK = 1, 屏蔽所有优先级可配置的中断
 *              - Fault异常( HardFault / MemManage / BusFault / UsageFault ) 不受影响
 *              - NMI (不可屏蔽中断) 不受影响
 *              - 与 __disable_irq() 等效, 但直接用内联汇编避免函数调用开销
 *              - 临界区保护需要成对使用: sys_intx_disable() → 临界代码 → sys_intx_enable()
 * @param       无
 * @retval      无
 */
void sys_intx_disable(void)
{
    __ASM volatile("cpsid i");
}

/**
 * @brief       全局开启所有可屏蔽中断
 * @note        执行 CPSIE I (Change Processor State - Enable Interrupts)
 *              - 清除 PRIMASK = 0, 恢复中断响应
 *              - 与 sys_intx_disable() 配对使用
 *              - FreeRTOS 的 taskENTER_CRITICAL() / taskEXIT_CRITICAL() 内部调用了类似机制
 *              - 注意: 如果嵌套调用 disable, 只需一次 enable 即可恢复 (PRIMASK 是单比特, 不支持嵌套计数)
 * @param       无
 * @retval      无
 */
void sys_intx_enable(void)
{
    __ASM volatile("cpsie i");
}

/**
 * @brief       设置主栈指针(MSP)
 * @note        用于设置栈顶地址, 通常在RTOS任务第一次启动或任务切换时使用
 *              - MSP (Main Stack Pointer): 主栈指针, 用于Handler模式(中断服务)和裸机程序
 *              - PSP (Process Stack Pointer): 进程栈指针, 用于Thread模式(RTOS任务)
 *              - FreeRTOS 创建第一个任务时, 会通过此函数设置MSP到新任务的栈顶
 *              - MDK左侧红X为误报: __set_MSP() 是CMSIS内联函数, 实际编译无问题
 * @param       addr: 栈顶地址 (通常来自任务栈数组的高地址端, 因为栈是向下生长的)
 * @retval      无
 */
void sys_msr_msp(uint32_t addr)
{
    __set_MSP(addr); /* 设置栈顶地址 */
}

/**
 * @brief       进入待机模式 (Standby Mode)
 * @note        待机模式是 STM32F103 最低功耗的模式:
 *              - 关闭内部LDO, Cortex-M3 内核和所有外设全部断电
 *              - 仅备份域( RTC / 备份寄存器 / 唤醒逻辑 )保持供电
 *              - 唤醒源: WKUP引脚上升沿 / RTC闹钟 / IWDG复位 / NRST引脚复位
 *              - 唤醒后等同于系统复位: 程序从开头重新执行, SRAM数据全部丢失
 *              - 与睡眠模式(Sleep)区别: Sleep 保留SRAM, 任意中断可唤醒, 待机功耗更低
 * @param       无
 * @retval      无
 */
void sys_standby(void)
{
    __HAL_RCC_PWR_CLK_ENABLE();    /* 使能电源时钟 */
    SET_BIT(PWR->CR, PWR_CR_PDDS); /* 进入待机模式 */
}

/**
 * @brief       系统软复位 (Software Reset)
 * @note        通过设置 SCB->AIRCR 的 SYSRESETREQ 位触发系统复位
 *              - 效果等效于按下外部复位按钮, 但由软件触发
 *              - 复位后: 所有寄存器恢复默认值, 程序从复位向量(0x00000004)重新执行
 *              - 常用场景: OTA固件升级完成后跳转到新固件 / 发生不可恢复错误时重启
 *              - 注意: 复位后 SRAM 数据不保留 (除非配置为备份域供电)
 * @param       无
 * @retval      无
 */
void sys_soft_reset(void)
{
    NVIC_SystemReset();
}

/* ================================================================================
 * F103 PLL倍频系数位域抽取宏
 * --------------------------------------------------------------------------------
 * RCC_CFGR 寄存器 bit[21:18] = PLLMUL[3:0]:
 *   0000 = PLL input clock × 2  →  RCC_PLL_MUL2  (0x00000000)
 *   0001 = PLL input clock × 3  →  RCC_PLL_MUL3  (0x00040000)
 *   0010 = PLL input clock × 4  →  RCC_PLL_MUL4  (0x00080000)
 *   ...
 *   1110 = PLL input clock × 16 →  RCC_PLL_MUL16 (0x00380000)
 *   1111 = PLL input clock × 16 → (重复, 见参考手册)
 *
 * 实际倍频数 = 位域值 + 2
 * 例如: RCC_PLL_MUL9 = 0x001C0000 → bit[21:18] = 0111 = 7 → 倍频数 = 7 + 2 = 9
 * ================================================================================ */
#define PLL_MUL_TO_INT(pll)     ((((pll) >> 18U) & 0x0FU) + 2U)

/**
 * @brief       根据SYSCLK频率自适应选择Flash等待周期
 * @param       sysclk_freq: 系统时钟频率 (Hz)
 * @retval      FLASH_LATENCY_x
 * @note        F103 Flash访问时间与供电电压/系统时钟的关系 (数据手册表):
 *              ┌──────────┬──────────────┬──────────────┐
 *              │ 等待周期  │  SYSCLK范围   │  适用场景      │
 *              ├──────────┼──────────────┼──────────────┤
 *              │ 0WS      │  0 ~ 24MHz   │  HSI直通(8MHz) │
 *              │ 1WS      │ 24 ~ 48MHz   │  PLL×6(48MHz)  │
 *              │ 2WS      │ 48 ~ 72MHz   │  PLL×9(72MHz)  │
 *              └──────────┴──────────────┴──────────────┘
 *              设置原则: 频率越高需要越多等待周期, 但过多的等待周期会降低性能,
 *              因此根据实际频率自适应选择最优值, 而不是固定使用FLASH_LATENCY_2
 */
static uint32_t sys_get_flash_latency(uint32_t sysclk_freq)
{
    if (sysclk_freq <= 24000000U)   return FLASH_LATENCY_0;
    if (sysclk_freq <= 48000000U)   return FLASH_LATENCY_1;
    return FLASH_LATENCY_2;  /* 48~72MHz */
}

/**
 * @brief       系统时钟初始化函数 (F103ZET6)
 * @param       plln: PLL倍频系数, 使用 RCC_PLL_MUL2 ~ RCC_PLL_MUL16 宏
 *              常用值: RCC_PLL_MUL9(72MHz) / RCC_PLL_MUL8(64MHz) / RCC_PLL_MUL6(48MHz)
 *
 * @note        【启动流程】
 *              上电复位 → SystemInit() (startup_stm32f1xx.s自动调用)
 *              SystemInit() 完成:
 *              - 开启 HSI (8MHz, 默认时钟源)
 *              - 开启 HSE, 等待就绪 (超时 HSE_STARTUP_TIMEOUT = 100ms)
 *              - 配置 PLL (HSE × 9 = 72MHz)
 *              - 切换到 PLL 作为系统时钟 SYSCLK
 *              - 更新 SystemCoreClock = 72MHz
 *              之后跳转到 main() → HAL_Init() → sys_stm32_clock_init()
 *              本函数"重新"配置, 允许用户代码显式控制时钟而非依赖启动代码默认值
 *
 * @note        【设计决策】
 *              1. HSE故障降级: 外部晶振损坏/虚焊/潮湿环境下, 自动回退到HSI 8MHz
 *                 避免系统完全不可用; 此时仍能通过串口/指示灯报告故障
 *              2. 自适应分频: APB1最大36MHz限制由硬件决定, 低频率时不分频以减少延迟
 *              3. 自适应Flash等待: 低的SYSCLK用更少等待周期, 降低功耗和延迟
 *              4. SystemCoreClockUpdate(): F1 HAL不会自动更新, 必须手动调用,
 *                 否则 delay_init() / SysTick配置会出错
 *              5. 返回值设计: 允许调用方根据返回值采取不同策略 (告警/降级运行/重启)
 *
 * @retval      0: HSE + PLL 配置成功, SYSCLK = HSE × (PLL倍频数), 典型72MHz
 *              1: HSE启动失败, 已降级为 HSI 8MHz, 系统降速运行但可用
 *              2: PLL倍频系数非法或HSI也无法启动, 系统可能无法正常工作
 */
uint8_t sys_stm32_clock_init(uint32_t plln)
{
    HAL_StatusTypeDef ret;
    RCC_OscInitTypeDef rcc_osc_init = {0};  /* {0} 将结构体全部清零, 避免未初始化字段的随机值 */
    RCC_ClkInitTypeDef rcc_clk_init = {0};
    uint32_t pllclk;       /* PLL输出频率, 即最终的 SYSCLK */
    uint32_t flash_latency;

    /* ============================================================================
     * 第一步: 尝试启动HSE + 配置PLL (外部晶振路径)
     * ----------------------------------------------------------------------------
     * 目标: HSE(8MHz) → PLL(倍频) → PLLCLK → SYSCLK → HCLK / PCLK1 / PCLK2
     * HAL_RCC_OscConfig() 内部流程:
     *   1. 开启HSE → 2. 等待HSERDY标志位置位(超时100ms) → 3. 配置PLL → 4. 等待PLLRDY
     * ============================================================================ */
    rcc_osc_init.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    rcc_osc_init.HSEState = RCC_HSE_ON;
    rcc_osc_init.HSEPredivValue = RCC_HSE_PREDIV_DIV1; /* HSE不分频, 8MHz直入PLL */
    rcc_osc_init.PLL.PLLState = RCC_PLL_ON;
    rcc_osc_init.PLL.PLLSource = RCC_PLLSOURCE_HSE;
    rcc_osc_init.PLL.PLLMUL = plln;
    ret = HAL_RCC_OscConfig(&rcc_osc_init);

    if (ret == HAL_OK)
    {
        /* HSE启动成功 → 计算PLL输出频率, 自适应APB1分频和Flash等待 */
        pllclk = HSE_VALUE * PLL_MUL_TO_INT(plln);  /* 例: 8MHz × 9 = 72MHz */

        /* 配置系统时钟分频 ─────────────────────────────────────────────────
         * AHB(HCLK)  = SYSCLK / 1          = 72MHz (最大)
         * APB1(PCLK1)= HCLK / (1 or 2)    ≤ 36MHz (F103硬件限制)
         * APB2(PCLK2)= HCLK / 1            = 72MHz (最大)
         * 注意: 当APB1预分频>1时, 定时器时钟 = APB1 × 2 (详见参考手册)
         * ──────────────────────────────────────────────────────────────── */
        rcc_clk_init.ClockType = (RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK
                                | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2);
        rcc_clk_init.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
        rcc_clk_init.AHBCLKDivider = RCC_SYSCLK_DIV1;
        /* F103 APB1最高36MHz, 自适应分频: SYSCLK>36M时分频, 否则直通 */
        rcc_clk_init.APB1CLKDivider = (pllclk > 36000000U) ? RCC_HCLK_DIV2 : RCC_HCLK_DIV1;
        rcc_clk_init.APB2CLKDivider = RCC_HCLK_DIV1;
        flash_latency = sys_get_flash_latency(pllclk);
        /* HAL_RCC_ClockConfig() 同时设置 Flash等待周期 + 各总线分频 + SYSCLK切换 */
        ret = HAL_RCC_ClockConfig(&rcc_clk_init, flash_latency);

        if (ret == HAL_OK)
        {
            /* F1 HAL的 HAL_RCC_ClockConfig() 不会更新 SystemCoreClock 全局变量,
             * 这与F4/F7系列不同(F4的HAL_RCC_ClockConfig内部会调用更新函数).
             * 如果不手动调用, SystemCoreClock 将保持 SystemInit() 中设置的值,
             * 导致 delay_init() / SysTick_Config() / HAL_GetTick() 等函数出错. */
            SystemCoreClockUpdate();
            return 0;
        }
    }

    /* ============================================================================
     * 第二步: HSE或PLL失败 → 降级为内部HSI (8MHz)
     * ----------------------------------------------------------------------------
     * 故障可能原因:
     *   - 外部8MHz晶振损坏或虚焊 (最常见)
     *   - 晶振负载电容不匹配, 起振失败
     *   - 潮湿/污染导致振荡回路Q值下降
     *   - PLL倍频系数配置错误导致锁相环失锁
     *
     * 降级策略:
     *   HSI是芯片内部RC振荡器, 出厂已校准(精度±1%), 不需要任何外部元件
     *   → 关闭HSE省电, 关闭PLL, HSI直通作为SYSCLK
     *   → 8MHz虽然慢, 但足以让系统启动并通过串口/LED报告故障状态
     * ============================================================================ */
    rcc_osc_init.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    rcc_osc_init.HSIState = RCC_HSI_ON;
    rcc_osc_init.HSEState = RCC_HSE_OFF;        /* 关闭故障的HSE, 避免持续消耗电流 */
    rcc_osc_init.PLL.PLLState = RCC_PLL_OFF;    /* 不经过PLL, HSI直接输出 */
    ret = HAL_RCC_OscConfig(&rcc_osc_init);

    if (ret != HAL_OK)
    {
        /* HSI也无法启动 —— 极低概率事件: 芯片内部RC振荡器故障
         * 此时系统没有任何时钟源可用, 只能死循环 (连串口打印都不行) */
        while (1);
    }

    /* HSI直通: SYSCLK = HSI = 8MHz
     * 所有总线不分频, Flash 0等待周期 (8MHz远低于24MHz的0WS上限) */
    rcc_clk_init.ClockType = (RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK
                            | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2);
    rcc_clk_init.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    rcc_clk_init.AHBCLKDivider = RCC_SYSCLK_DIV1;
    rcc_clk_init.APB1CLKDivider = RCC_HCLK_DIV1; /* 8MHz < 36MHz限制, 无需分频 */
    rcc_clk_init.APB2CLKDivider = RCC_HCLK_DIV1;
    ret = HAL_RCC_ClockConfig(&rcc_clk_init, FLASH_LATENCY_0);

    if (ret != HAL_OK)
    {
        while (1);  /* 时钟切换本身失败, 极端罕见情况 */
    }

    SystemCoreClockUpdate();  /* SystemCoreClock = 8000000 */
    return 1;  /* 返回1告知调用方: HSE故障, 当前降级运行中 */
}

/**
 * @brief       系统时钟动态切换 (兼容旧接口)
 * @param       plln: PLL倍频系数, 使用 RCC_PLL_MUL2 ~ RCC_PLL_MUL16 宏
 * @note        与 sys_stm32_clock_init 功能完全相同, 保留此接口仅为兼容旧代码
 *              建议新代码直接使用 sys_stm32_clock_init()
 * @retval      同 sys_stm32_clock_init: 0=成功, 1=降级HSI, 2=失败
 */
uint8_t sys_clock_set(uint32_t plln)
{
    return sys_stm32_clock_init(plln);
}

/* ================================================================================
 * 文件结束
 * ================================================================================ */
