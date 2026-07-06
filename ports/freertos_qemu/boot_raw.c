/*
 * 裸机 bootstrap —— 阶段 2b (NO_SYS=1, 无 FreeRTOS)。
 *
 * 和 boot_fr.c 的差异: 没有 RTOS 调度器。main 直接调 run_echo_loop()
 * (loop_run 自己手动 poll lwIP)。时间基准由 SysTick 提供 (lwIP 的 sys_now
 * 要单调毫秒), 而不是 FreeRTOS tick。
 *
 * 只 include lwIP + CMSIS 风格的 SysTick 寄存器, 不碰 FreeRTOS 头。
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

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("boot: bare-metal (NO_SYS) + lwIP raw + libcoro\n");
    systick_init();
    int rc = run_echo_loop();
    exit(rc);
}
