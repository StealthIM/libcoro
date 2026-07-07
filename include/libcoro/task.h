#pragma once

#include <stddef.h>
#include <libcoro/future.h>
#include <libcoro/generator.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct future_s future_t;

/* 异步生成器的产出回调: 协程 gen_emit(item) 时, 驱动器调用它交付 item。
 * 只在 gen_emit 那一 tick 内有效 —— 若 item 需留存, 回调里自行拷贝。 */
typedef void (*task_emit_cb_t)(void *item, void *userdata);

typedef struct task_s {
    future_t *future;   /* this task's own completion future */
    gen_t *gen;
    future_t *awaiting; /* leaf future the driver is currently blocked on; owned here */
    task_emit_cb_t on_emit;   /* gen_emit 产出回调 (NULL = 不消费, 见 task_set_on_emit) */
    void *emit_userdata;
} task_t;

task_t *task_create(gen_t *cb);
void task_remove_auto_destroy(task_t *task);
void task_destroy(task_t *task);
void task_run(task_t *task);

/* 设置 gen_emit 的产出回调。须在 task_run 之前调用。 */
void task_set_on_emit(task_t *task, task_emit_cb_t cb, void *userdata);

#define task_arg(name) \
    name (void* data); \
    gen_ret_t _CONNECT1(name, _sub) (gen_ctx_t *ctx, void *arg); \
    task_t* name (void* data) { \
        gen_t* gen = gen_create(_CONNECT1(name, _sub), data); \
        if (!gen) return NULL; \
        return task_create(gen);\
    } \
    gen_ret_t _CONNECT1(name, _sub) (gen_ctx_t *ctx, void *arg)

#define task(name) \
    name (); \
    gen_ret_t _CONNECT1(name, _sub) (gen_ctx_t *ctx, void *arg); \
    task_t* name () { \
        gen_t* gen = gen_create(_CONNECT1(name, _sub), NULL); \
        if (!gen) return NULL; \
        return task_create(gen);\
    } \
    gen_ret_t _CONNECT1(name, _sub) (gen_ctx_t *ctx, void *arg)

#ifdef __cplusplus
}
#endif
