#include "task.h"

static void real_invoke_callback(loop_t *_, void *data) {
    callback_data_t *callback = data;
    callback->cb(callback->future, callback->userdata);
    free(data);
}

static void real_cleanup(loop_t *_, void *data) {
    future_t *fut = data;
    future_destroy(fut);
}

static void future_invoke_callbacks(future_t *fut) {
    future_cb_node_t *node = fut->callbacks;
    while (node) {
        callback_data_t *data = calloc(1, sizeof(callback_data_t));
        data->userdata = node->userdata;
        data->future = fut;
        data->cb = node->cb;
        loop_call_soon(real_invoke_callback, data);
        node = node->next;
    }
    loop_call_soon(real_cleanup, fut);
}

future_t *future_create() {
    future_t *f = calloc(1, sizeof(future_t));
    f->state = FUTURE_PENDING;
    return f;
}

void future_destroy(future_t *fut) {
    future_cb_node_t *node = fut->callbacks;
    while (node) {
        future_cb_node_t *next = node->next;
        free(node);
        node = next;
    }
    free(fut);
}

void future_done(future_t *fut, void *result) {
    if (fut->state != FUTURE_PENDING) return;
    fut->state = FUTURE_DONE;
    fut->result = result;
    future_invoke_callbacks(fut);
}

void future_reject(future_t *fut, void *value) {
    if (fut->state != FUTURE_PENDING) return;
    fut->state = FUTURE_REJECTED;
    fut->result = value;
    future_invoke_callbacks(fut);
}

void future_cancel(future_t *fut) {
    if (fut->state != FUTURE_PENDING) return;
    fut->state = FUTURE_CANCELLED;
    future_invoke_callbacks(fut);
}

void future_add_done_callback(future_t *fut, future_cb_t cb, void *userdata) {
    future_cb_node_t *node = malloc(sizeof(future_cb_node_t));
    node->cb = cb;
    node->userdata = userdata;
    node->next = NULL;

    // append to list
    if (!fut->callbacks) {
        fut->callbacks = node;
    } else {
        future_cb_node_t *cur = fut->callbacks;
        while (cur->next) cur = cur->next;
        cur->next = node;
    }

    if (fut->state != FUTURE_PENDING) {
        cb(fut, userdata);
    }
}

void sleep_cb(loop_t *_, void *userdata) {
    future_t *fut = userdata;
    if (fut) {
        future_done(fut, NULL);
    }
}

future_t *async_sleep(uint64_t ms) {
    future_t *fut = future_create();
    if (!fut) return NULL;
    uint64_t cur_ms = loop_time_ms();
    loop_add_timer(cur_ms + ms, sleep_cb, fut);
    return fut;
}
