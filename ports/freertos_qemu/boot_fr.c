/*
 * FreeRTOS bootstrap (阶段 2a)。
 *
 * 只 include FreeRTOS 头, 不碰 libcoro 私有头 (libcoro 的 task.h 与
 * FreeRTOS task.h 同名, 同一翻译单元无法共存)。libcoro loop 逻辑在
 * test_fr_echo.c 里, 通过 run_echo_loop() 暴露。
 */

#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"
#include "task.h"

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
