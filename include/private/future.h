#pragma once

#include <stdlib.h>
#include <stdint.h>
#include "loop.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FUTURE_PENDING,
    FUTURE_DONE,
    FUTURE_REJECTED,
    FUTURE_CANCELLED
} future_state_t;

typedef struct future_s future_t;
typedef void (*future_cb_t)(future_t *fut, void *userdata);
typedef struct future_cb_node_s future_cb_node_t;
typedef struct callback_data_s callback_data_t;

typedef struct future_cb_node_s {
    future_cb_t cb;
    void *userdata;
    struct future_cb_node_s *next;
} future_cb_node_t;

struct future_s {
    future_state_t state;
    void *result;

    future_cb_node_t *callbacks;
};

typedef struct callback_data_s {
    future_cb_t cb;
    future_t *future;
    void *userdata;
} callback_data_t;

future_t *future_create();
void future_destroy(future_t *fut);

void future_done(future_t *fut, void *result);
void future_reject(future_t *fut, void *value);
void future_cancel(future_t *fut);

void future_add_done_callback(future_t *fut, future_cb_t cb, void *userdata);
void future_remove_done_callback(future_t *fut, future_cb_t cb);

future_t *async_sleep(uint64_t ms);

#define future_state(fut) ((fut)->state)
#define future_is_finished(fut) (future_state(fut) != FUTURE_PENDING)
#define future_result(fut) ((fut)->result)
#define future_is_pending(fut) (future_state(fut) == FUTURE_PENDING)
#define future_is_done(fut) (future_state(fut) == FUTURE_DONE)
#define future_is_rejected(fut) (future_state(fut) == FUTURE_REJECTED)
#define future_is_cancelled(fut) (future_state(fut) == FUTURE_CANCELLED)

#ifdef __cplusplus
}
#endif
