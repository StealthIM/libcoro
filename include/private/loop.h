#pragma once

#include <stdint.h>
#include <stdbool.h>

#if defined(LIBCORO_LWIP_RAW)
/* 裸机 NO_SYS: lwIP 的 sockaddr 被 LWIP_SOCKET gate 掉 (LWIP_SOCKET=0),
 * 用端口自带的最小 sockaddr 定义 (只够 IPv4 环回)。 */
#include "raw_inet.h"
#elif defined(LIBCORO_LWIP)
/* lwIP 的 struct sockaddr 是 BSD 风格 (首字节 sa_len), 与 Linux 布局不同。
 * lwip 后端下全链路 (loop / asyncweb pal_socket) 必须统一用 lwIP 的定义。 */
#include "lwip/sockets.h"
#elif defined(WIN32)
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

/* ========== offload 集成钩子 (每个后端各实现) ========== */

/* 线程安全地唤醒 loop (可从 worker 线程调用)。 */
void loop_wake(loop_t *loop);

/* 取该 loop 的 offload 线程池句柄 (仅主线程)。 */
struct offload_pool_s *loop_get_offload(loop_t *loop);

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

#if defined(LIBCORO_LWIP_RAW)
/* ========== 裸机 raw lwIP socket setup (lwip_raw.c 实现) ==========
 * NO_SYS=1 下没有 lwip_socket()/fd; 这几个函数建/配 raw_conn_t 包装, 返回它
 * 作 loop 的 void* handle。只做非阻塞 setup, 不做 sync IO (裸机决策)。
 * connect/recv/send/accept 走上面的异步 op API, handle 传这里返回的指针。 */
void *anet_raw_new_tcp(void);
int   anet_raw_bind(void *handle, const struct sockaddr *addr, int addrlen);
int   anet_raw_listen(void *handle, int backlog);
void  anet_raw_close(void *handle);
/* 读 raw_conn 的本地绑定地址 (bind 后, 含 port==0 时内核选的临时端口)。
 * 只填 IPv4 sockaddr_in。成功返 0。 */
int   anet_raw_getsockname(void *handle, struct sockaddr *addr, int *addrlen);
#endif

#ifdef __cplusplus
}
#endif
