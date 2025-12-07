#pragma once

#include <stddef.h>
#include "future.h"
#include "generator.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct future_s future_t;
typedef struct task_s {
    future_t *future;
    gen_t *gen;
} task_t;

task_t *task_create(gen_t *cb);
void task_destroy(task_t *task);
void task_run(task_t *task);

#define task_arg(name) \
    name (type data); \
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
