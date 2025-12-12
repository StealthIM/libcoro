#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct loop_s loop_t;
typedef struct task_s task_t;

/* basic callbacks */
typedef void (*loop_cb_t)(loop_t *loop, void *userdata);

/* timers */
typedef int32_t timer_id_t;

/* create/get loop (thread-local convenience) */
loop_t *loop_create_sub();
extern __thread loop_t *current_loop;
#define loop_get() loop_create()
static inline loop_t *loop_create() {
    if (!current_loop) current_loop = loop_create_sub();
    return current_loop;
}

void loop_destroy();
void loop_run(task_t *task);
void loop_stop();

timer_id_t loop_add_timer(uint64_t when_ms, loop_cb_t cb, void *userdata);
int loop_cancel_timer(timer_id_t id);

void loop_call_soon(loop_cb_t cb, void *userdata);
uint64_t loop_time_ms();

/* ========== IO Future API ========== */

typedef int32_t loop_op_id_t;
#define LOOP_INVALID_OP_ID ((loop_op_id_t)-1)

/* operation type */
typedef enum {
    LOOP_OP_RECV = 1,
    LOOP_OP_SEND = 2,
    LOOP_OP_CONNECT = 3,
    LOOP_OP_ACCEPT = 4
} loop_op_type_t;

/* recv data struct (passed to recv callbacks) */
typedef struct recv_data_s {
    void *userdata;
    char *data;
    unsigned long len;
} recv_data_t;

/* unified IO completion callback
 * - loop: the loop
 * - userdata: what the caller passed when posting the op
 * - type: operation type
 * - err: 0 = success, otherwise errno/WSA error
 * - bytes: number of bytes transferred (recv/send)
 * - rdata: valid when type==LOOP_OP_RECV (contains pointer to received buffer)
 */
typedef void (*loop_io_cb_t)(
    loop_t *loop,
    void *userdata,
    loop_op_type_t type,
    int err,
    unsigned long bytes,
    recv_data_t *rdata
);

/* Must be called once per socket (binds socket to IOCP). Returns 0 on success */
int loop_bind_handle(void *handle);

/* post one-shot operations. return op id or LOOP_INVALID_OP_ID on failure */
loop_op_id_t loop_post_recv(void *handle, char *buf, unsigned long len, loop_io_cb_t cb, void *userdata);
loop_op_id_t loop_post_send(void *handle, const char *buf, unsigned long len, loop_io_cb_t cb, void *userdata);
loop_op_id_t loop_connect_async(void *handle, const struct sockaddr *addr, int addrlen, loop_io_cb_t cb, void *userdata);
loop_op_id_t loop_accept_async(void *listen_handle, void **accept_handle_out, loop_io_cb_t cb, void *userdata);

/* cancel op */
int loop_cancel_op(loop_op_id_t id);

#ifdef __cplusplus
}
#endif
