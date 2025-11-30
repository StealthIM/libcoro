#pragma once

#include "task.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loop_s loop_t;
typedef struct recv_data_s {
    void *userdata;
    char *data;
    unsigned long len;
} recv_data_t;
typedef void (*loop_cb_t)(loop_t *loop, void *userdata);

extern __thread loop_t *current_loop;

#define LOOP_EV_READ 0x01
#define LOOP_EV_WRITE 0x02

loop_t *loop_create_sub();
#define loop_get() loop_create()
static loop_t *loop_create() {
    if (!current_loop) current_loop = loop_create_sub();
    return current_loop;
}

void loop_destroy();

typedef struct task_s task_t;
void loop_run(task_t *task);
void loop_stop();

int loop_register_handle(void *handle, loop_cb_t cb, void *userdata);
int loop_unregister_handle(void *handle, bool close_socket);

typedef int32_t timer_id_t;
timer_id_t loop_add_timer(uint64_t when_ms, loop_cb_t cb, void *userdata);
int loop_cancel_timer(timer_id_t id);

void loop_call_soon(loop_cb_t cb, void *userdata);

uint64_t loop_time_ms();

#ifdef __cplusplus
}
#endif
