#pragma once

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    GEN_STATE_START = 0,
    GEN_STATE_MIDDLE,
    GEN_STATE_YIELD_FROM,
    GEN_STATE_END,
} gen_state_t;

typedef enum {
    GEN_NORMAL = 0,
    GEN_YIELD_FROM
} gen_ret_t;

typedef struct gen_s gen_t;
typedef struct future_s future_t;

typedef struct gen_ctx_s {
    int lineno;
    gen_state_t state;
    void *stack_vars;
    void *userdata;
    void *yield_val;
    void *yield_from_val;
    int yield_from_returned;
    void *ret_val;
    gen_t *sub_gen;
} gen_ctx_t;

typedef gen_ret_t (gen_func)(gen_ctx_t *ctx, void *arg);

typedef struct gen_s {
    gen_ctx_t ctx;
    gen_func *func;
} gen_t;

gen_t *gen_create(gen_func *func, void *userdata);
void gen_destroy(gen_t *gen);

#define gen_finished(gen) ((gen)->ctx.state == GEN_STATE_END)
#define gen_is_running(gen) ((gen)->ctx.state != GEN_STATE_END)
#define gen_in_yield_from(gen) ((gen)->ctx.state == GEN_STATE_YIELD_FROM)
#define gen_userdata() (__ctx->userdata)

#define gen_dec_vars(vars) \
    typedef struct { vars; } __stack_vars;
#define gen_var(var) (((__stack_vars*)(__ctx->stack_vars))->var)
#define gen_begin(ctx) \
    gen_ctx_t *__ctx = ctx; \
    switch(ctx->lineno) { \
        case 0: \
            ctx->stack_vars = calloc(1, sizeof(__stack_vars)); \
        case __LINE__:
#define gen_yield(val) \
            __ctx->lineno = __LINE__; \
            __ctx->state = GEN_STATE_MIDDLE; \
            __ctx->yield_val = (future_t*)(val); \
            return GEN_NORMAL; \
        case __LINE__:;
#define _CONNECT1(x,y) x##y
#define _CONNECT2(x,y) _CONNECT1(x,y)
#define gen_yield_from(gen) \
            __ctx->sub_gen = (gen); \
            __ctx->state = GEN_STATE_YIELD_FROM; \
            __ctx->lineno = __LINE__; \
            return GEN_YIELD_FROM; \
        case -__LINE__: \
            gen_close(__ctx->sub_gen); \
            gen_destroy(__ctx->sub_gen); \
            return GEN_NORMAL; \
        case __LINE__:
#define gen_yield_from_task(task) \
        __ctx->sub_gen = (task->gen); \
        __ctx->userdata = (task); \
        __ctx->state = GEN_STATE_YIELD_FROM; \
        __ctx->lineno = __LINE__; \
        return GEN_YIELD_FROM; \
    case -__LINE__: \
        gen_close(__ctx->sub_gen); \
        gen_destroy(__ctx->sub_gen); \
        return GEN_NORMAL; \
    case __LINE__:
#define gen_yield_from_val() __ctx->yield_from_val
#define gen_cleanup() \
        case -1:
#define gen_end(val) \
            __ctx->state = GEN_STATE_END; \
            __ctx->yield_val = NULL; \
            __ctx->ret_val = (void*)(val);; \
            return GEN_NORMAL; \
        default: \
            return GEN_NORMAL; \
    } \
    return -1;
#define gen_return(val) \
    __ctx->state = GEN_STATE_END; \
    __ctx->yield_val = NULL; \
    __ctx->ret_val = (void*)(val);; \
    return GEN_NORMAL; \

#define gen_for(type, val, gen) \
    type val; \
    for(;!gen_finished(gen) && ((val = (type)gen_next(gen)) || 1); )

void *gen_send(gen_t *gen, void *arg);
#define gen_next(gen) gen_send((gen), NULL)
void gen_close(gen_t *gen);

#define generator_arg(name) \
    name (type data); \
    gen_ret_t _CONNECT1(name, _sub) (gen_ctx_t *ctx, void *arg); \
    gen_t* name (void* data) { \
        return gen_create(_CONNECT1(name, _sub), data); \
    } \
    gen_ret_t _CONNECT1(name, _sub) (gen_ctx_t *ctx, void *arg)

#define generator(name) \
    name (); \
    gen_ret_t _CONNECT1(name, _sub) (gen_ctx_t *ctx, void *arg); \
    gen_t* name () { \
        return gen_create(_CONNECT1(name, _sub), NULL); \
    } \
    gen_ret_t _CONNECT1(name, _sub) (gen_ctx_t *ctx, void *arg)

#ifdef __cplusplus
}
#endif
