/*
 * 阶段 2a: offload 线程池端到端验证 (FreeRTOS)。
 *
 * offload 是 RTOS 路线相比裸机保留的核心特性 (异步 DNS 靠它)。这里在 loop
 * task 里用 loop_run_in_thread 提交一个阻塞函数, 验证:
 *   - offload worker (FreeRTOS task, 经 pal_thread) 能跑 fn。
 *   - 结果经完成队列 + loop_wake 回到 loop 主 task, resolve future。
 *   - gen_yield 在 future 上挂起/恢复正确。
 *
 * 只 include libcoro (不碰 FreeRTOS 头, 见 test_fr_echo.c 说明)。
 * bootstrap 复用 boot_fr.c, 它调 run_echo_loop() —— 这里同名提供。
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "libcoro.h"

static int g_pass = 0;

/* 在 offload worker 线程 (FreeRTOS task) 里跑的"阻塞"函数。 */
static void *blocking_job(void *arg) {
    int v = (int)(intptr_t)arg;
    /* 模拟阻塞计算 */
    volatile int acc = 0;
    for (int i = 0; i < v; i++) acc += i;
    return (void *)(intptr_t)(v * 2);
}

task_t* task(offload_task) {
    gen_dec_vars(
        future_t *fut;
    );
    gen_begin(ctx);

    gen_var(fut) = loop_run_in_thread(blocking_job, (void *)(intptr_t)21);
    if (!gen_var(fut)) { printf("run_in_thread failed\n"); gen_return(1); }

    gen_yield(gen_var(fut));   /* 挂起, 等 worker 完成 */

    {
        int result = (int)(intptr_t)future_result(gen_var(fut));
        if (result == 42) { g_pass = 1; printf("offload result=%d\n", result); }
        else              { printf("offload wrong result=%d\n", result); }
    }

    gen_end(0);
}

int run_echo_loop(void) {   /* 复用 boot_fr.c 的入口名 */
    printf("loop task: offload test\n");
    task_t *t = offload_task();
    loop_run(t);
    loop_destroy();
    printf("%s\n", g_pass ? "STAGE2A OFFLOAD OK" : "STAGE2A OFFLOAD FAIL");
    return g_pass ? 0 : 1;
}
