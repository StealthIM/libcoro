/*
 * libcoro 最小示例: 用协程 DSL 并发跑两个"任务", 每个任务 await 若干次
 * async_sleep, 父任务再 await 子任务的完成值。
 *
 * 编译运行 (装好 libcoro 后):
 *   cmake -B build -DLIBCORO_OS_BACKEND=epoll && cmake --build build
 *   ./build/sleep_tasks
 */
#include <stdio.h>
#include <libcoro/libcoro.h>

/* 子任务: 睡两次, 返回一个整数 (打包成 void* 当作 future 结果)。 */
static task_t *task(worker) {
    gen_dec_vars(
        int result;
    );
    gen_begin(ctx);

    printf("[worker] start\n");
    gen_yield(async_sleep(50));     /* await: 把 future 交给驱动, 完成后恢复 */
    gen_var(result) = 21;
    gen_yield(async_sleep(50));

    printf("[worker] done -> %d\n", gen_var(result));
    gen_end((void *)(intptr_t)(gen_var(result) * 2));
}

/* 父任务: 起子任务, yield-from 等它完成, 读它的结果。 */
static task_t *task(parent) {
    gen_dec_vars(
        task_t *child;
        int value;
    );
    gen_begin(ctx);

    printf("[parent] spawning worker\n");
    gen_var(child) = worker();
    gen_yield_from_task(gen_var(child));    /* await 子任务 */

    gen_var(value) = (int)(intptr_t)gen_var(child)->future->result;
    printf("[parent] worker returned %d\n", gen_var(value));

    gen_end(NULL);
}

int main(void) {
    task_t *root = parent();
    loop_run(root);     /* 驱动事件循环, 直到 root 任务完成 */
    loop_destroy();
    return 0;
}
