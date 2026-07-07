/*
 * FreeRTOS bootstrap。
 *
 * 只 include FreeRTOS 头, 不碰 libcoro 私有头 (libcoro 的 task.h 与
 * FreeRTOS task.h 同名, 同一翻译单元无法共存)。libcoro loop 逻辑在
 * test_fr_echo.c 里, 通过 run_echo_loop() 暴露。
 */

#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "FreeRTOS.h"
#include <libcoro/task.h>

/* test_fr_echo.c 提供 (只依赖 libcoro/lwIP) */
extern int run_echo_loop(void);

static void loop_task(void *arg) {
    (void)arg;
    int rc = run_echo_loop();
    exit(rc);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("boot: FreeRTOS+lwIP+libcoro\n");
    xTaskCreate(loop_task, "loop", 4096, NULL, 3, NULL);
    vTaskStartScheduler();
    for (;;) {}
}

void vApplicationMallocFailedHook(void) { printf("FreeRTOS malloc failed\n"); exit(1); }
void vApplicationStackOverflowHook(TaskHandle_t t, char *n) { (void)t; printf("stack overflow: %s\n", n); exit(1); }

/* lwIP arch/cc.h 的 LWIP_RAND 用: 简单 LCG (环回测试不需要密码学随机)。 */
unsigned int lwip_port_rand(void) {
    static unsigned int seed = 0x1234abcd;
    seed = seed * 1103515245u + 12345u;
    return seed;
}

/* ---- wolfSSL 裸机桩 (熵源/计时) ---- */

/* 熵源: QEMU 无硬件 RNG, 用 LCG 填充 (仅验证握手能跑通, 非密码学安全!)。
 * 真硬件必须换成真随机 (硬件 RNG / 累积抖动熵)。 */
int wolf_gen_seed(unsigned char* output, unsigned int sz) {
    static unsigned int s = 0xC0FFEE11;
    for (unsigned int i = 0; i < sz; i++) {
        s = s * 1103515245u + 12345u;
        output[i] = (unsigned char)(s >> 16);
    }
    return 0;
}

/* 计时: FreeRTOS tick -> ms (configTICK_RATE_HZ=1000, 所以 tick==ms)。
 * 放这里让 probe_wolfssl.c 不必直接 include FreeRTOS.h。 */
unsigned long probe_now_ms(void) {
    return (unsigned long)xTaskGetTickCount();
}
