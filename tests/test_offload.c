#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include "libcoro.h"

/* 在 worker 线程执行的阻塞函数:睡一会儿再返回 (arg*2)。
   返回值 (void*) 直接成为 future 的 result。 */
static void *blocking_double(void *arg) {
    usleep(20 * 1000); /* 20ms,模拟阻塞调用 (如 getaddrinfo) */
    intptr_t v = (intptr_t)arg;
    return (void *)(intptr_t)(v * 2);
}

/* 协程:并发提交多个 offload 任务,逐个 yield 其 future,校验结果。 */
static task_t* task(offload_task) {
    gen_dec_vars(
        future_t *f1;
        future_t *f2;
        future_t *f3;
    );
    gen_begin(ctx);

    /* 三个任务几乎同时提交,验证线程池并发 */
    gen_var(f1) = loop_run_in_thread(blocking_double, (void *)(intptr_t)21);
    gen_var(f2) = loop_run_in_thread(blocking_double, (void *)(intptr_t)50);
    gen_var(f3) = loop_run_in_thread(blocking_double, (void *)(intptr_t)100);
    assert(gen_var(f1) && gen_var(f2) && gen_var(f3));

    gen_yield(gen_var(f1));
    assert(future_is_done(gen_var(f1)));
    assert((intptr_t)future_result(gen_var(f1)) == 42);

    gen_yield(gen_var(f2));
    assert((intptr_t)future_result(gen_var(f2)) == 100);

    gen_yield(gen_var(f3));
    assert((intptr_t)future_result(gen_var(f3)) == 200);

    printf("offload results: 42 / 100 / 200 OK\n");
    gen_end((void *)(intptr_t)42);
}

static void check_done(future_t *fut, void *userdata) {
    (void)userdata;
    assert(future_is_done(fut));
    assert((intptr_t)future_result(fut) == 42);
}

int test_offload() {
    printf("=== Testing offload (loop_run_in_thread) ===\n");
    task_t *t = offload_task();
    future_add_done_callback(t->future, check_done, NULL);
    loop_run(t);
    loop_destroy();
    printf("=== Test passed ===\n");
    return 0;
}
