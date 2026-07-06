/*
 * 裸机 bootstrap —— 阶段 2b (NO_SYS=1, 无 FreeRTOS)。
 *
 * 和 boot_fr.c 的差异: 没有 RTOS 调度器。main 直接调 run_echo_loop()
 * (loop_run 自己手动 poll lwIP)。时间基准由 SysTick 提供 (lwIP 的 sys_now
 * 要单调毫秒), 而不是 FreeRTOS tick。
 *
 * 只 include lwIP + CMSIS 风格的 SysTick 寄存器, 不碰 FreeRTOS 头。
 *
 * ============================================================================
 * !!! 真部署清单 (QEMU 里跑通了, 但下面几项是仿真桩, 上真硬件必须替换) !!!
 *   1. wolf_gen_seed (本文件): 现在是 LCG, **非密码学安全**。真硬件用片上
 *      硬件 RNG (如 STM32 RNG / 累积 SysTick 抖动熵)。不换 = TLS 密钥可预测。
 *   2. NO_ASN_TIME (user_settings.h): 关掉了证书有效期校验, 因为裸机无可信
 *      时间源。真部署接 RTC / NTP 后去掉, 否则过期/未生效证书也会被接受。
 *   3. CPU_HZ (本文件): 假定 QEMU mps2-an385 的 25MHz。真硬件按实际主频改,
 *      否则 SysTick 毫秒基准偏, lwIP 超时/重传时序全错。
 *   4. WC_NO_HARDEN (user_settings.h): 关了 blinding 等侧信道加固, 真部署权衡。
 * ============================================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

extern int run_echo_loop(void);   /* test_fr_raw.c 提供 */

/* -------------------- SysTick 毫秒时基 -------------------- */
/* Cortex-M3 SysTick 寄存器 (SCS @ 0xE000E010)。 */
#define SYSTICK_CTRL  (*(volatile uint32_t*)0xE000E010)
#define SYSTICK_LOAD  (*(volatile uint32_t*)0xE000E014)
#define SYSTICK_VAL   (*(volatile uint32_t*)0xE000E018)

/* mps2-an385 默认核心时钟约 25MHz (QEMU 模型)。1ms = 25000 cycles。
 * 精度对 lwIP 超时够用 (环回测试)。 */
#define CPU_HZ        25000000u
#define TICK_HZ       1000u

static volatile uint32_t g_ms = 0;

void SysTick_Handler(void) { g_ms++; }

static void systick_init(void) {
    SYSTICK_LOAD = (CPU_HZ / TICK_HZ) - 1;
    SYSTICK_VAL  = 0;
    SYSTICK_CTRL = 0x7;   /* ENABLE | TICKINT | CLKSOURCE=core */
}

/* lwIP NO_SYS: sys_now() 由我们提供 (单调 ms)。 */
uint32_t sys_now(void) { return g_ms; }

/* lwIP arch/cc.h 的 LWIP_RAND: 简单 LCG (环回不需要密码学随机)。 */
unsigned int lwip_port_rand(void) {
    static unsigned int seed = 0x1234abcd;
    seed = seed * 1103515245u + 12345u;
    return seed;
}

/* wolfSSL 熵源 (user_settings.h 的 CUSTOM_RAND_GENERATE_SEED): QEMU 无硬件
 * RNG, LCG 填充。仅验证握手能跑通, 非密码学安全! 真部署必须换硬件 RNG/抖动熵。
 * (只在 TLS 构建 build_tls_raw.sh 里被链到; 明文构建不引用。) */
int wolf_gen_seed(unsigned char* output, unsigned int sz) {
    static unsigned int s = 0xC0FFEE11;
    for (unsigned int i = 0; i < sz; i++) {
        s = s * 1103515245u + 12345u;
        output[i] = (unsigned char)(s >> 16);
    }
    return 0;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("boot: bare-metal (NO_SYS) + lwIP raw + libcoro\n");
    systick_init();
    int rc = run_echo_loop();
    exit(rc);
}
