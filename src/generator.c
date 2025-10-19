#include <stddef.h>
#include <stdlib.h>
#include "generator.h"

gen_t *gen_create(gen_func *func, void *userdata) {
    gen_t *gen = calloc(1, sizeof(gen_t));
    if (!gen) return NULL;
    gen->func = func;
    gen->ctx.userdata = userdata;
    gen->ctx.lineno = 0;
    gen->ctx.state = GEN_STATE_START;
    gen->ctx.stack_vars = NULL;
    return gen;
}

void gen_destroy(gen_t *gen) {
    if (!gen) return;
    if (gen->ctx.stack_vars) free(gen->ctx.stack_vars);
    free(gen);
}

void *gen_send(gen_t *gen, void *arg) {
    if (!gen) return NULL;
    if (gen_finished(gen)) return NULL;
    if (gen_in_yield_from(gen)) {
        if (!gen_finished(gen->ctx.sub_gen)) {
            return gen_send(gen->ctx.sub_gen, NULL);
        }
        gen_destroy(gen->ctx.sub_gen);
        gen->ctx.sub_gen = NULL;
        gen->ctx.state = GEN_STATE_MIDDLE;
    }
    gen_ret_t ret = gen->func(&gen->ctx, arg);
    if (ret == GEN_YIELD_FROM) {
        return gen_send(gen->ctx.sub_gen, NULL);  // Do the first step of the sub-generator
    }
    return gen->ctx.ret_val;
}

void gen_close(gen_t *gen) {
    if (!gen) return;
    if (gen_in_yield_from(gen)) {
        gen->ctx.lineno = -gen->ctx.lineno; // Go to the cleanup block for yield from statement
        gen->func(&gen->ctx, NULL);
    }
    gen->ctx.lineno = -1;   // Do the real cleanup
    gen->func(&gen->ctx, NULL);
}
