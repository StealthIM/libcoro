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

#ifdef __cplusplus
}
#endif
