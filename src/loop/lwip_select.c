/*
 * libcoro lwIP 后端 (阶段 1: UNIX host port, socket 模式)。
 *
 * 结构镜像 linux_select.c, 但有三处 lwIP 特有改动:
 *   1. 协议栈初始化: 首次 loop_create 时 tcpip_init + 挂 loopback netif。
 *      (linux_select 直接用 OS, 没有这步。)
 *   2. 所有系统调用换 lwip_* (lwip_select / lwip_recv / lwip_accept ...)。
 *   3. 自唤醒: 裸 pipe() 在 lwIP socket 命名空间里不存在, 改用环回 UDP
 *      self-pipe —— 绑一个 127.0.0.1 UDP socket, loop_wake 往它发一字节。
 *
 * fd 全部是 lwIP 的 socket 描述符 (lwip_socket 返回), 与系统 fd 是两个
 * 命名空间, 不能混用。
 */

#include <libcoro/loop.h>
#include <internal/offload_internal.h>
#include <internal/loop_internal.h>
#include <libcoro/task.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "lwip/opt.h"
#include "lwip/tcpip.h"
#include "lwip/init.h"
#include "lwip/netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "lwip/sys.h"

/* thread-local current loop */
__thread loop_t *current_loop = NULL;

/* -------------------- internal types -------------------- */

typedef struct loop_op_s {
    loop_op_type_t type;
    loop_op_id_t id;
    int fd;                       /* lwIP socket fd */

    loop_io_cb_t cb;
    void *userdata;

    int cancelled;

    union {
        struct { char *buf; unsigned long len; } recv;
        struct { char *buf; unsigned long len; } send;
        struct { struct sockaddr_storage addr; socklen_t addrlen; int state; } conn;
        struct { int accept_fd; void **out; } acc;
    } u;

    struct loop_op_s *next;
} loop_op_t;

typedef struct timer_req_s {
    loop_cb_t cb;
    void *userdata;
    timer_id_t id;
    uint64_t expire_ms;
    int cancelled;
} timer_req_t;

typedef struct soon_task_s {
    loop_cb_t cb;
    void *userdata;
    struct soon_task_s *next;
} soon_task_t;

struct loop_s {
    volatile int next_op_id;
    volatile int next_timer_id;
    int stop_flag;

    timer_req_t **heap;
    int heap_size;
    int heap_cap;

    soon_task_t *soon_head;
    soon_task_t *soon_tail;

    loop_op_t *ops_head;

    /* 环回 UDP self-pipe: wake_r 收唤醒字节, wake_w 的目的地址是 wake_r。 */
    int wake_r;
    int wake_w;
    struct sockaddr_in wake_addr;   /* wake_r 实际绑定的地址 (含内核选的端口) */

    struct offload_pool_s *offload;
};

/* -------------------- lwIP 协议栈初始化 (进程级, 一次) -------------------- */

static int          g_lwip_inited = 0;
static sys_sem_t    g_tcpip_ready;

static void tcpip_init_done(void *arg) {
    sys_sem_signal((sys_sem_t *)arg);
}

/* 首次创建 loop 时初始化 lwIP 协议栈。socket 模式下 tcpip_init 会拉起
 * 内部 tcpip_thread; 环回 netif (loopif) 由 LWIP_HAVE_LOOPIF 自动创建。 */
static void ensure_lwip_init(void) {
    if (g_lwip_inited) return;
    g_lwip_inited = 1;

    if (sys_sem_new(&g_tcpip_ready, 0) != ERR_OK) abort();
    tcpip_init(tcpip_init_done, &g_tcpip_ready);
    sys_sem_wait(&g_tcpip_ready);   /* 等 tcpip_thread 就绪 */
    sys_sem_free(&g_tcpip_ready);
}

/* -------------------- utilities -------------------- */

uint64_t loop_time_ms() {
    return (uint64_t)sys_now();
}

static int set_nonblocking(int fd) {
    if (fd < 0) return -1;
    int flags = lwip_fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (lwip_fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
    return 0;
}

/* -------------------- heap helpers -------------------- */

static void heap_swap(timer_req_t **a, timer_req_t **b) { timer_req_t *tmp = *a; *a = *b; *b = tmp; }
static void heap_up(timer_req_t **heap, int idx) {
    while (idx > 0) {
        int p = (idx - 1) / 2;
        if (heap[p]->expire_ms <= heap[idx]->expire_ms) break;
        heap_swap(&heap[p], &heap[idx]);
        idx = p;
    }
}
static void heap_down(timer_req_t **heap, int n, int idx) {
    while (1) {
        int l = idx * 2 + 1, r = idx * 2 + 2, smallest = idx;
        if (l < n && heap[l]->expire_ms < heap[smallest]->expire_ms) smallest = l;
        if (r < n && heap[r]->expire_ms < heap[smallest]->expire_ms) smallest = r;
        if (smallest == idx) break;
        heap_swap(&heap[idx], &heap[smallest]);
        idx = smallest;
    }
}
static void heap_push(loop_t *loop, timer_req_t *t) {
    if (loop->heap_size == loop->heap_cap) {
        loop->heap_cap = loop->heap_cap ? loop->heap_cap * 2 : 16;
        timer_req_t **new_heap = realloc(loop->heap, loop->heap_cap * sizeof(timer_req_t*));
        if (!new_heap) abort();
        loop->heap = new_heap;
    }
    loop->heap[loop->heap_size] = t;
    heap_up(loop->heap, loop->heap_size);
    loop->heap_size++;
}
static timer_req_t* heap_top(loop_t *loop) { return loop->heap_size ? loop->heap[0] : NULL; }
static timer_req_t* heap_pop(loop_t *loop) {
    if (!loop->heap_size) return NULL;
    timer_req_t *ret = loop->heap[0];
    loop->heap_size--;
    if (loop->heap_size > 0) {
        loop->heap[0] = loop->heap[loop->heap_size];
        heap_down(loop->heap, loop->heap_size, 0);
    }
    return ret;
}

/* -------------------- op list helpers -------------------- */

static loop_op_id_t alloc_op_id(loop_t *loop) { return (loop_op_id_t)++loop->next_op_id; }
static void ops_add(loop_t *loop, loop_op_t *op) { op->next = loop->ops_head; loop->ops_head = op; }
static void ops_remove(loop_t *loop, loop_op_t *op) {
    loop_op_t **pp = &loop->ops_head;
    while (*pp && *pp != op) pp = &(*pp)->next;
    if (*pp) *pp = op->next;
}
static loop_op_t *ops_find_by_id(loop_t *loop, loop_op_id_t id) {
    loop_op_t *p = loop->ops_head;
    while (p) { if (p->id == id) return p; p = p->next; }
    return NULL;
}

/* -------------------- 环回 UDP 自唤醒 -------------------- */

/* loop_wake 从任意线程往 wake_r 的地址发一字节, 打断阻塞的 lwip_select。 */
void loop_wake(loop_t *loop) {
    if (!loop || loop->wake_w < 0) return;
    char b = 1;
    lwip_sendto(loop->wake_w, &b, 1, 0,
                (struct sockaddr *)&loop->wake_addr, sizeof(loop->wake_addr));
}

/* 主线程内的自唤醒 (投递 op / timer / stop 后打断当前 select)。 */
static void wake_self(loop_t *loop) {
    loop_wake(loop);
}

/* -------------------- soon tasks & timers -------------------- */

timer_id_t loop_add_timer(uint64_t when_ms, loop_cb_t cb, void *userdata) {
    if (!cb) return 0;
    loop_t *loop = loop_get();
    timer_req_t *t = calloc(1, sizeof(timer_req_t));
    t->cb = cb; t->userdata = userdata;
    t->id = (timer_id_t)++loop->next_timer_id;
    t->expire_ms = when_ms;
    t->cancelled = 0;
    heap_push(loop, t);
    wake_self(loop);
    return t->id;
}

int loop_cancel_timer(timer_id_t id) {
    loop_t *loop = loop_get();
    for (int i = 0; i < loop->heap_size; ++i) {
        if (loop->heap[i]->id == id) { loop->heap[i]->cancelled = 1; return 0; }
    }
    return -1;
}

void loop_call_soon(loop_cb_t cb, void *userdata) {
    if (!cb) return;
    loop_t *loop = loop_get();
    soon_task_t *t = malloc(sizeof(soon_task_t));
    t->cb = cb; t->userdata = userdata; t->next = NULL;
    if (!loop->soon_tail) loop->soon_head = loop->soon_tail = t; else { loop->soon_tail->next = t; loop->soon_tail = t; }
    wake_self(loop);
}

static void run_soon_tasks(loop_t *loop) {
    while (loop->soon_head) {
        soon_task_t *t = loop->soon_head; loop->soon_head = t->next; if (!loop->soon_head) loop->soon_tail = NULL;
        loop_cb_t cb = t->cb; void *ud = t->userdata; free(t); if (cb) cb(loop, ud);
    }
}

static void run_timers(loop_t *loop) {
    uint64_t now = loop_time_ms();
    while (1) {
        timer_req_t *top = heap_top(loop);
        if (!top) break;
        if (top->expire_ms > now) break;
        heap_pop(loop);
        if (!top->cancelled) {
            loop_cb_t cb = top->cb; void *ud = top->userdata; free(top); if (cb) cb(loop, ud);
        } else free(top);
    }
}

/* -------------------- core IO ops -------------------- */

int loop_bind_handle(void *handle) {
    if (!handle) return -1;
    int fd = (int)(intptr_t)handle;
    return set_nonblocking(fd) == 0 ? 0 : -1;
}

loop_op_id_t loop_post_recv(void *handle, char *buf, unsigned long len, loop_io_cb_t cb, void *userdata) {
    if (!handle || !cb || !buf || len == 0) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    int fd = (int)(intptr_t)handle;
    loop_op_t *op = calloc(1, sizeof(loop_op_t));
    if (!op) return LOOP_INVALID_OP_ID;
    op->type = LOOP_OP_RECV; op->id = alloc_op_id(loop); op->fd = fd; op->cb = cb; op->userdata = userdata;
    op->u.recv.buf = buf; op->u.recv.len = len;
    ops_add(loop, op);
    set_nonblocking(fd);
    wake_self(loop);
    return op->id;
}

loop_op_id_t loop_post_send(void *handle, const char *buf, unsigned long len, loop_io_cb_t cb, void *userdata) {
    if (!handle || !cb || !buf || len == 0) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    int fd = (int)(intptr_t)handle;
    loop_op_t *op = calloc(1, sizeof(loop_op_t));
    if (!op) return LOOP_INVALID_OP_ID;
    op->type = LOOP_OP_SEND; op->id = alloc_op_id(loop); op->fd = fd; op->cb = cb; op->userdata = userdata;
    op->u.send.len = len;
    op->u.send.buf = malloc((size_t)len);
    if (!op->u.send.buf) { free(op); return LOOP_INVALID_OP_ID; }
    memcpy(op->u.send.buf, buf, (size_t)len);
    ops_add(loop, op);
    set_nonblocking(fd);
    wake_self(loop);
    return op->id;
}

loop_op_id_t loop_connect_async(void *handle, const struct sockaddr *addr, int addrlen, loop_io_cb_t cb, void *userdata) {
    if (!handle || !addr || !cb) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    int fd = (int)(intptr_t)handle;
    set_nonblocking(fd);
    lwip_connect(fd, addr, addrlen);   /* 非阻塞: 通常返回 -1/EINPROGRESS */
    loop_op_t *op = calloc(1, sizeof(loop_op_t));
    if (!op) return LOOP_INVALID_OP_ID;
    op->type = LOOP_OP_CONNECT; op->id = alloc_op_id(loop); op->fd = fd; op->cb = cb; op->userdata = userdata;
    op->u.conn.state = 0;
    if (addrlen <= (int)sizeof(op->u.conn.addr)) {
        memcpy(&op->u.conn.addr, addr, addrlen);
        op->u.conn.addrlen = addrlen;
    }
    ops_add(loop, op);
    wake_self(loop);
    return op->id;
}

loop_op_id_t loop_accept_async(void *listen_handle, void **accept_handle_out, loop_io_cb_t cb, void *userdata) {
    if (!listen_handle || !cb) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    int listen_fd = (int)(intptr_t)listen_handle;
    loop_op_t *op = calloc(1, sizeof(loop_op_t));
    if (!op) return LOOP_INVALID_OP_ID;
    op->type = LOOP_OP_ACCEPT; op->id = alloc_op_id(loop); op->fd = listen_fd; op->cb = cb; op->userdata = userdata;
    op->u.acc.accept_fd = -1;
    op->u.acc.out = accept_handle_out;
    ops_add(loop, op);
    set_nonblocking(listen_fd);
    wake_self(loop);
    return op->id;
}

int loop_cancel_op(loop_op_id_t id) {
    if (id == LOOP_INVALID_OP_ID) return -1;
    loop_t *loop = loop_get();
    loop_op_t *op = ops_find_by_id(loop, id);
    if (!op) return -1;
    op->cancelled = 1;
    wake_self(loop);
    return 0;
}

/* -------------------- completion handling -------------------- */

static void complete_and_free_op(loop_t *loop, loop_op_t *op, int err, unsigned long bytes, char *rbuf) {
    ops_remove(loop, op);

    if (op->cancelled) {
        if (op->type == LOOP_OP_ACCEPT && bytes != 0) {
            int afd = (int)bytes;
            lwip_close(afd);
        }
        if (op->type == LOOP_OP_SEND && op->u.send.buf) free(op->u.send.buf);
        free(op);
        return;
    }

    recv_data_t r;
    recv_data_t *rptr = NULL;
    if (op->type == LOOP_OP_RECV) {
        r.userdata = op->userdata;
        r.data = rbuf ? rbuf : op->u.recv.buf;
        r.len = bytes;
        rptr = &r;
    }

    if (op->cb) op->cb(loop, op->userdata, op->type, err, bytes, rptr);

    if (op->type == LOOP_OP_SEND) {
        if (op->u.send.buf) free(op->u.send.buf);
    }

    free(op);
}

/* -------------------- main loop run_once & run -------------------- */

static int any_pending_ops(loop_t *loop) { return loop->ops_head != NULL; }

static void run_pending_soon_and_timers(loop_t *loop) { run_soon_tasks(loop); run_timers(loop); }

static int calc_next_timeout_ms(loop_t *loop) {
    timer_req_t *top = heap_top(loop);
    if (!top) return -1;
    uint64_t now = loop_time_ms();
    if (top->expire_ms <= now) return 0;
    uint64_t diff = top->expire_ms - now;
    if (diff > 1000000ULL) return -1;
    return (int)diff;
}

static void loop_run_once() {
    loop_t *loop = loop_get();
    run_pending_soon_and_timers(loop);

    fd_set readfds, writefds;
    FD_ZERO(&readfds); FD_ZERO(&writefds);
    int maxfd = -1;

    if (loop->wake_r >= 0) { FD_SET(loop->wake_r, &readfds); if (loop->wake_r > maxfd) maxfd = loop->wake_r; }

    loop_op_t *p = loop->ops_head;
    while (p) {
        if (p->cancelled) { p = p->next; continue; }
        switch (p->type) {
            case LOOP_OP_RECV:
            case LOOP_OP_ACCEPT:
                FD_SET(p->fd, &readfds);
                if (p->fd > maxfd) maxfd = p->fd;
                break;
            case LOOP_OP_SEND:
            case LOOP_OP_CONNECT:
                FD_SET(p->fd, &writefds);
                if (p->fd > maxfd) maxfd = p->fd;
                break;
            default:
                break;
        }
        p = p->next;
    }

    int timeout_ms = calc_next_timeout_ms(loop);
    struct timeval tvbuf, *tv = NULL;
    if (timeout_ms >= 0) { tvbuf.tv_sec = timeout_ms / 1000; tvbuf.tv_usec = (timeout_ms % 1000) * 1000; tv = &tvbuf; }

    int nfds = lwip_select(maxfd + 1, &readfds, &writefds, NULL, tv);

    run_pending_soon_and_timers(loop);

    if (nfds < 0) {
        return; /* EINTR 等: 下一轮重试 */
    }
    if (nfds == 0) {
        return; /* 超时: timers 已在上面跑过 */
    }

    /* 排空唤醒 UDP 字节 */
    if (loop->wake_r >= 0 && FD_ISSET(loop->wake_r, &readfds)) {
        char buf[64];
        while (lwip_recvfrom(loop->wake_r, buf, sizeof(buf), 0, NULL, NULL) > 0) {}
    }

    loop_op_t *cur = loop->ops_head;
    loop_op_t *next;
    while (cur) {
        next = cur->next;
        if (cur->cancelled) {
            ops_remove(loop, cur);
            if (cur->type == LOOP_OP_SEND && cur->u.send.buf) free(cur->u.send.buf);
            free(cur);
            cur = next; continue;
        }
        if (cur->type == LOOP_OP_RECV && FD_ISSET(cur->fd, &readfds)) {
            int r = lwip_recv(cur->fd, cur->u.recv.buf, cur->u.recv.len, 0);
            if (r < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) { /* try later */ }
                else complete_and_free_op(loop, cur, errno, 0, NULL);
            } else {
                complete_and_free_op(loop, cur, 0, (unsigned long)r, NULL);
            }
        } else if (cur->type == LOOP_OP_SEND && FD_ISSET(cur->fd, &writefds)) {
            int s = lwip_send(cur->fd, cur->u.send.buf, cur->u.send.len, 0);
            if (s < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) { /* try later */ }
                else complete_and_free_op(loop, cur, errno, 0, NULL);
            } else {
                complete_and_free_op(loop, cur, 0, (unsigned long)s, NULL);
            }
        } else if (cur->type == LOOP_OP_CONNECT && FD_ISSET(cur->fd, &writefds)) {
            int err = 0; socklen_t len = sizeof(err);
            if (lwip_getsockopt(cur->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
                complete_and_free_op(loop, cur, errno, 0, NULL);
            } else {
                complete_and_free_op(loop, cur, err, 0, NULL);
            }
        } else if (cur->type == LOOP_OP_ACCEPT && FD_ISSET(cur->fd, &readfds)) {
            struct sockaddr_storage addr; socklen_t alen = sizeof(addr);
            int afd = lwip_accept(cur->fd, (struct sockaddr*)&addr, &alen);
            if (afd < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) { /* nothing */ }
                else complete_and_free_op(loop, cur, errno, 0, NULL);
            } else {
                set_nonblocking(afd);
                if (cur->u.acc.out) *cur->u.acc.out = (void*)(intptr_t)afd;
                complete_and_free_op(loop, cur, 0, 0, NULL);
            }
        }
        cur = next;
    }

    offload_drain_completions(loop->offload);
}

void loop_run(task_t *task) {
    loop_t *loop = loop_get();
    if (task) task_run(task);
    while (!loop->stop_flag &&
           (loop->heap_size != 0 ||
            loop->soon_head != NULL ||
            any_pending_ops(loop) ||
            offload_has_pending(loop->offload))) {
        loop_run_once();
    }
}

/* -------------------- create / destroy -------------------- */

/* 创建环回 UDP self-pipe: 绑一个 127.0.0.1 随机端口的 UDP socket 作 wake_r,
 * 另开一个 UDP socket 作 wake_w, 目的地址记为 wake_r 的实际地址。 */
static int setup_wake_pipe(loop_t *loop) {
    loop->wake_r = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    loop->wake_w = lwip_socket(AF_INET, SOCK_DGRAM, 0);
    if (loop->wake_r < 0 || loop->wake_w < 0) return -1;

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = 0;                                   /* 内核选端口 */
    a.sin_addr.s_addr = PP_HTONL(INADDR_LOOPBACK);    /* 127.0.0.1 */
    if (lwip_bind(loop->wake_r, (struct sockaddr*)&a, sizeof(a)) < 0) return -1;

    socklen_t alen = sizeof(loop->wake_addr);
    memset(&loop->wake_addr, 0, sizeof(loop->wake_addr));
    if (lwip_getsockname(loop->wake_r, (struct sockaddr*)&loop->wake_addr, &alen) < 0) return -1;

    set_nonblocking(loop->wake_r);
    set_nonblocking(loop->wake_w);
    return 0;
}

loop_t *loop_create_sub() {
    ensure_lwip_init();

    loop_t *loop = calloc(1, sizeof(loop_t));
    if (!loop) return NULL;
    loop->wake_r = -1;
    loop->wake_w = -1;
    if (setup_wake_pipe(loop) != 0) {
        if (loop->wake_r >= 0) lwip_close(loop->wake_r);
        if (loop->wake_w >= 0) lwip_close(loop->wake_w);
        free(loop);
        return NULL;
    }
    loop->offload = offload_pool_create(loop);
    return loop;
}

void loop_stop() {
    loop_t *loop = loop_get();
    loop->stop_flag = 1;
    wake_self(loop);
}

struct offload_pool_s *loop_get_offload(loop_t *loop) {
    return loop ? loop->offload : NULL;
}

void loop_destroy() {
    loop_t *loop = loop_get(); if (!loop) return;
    loop_stop();
    offload_pool_destroy(loop->offload);
    loop->offload = NULL;

    loop_op_t *op = loop->ops_head;
    while (op) {
        loop_op_t *n = op->next;
        if (op->type == LOOP_OP_SEND && op->u.send.buf) free(op->u.send.buf);
        free(op);
        op = n;
    }
    for (int i = 0; i < loop->heap_size; ++i) free(loop->heap[i]);
    free(loop->heap);
    soon_task_t *s = loop->soon_head;
    while (s) { soon_task_t *n = s->next; free(s); s = n; }
    if (loop->wake_r >= 0) lwip_close(loop->wake_r);
    if (loop->wake_w >= 0) lwip_close(loop->wake_w);
    free(loop);
    current_loop = NULL;
}
