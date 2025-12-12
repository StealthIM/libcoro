#include "loop.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <assert.h>

/* thread-local current loop */
__thread loop_t *current_loop = NULL;

/* -------------------- internal types -------------------- */

typedef struct loop_op_s {
    loop_op_type_t type; /* public type */
    loop_op_id_t id;
    int fd; /* socket fd (or listen fd for accept) */

    loop_io_cb_t cb;
    void *userdata;

    int cancelled; /* logical cancellation flag */

    union {
        struct { char *buf; unsigned long len; } recv;
        struct { char *buf; unsigned long len; } send;
        struct { struct sockaddr_storage addr; socklen_t addrlen; int state; } conn;
        struct { int accept_fd; } acc;
    } u;

    struct loop_op_s *next;
} loop_op_t;

/* timer heap element */
typedef struct timer_req_s {
    loop_cb_t cb;
    void *userdata;
    timer_id_t id;
    uint64_t expire_ms;
    int cancelled;
} timer_req_t;

/* soon (microtask) node */
typedef struct soon_task_s {
    loop_cb_t cb;
    void *userdata;
    struct soon_task_s *next;
} soon_task_t;

/* main loop state */
struct loop_s {
    volatile int next_op_id;
    volatile int next_timer_id;
    int stop_flag;

    /* timer heap */
    timer_req_t **heap;
    int heap_size;
    int heap_cap;

    /* microtask queue */
    soon_task_t *soon_head;
    soon_task_t *soon_tail;

    /* active ops linked list */
    loop_op_t *ops_head;

    /* wake pipe to interrupt select */
    int wake_r;
    int wake_w;
};

/* -------------------- utilities -------------------- */

static uint64_t loop_time_ms_impl() {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
        return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
    }
    /* fallback */
    return 0;
}

uint64_t loop_time_ms() {
    return loop_time_ms_impl();
}

static int set_nonblocking(int fd) {
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) return -1;
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
    /* wake select */
    if (loop->wake_w >= 0) { char b = 1; ssize_t ignored = write(loop->wake_w, &b, 1); (void)ignored; }
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
    if (loop->wake_w >= 0) { char b = 1; ssize_t ignored = write(loop->wake_w, &b, 1); (void)ignored; }
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

/* -------------------- core IO ops (post/recv/send/connect/accept) -------------------- */

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
    op->type = LOOP_OP_RECV; op->id = alloc_op_id(loop); op->fd = fd; op->cb = cb; op->userdata = userdata; op->cancelled = 0;
    op->u.recv.buf = buf; op->u.recv.len = len;
    ops_add(loop, op);
    /* ensure non-blocking so recv won't block when polled */
    set_nonblocking(fd);
    /* wake select so it will include this fd */
    if (loop->wake_w >= 0) { char b = 1; ssize_t ignored = write(loop->wake_w, &b, 1); (void)ignored; }
    return op->id;
}

loop_op_id_t loop_post_send(void *handle, const char *buf, unsigned long len, loop_io_cb_t cb, void *userdata) {
    if (!handle || !cb || !buf || len == 0) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    int fd = (int)(intptr_t)handle;
    loop_op_t *op = calloc(1, sizeof(loop_op_t));
    if (!op) return LOOP_INVALID_OP_ID;
    op->type = LOOP_OP_SEND; op->id = alloc_op_id(loop); op->fd = fd; op->cb = cb; op->userdata = userdata; op->cancelled = 0;
    op->u.send.len = len;
    op->u.send.buf = malloc((size_t)len);
    if (!op->u.send.buf) { free(op); return LOOP_INVALID_OP_ID; }
    memcpy(op->u.send.buf, buf, (size_t)len);
    ops_add(loop, op);
    set_nonblocking(fd);
    if (loop->wake_w >= 0) { char b = 1; ssize_t ignored = write(loop->wake_w, &b, 1); (void)ignored; }
    return op->id;
}

loop_op_id_t loop_connect_async(void *handle, const struct sockaddr *addr, int addrlen, loop_io_cb_t cb, void *userdata) {
    if (!handle || !addr || !cb) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    int fd = (int)(intptr_t)handle;
    /* make non-blocking and initiate connect */
    set_nonblocking(fd);
    int ret = connect(fd, addr, addrlen);
    if (ret == 0) {
        /* immediate success — we'll still post an op so callback semantics are uniform */
    }
    loop_op_t *op = calloc(1, sizeof(loop_op_t));
    if (!op) return LOOP_INVALID_OP_ID;
    op->type = LOOP_OP_CONNECT; op->id = alloc_op_id(loop); op->fd = fd; op->cb = cb; op->userdata = userdata; op->cancelled = 0;
    op->u.conn.state = 0; /* pending */
    if (addrlen <= (int)sizeof(op->u.conn.addr)) {
        memcpy(&op->u.conn.addr, addr, addrlen);
        op->u.conn.addrlen = addrlen;
    }
    ops_add(loop, op);
    if (loop->wake_w >= 0) { char b = 1; ssize_t ignored = write(loop->wake_w, &b, 1); (void)ignored; }
    return op->id;
}

loop_op_id_t loop_accept_async(void *listen_handle, void **accept_handle_out, loop_io_cb_t cb, void *userdata) {
    if (!listen_handle || !cb) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    int listen_fd = (int)(intptr_t)listen_handle;
    loop_op_t *op = calloc(1, sizeof(loop_op_t));
    if (!op) return LOOP_INVALID_OP_ID;
    op->type = LOOP_OP_ACCEPT; op->id = alloc_op_id(loop); op->fd = listen_fd; op->cb = cb; op->userdata = userdata; op->cancelled = 0;
    op->u.acc.accept_fd = -1;
    ops_add(loop, op);
    set_nonblocking(listen_fd);
    if (loop->wake_w >= 0) { char b = 1; ssize_t ignored = write(loop->wake_w, &b, 1); (void)ignored; }
    /* Note: POSIX accept returns accepted fd only at accept time; we don't create accept socket here. */
    (void)accept_handle_out; /* unused in this implementation */
    return op->id;
}

int loop_cancel_op(loop_op_id_t id) {
    if (id == LOOP_INVALID_OP_ID) return -1;
    loop_t *loop = loop_get();
    loop_op_t *op = ops_find_by_id(loop, id);
    if (!op) return -1;
    /* mark cancelled logically; don't free here to avoid races with select iteration */
    op->cancelled = 1;
    /* wake select so it can clean up promptly */
    if (loop->wake_w >= 0) { char b = 1; ssize_t ignored = write(loop->wake_w, &b, 1); (void)ignored; }
    return 0;
}

/* -------------------- completion handling -------------------- */

static void complete_and_free_op(loop_t *loop, loop_op_t *op, int err, unsigned long bytes, char *rbuf) {
    /* remove from list */
    ops_remove(loop, op);

    if (op->cancelled) {
        /* logical cancellation: do not invoke callback; close accepted fd if any to avoid leak */
        if (op->type == LOOP_OP_ACCEPT && bytes != 0) {
            int afd = (int)bytes;
            close(afd);
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

static int calc_next_timeout_ms_for_select(loop_t *loop) {
    timer_req_t *top = heap_top(loop);
    if (!top) return -1; /* no timeout => wait indefinitely */
    uint64_t now = loop_time_ms();
    if (top->expire_ms <= now) return 0;
    uint64_t diff = top->expire_ms - now;
    if (diff > 1000000ULL) return -1;
    return (int)diff;
}

static void loop_run_once() {
    loop_t *loop = loop_get();
    run_pending_soon_and_timers(loop);

    /* prepare fd sets */
    fd_set readfds, writefds;
    FD_ZERO(&readfds); FD_ZERO(&writefds);
    int maxfd = -1;

    /* always monitor wake pipe read end */
    if (loop->wake_r >= 0) { FD_SET(loop->wake_r, &readfds); if (loop->wake_r > maxfd) maxfd = loop->wake_r; }

    /* iterate ops and prepare sets */
    loop_op_t *p = loop->ops_head;
    while (p) {
        if (p->cancelled) { p = p->next; continue; }
        switch (p->type) {
            case LOOP_OP_RECV:
                FD_SET(p->fd, &readfds);
                if (p->fd > maxfd) maxfd = p->fd;
                break;
            case LOOP_OP_SEND:
                FD_SET(p->fd, &writefds);
                if (p->fd > maxfd) maxfd = p->fd;
                break;
            case LOOP_OP_CONNECT:
                /* monitor writability to detect connect completion */
                FD_SET(p->fd, &writefds);
                if (p->fd > maxfd) maxfd = p->fd;
                break;
            case LOOP_OP_ACCEPT:
                FD_SET(p->fd, &readfds);
                if (p->fd > maxfd) maxfd = p->fd;
                break;
            default:
                break;
        }
        p = p->next;
    }

    /* calc timeout */
    int timeout_ms = calc_next_timeout_ms_for_select(loop);
    struct timeval tvbuf, *tv = NULL;
    if (timeout_ms >= 0) { tvbuf.tv_sec = timeout_ms / 1000; tvbuf.tv_usec = (timeout_ms % 1000) * 1000; tv = &tvbuf; }

    int nfds = select(maxfd + 1, &readfds, &writefds, NULL, tv);

    /* after wake, run soon/timers again */
    run_pending_soon_and_timers(loop);

    if (nfds < 0) {
        if (errno == EINTR) return;
        return;
    }
    if (nfds == 0) {
        /* timeout — timers already run above */
        return;
    }

    /* handle wake pipe */
    if (loop->wake_r >= 0 && FD_ISSET(loop->wake_r, &readfds)) {
        char buf[64]; while (read(loop->wake_r, buf, sizeof(buf)) > 0) {}
        /* fall through to process other fds */
    }

    /* iterate ops again to handle ready fds; collect completed ops into a small list to avoid modifying while iterating */
    loop_op_t *cur = loop->ops_head;
    loop_op_t *next;
    while (cur) {
        next = cur->next;
        if (cur->cancelled) {
            /* clean cancelled op (no callback) */
            ops_remove(loop, cur);
            if (cur->type == LOOP_OP_SEND && cur->u.send.buf) free(cur->u.send.buf);
            free(cur);
            cur = next; continue;
        }
        if (cur->type == LOOP_OP_RECV && FD_ISSET(cur->fd, &readfds)) {
            ssize_t r = recv(cur->fd, cur->u.recv.buf, cur->u.recv.len, 0);
            if (r < 0) {
                int err = errno;
                if (err == EAGAIN || err == EWOULDBLOCK) { /* try later */ }
                else {
                    complete_and_free_op(loop, cur, err, 0, NULL);
                }
            } else {
                unsigned long bytes = (unsigned long)r;
                complete_and_free_op(loop, cur, 0, bytes, NULL);
            }
        } else if (cur->type == LOOP_OP_SEND && FD_ISSET(cur->fd, &writefds)) {
            ssize_t s = send(cur->fd, cur->u.send.buf, cur->u.send.len, 0);
            if (s < 0) {
                int err = errno;
                if (err == EAGAIN || err == EWOULDBLOCK) { /* try later */ }
                else {
                    complete_and_free_op(loop, cur, err, 0, NULL);
                }
            } else {
                unsigned long bytes = (unsigned long)s;
                complete_and_free_op(loop, cur, 0, bytes, NULL);
            }
        } else if (cur->type == LOOP_OP_CONNECT && FD_ISSET(cur->fd, &writefds)) {
            int err = 0; socklen_t len = sizeof(err);
            if (getsockopt(cur->fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
                int e = errno;
                complete_and_free_op(loop, cur, e, 0, NULL);
            } else {
                if (err != 0) {
                    complete_and_free_op(loop, cur, err, 0, NULL);
                } else {
                    /* success */
                    complete_and_free_op(loop, cur, 0, 0, NULL);
                }
            }
        } else if (cur->type == LOOP_OP_ACCEPT && FD_ISSET(cur->fd, &readfds)) {
            struct sockaddr_storage addr; socklen_t alen = sizeof(addr);
            int afd = accept(cur->fd, (struct sockaddr*)&addr, &alen);
            if (afd < 0) {
                int e = errno;
                if (e == EAGAIN || e == EWOULDBLOCK) { /* nothing */ }
                else {
                    complete_and_free_op(loop, cur, e, 0, NULL);
                }
            } else {
                /* accepted one connection; set non-blocking and deliver fd in bytes field */
                set_nonblocking(afd);
                complete_and_free_op(loop, cur, 0, (unsigned long)afd, NULL);
            }
        }
        cur = next;
    }
}

void loop_run(task_t *task) {
    loop_t *loop = loop_get();
    if (task) task_run(task);
    while (!loop->stop_flag || loop->heap_size != 0 || loop->soon_head != NULL || any_pending_ops(loop)) {
        loop_run_once();
    }
}

/* -------------------- create / destroy / time -------------------- */

loop_t *loop_create_sub() {
    loop_t *loop = calloc(1, sizeof(loop_t));
    if (!loop) return NULL;
    loop->next_op_id = 0;
    loop->next_timer_id = 0;
    loop->heap = NULL; loop->heap_size = 0; loop->heap_cap = 0;
    loop->soon_head = loop->soon_tail = NULL;
    loop->ops_head = NULL;
    loop->stop_flag = 0;
    int pipefd[2];
    if (pipe(pipefd) < 0) {
        free(loop); return NULL;
    }
    loop->wake_r = pipefd[0]; loop->wake_w = pipefd[1];
    /* set pipe ends non-blocking */
    set_nonblocking(loop->wake_r);
    set_nonblocking(loop->wake_w);
    return loop;
}

void loop_stop() {
    loop_t *loop = loop_get();
    loop->stop_flag = 1;
    if (loop->wake_w >= 0) { char b = 1; ssize_t ignored = write(loop->wake_w, &b, 1); (void)ignored; }
}

void loop_destroy() {
    loop_t *loop = loop_get(); if (!loop) return;
    loop_stop();
    /* free ops */
    loop_op_t *op = loop->ops_head;
    while (op) {
        loop_op_t *n = op->next;
        if (op->type == LOOP_OP_SEND && op->u.send.buf) free(op->u.send.buf);
        free(op);
        op = n;
    }
    /* free timers */
    for (int i = 0; i < loop->heap_size; ++i) free(loop->heap[i]);
    free(loop->heap);
    /* free soon tasks */
    soon_task_t *s = loop->soon_head;
    while (s) { soon_task_t *n = s->next; free(s); s = n; }
    if (loop->wake_r >= 0) close(loop->wake_r);
    if (loop->wake_w >= 0) close(loop->wake_w);
    free(loop);
    current_loop = NULL;
}
