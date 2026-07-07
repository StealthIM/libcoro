#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <libcoro/libcoro.h>

/*
 * 异步生成器 (gen_emit) 测试。
 *
 * 一个顶层 task 生成器边 await IO (async_sleep) 边产出一串整数 (gen_emit)。
 * 验证:
 *   - on_emit 回调按顺序收到全部 N 个产出值
 *   - emit 与 await 交替不乱序、不丢
 *   - 生成器最后 gen_end 正常完成 (task future done, ret_val 正确)
 *   - 全程 ASan 零泄漏 (emit 轮无 leaf future, awaiting 保持 NULL)
 */

#define EMIT_N 5

static int g_seq[EMIT_N];   /* on_emit 收到的值, 按序 */
static int g_count = 0;

static void on_emit(void *item, void *userdata) {
    int v = (int)(intptr_t)item;
    assert(userdata == (void*)0xBEEF);
    assert(g_count < EMIT_N);
    g_seq[g_count++] = v;
}

/* 顶层生成器: 交替 await sleep 与 emit i*10, 共 EMIT_N 个, 最后返回 EMIT_N。 */
static task_t* task(emitter_task) {
    gen_dec_vars(
        int i;
    );
    gen_begin(ctx);

    for (gen_var(i) = 0; gen_var(i) < EMIT_N; gen_var(i)++) {
        gen_yield(async_sleep(5));                 /* await IO */
        gen_emit((void*)(intptr_t)(gen_var(i) * 10));  /* 产出数据项 */
    }

    gen_end((void*)(intptr_t)EMIT_N);
}

static void done_check(future_t *fut, void *userdata) {
    assert(future_is_done(fut));
    assert(future_result(fut) == (void*)(intptr_t)EMIT_N);
}

int test_emit() {
    printf("=== Testing async generator (gen_emit) ===\n");

    task_t *t = emitter_task();
    task_set_on_emit(t, on_emit, (void*)0xBEEF);
    future_add_done_callback(t->future, done_check, NULL);

    loop_run(t);

    /* 校验产出序列 */
    assert(g_count == EMIT_N);
    for (int i = 0; i < EMIT_N; i++) {
        printf("emit[%d] = %d\n", i, g_seq[i]);
        assert(g_seq[i] == i * 10);
    }

    loop_destroy();
    printf("=== Test passed ===\n");
    return 0;
}
