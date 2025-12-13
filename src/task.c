#include <stdio.h>
#include <stdlib.h>
#include "task.h"

void task_destroy_sub_sub(loop_t *_, void *data) {
    task_t *task = data;
    gen_destroy(task->gen);
    free(task);
}

void task_destroy_sub(future_t *fut, void *task) {
    loop_call_soon(task_destroy_sub_sub, task);
}

task_t *task_create(gen_t *gen) {
    task_t *t = calloc(1, sizeof(task_t));
    if (!t) return NULL;
    t->future = future_create();
    if (!t->future) {
        free(t);
        return NULL;
    }
    t->gen = gen;
    // This callback will be called first, but the following callbacks might use this task
    // So we have to delay the actual destruction to the next loop iteration
    future_add_done_callback(t->future, task_destroy_sub, t);
    return t;
}

void task_destroy(task_t *task) {
    if (!task) return;
    // The only condition that we have to handle here is that the generator is not finished yet
    // So we just reject the future
    if (!gen_finished(task->gen)) {
        gen_close(task->gen);
        future_reject(task->future, NULL);
    }
}
void _task_run_cb(loop_t *_, void *userdata);
void _task_run_cb_future(future_t *_, void *userdata) {
    _task_run_cb(NULL, userdata);
}

void _task_run_cb(loop_t *_, void *userdata) {
    task_t *task = userdata;
    future_t *fut = NULL;
    while (!fut && !gen_finished(task->gen)) {
        fut = gen_next(task->gen);
        if (task->gen->ctx.yield_from_returned) {
            gen_t *inner = task->gen;
            while (inner->ctx.sub_gen) {
                inner = inner->ctx.sub_gen;
            }
            future_done(((task_t *)(inner->ctx.userdata))->future, task->gen->ctx.yield_from_val);
        }
    }
    if (gen_finished(task->gen)) {
        future_done(task->future, task->gen->ctx.ret_val);
        return;
    }

    future_add_done_callback(fut, _task_run_cb_future, task);
}

void task_run(task_t *task) {
    loop_call_soon(_task_run_cb, task);
}
