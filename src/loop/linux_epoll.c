#define _GNU_SOURCE
#include "loop.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <assert.h>
#include <time.h>

#include "task.h"

/* thread-local current loop (matches header extern __thread declaration) */
__thread loop_t *current_loop = NULL;

/* -------------------- internal data -------------------- */

typedef struct loop_op_s {
    /* common */
    int fd;                     /* associated fd (socket) */
    loop_op_type_t type;
    loop_op_id_t id;
    loop_io_cb_t cb;
    void *userdata;
    int cancelled;

    /* per-op data */
    union {
        struct { char *buf; unsigned long len; } recv;
        struct {
            char *buf;
            unsigned long len;
            unsigned long sent_so_far;
        } send;
        struct {
            struct sockaddr_storage addr;
            socklen_t addrlen;
        } conn;
        struct {
            int accept_fd; /* pre-created accept fd returned immediately to caller */
            int got_accepted; /* flag whether accept completed and accept_fd now refers to accepted conn */
        } acc;
    };

    struct loop_op_s *next;
} loop_op_t;

typedef struct soon_task_s {
    loop_cb_t cb;
    void *userdata;
    struct soon_task_s *next;
} soon_task_t;

typedef struct timer_req_s {
    loop_cb_t cb;
    void *userdata;
    timer_id_t id;
    uint64_t expire_ms;
    int cancelled;
} timer_req_t;

/* main loop state */
struct loop_s {
    int epfd;
    int event_fd; /* eventfd used to wake epoll for soon/timers/stop */
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

    /* active ops linked list (for cancel lookup) */
    loop_op_t *ops_head;

    /* per-fd reference count of registered events (to compute epoll interest) */
    /* We'll maintain op list and recompute epoll interest when ops change. */
};

/* -------------------- utility helpers -------------------- */

static void make_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) flags = 0;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int set_close_on_exec(int fd) {
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
}

/* -------------------- heap helpers -------------------- */

static void heap_swap(timer_req_t **a, timer_req_t **b) {
    timer_req_t *tmp = *a;
    *a = *b;
    *b = tmp;
}

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

static timer_req_t* heap_top(loop_t *loop) {
    return loop->heap_size ? loop->heap[0] : NULL;
}

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

static loop_op_id_t alloc_op_id(loop_t *loop) {
    return (loop_op_id_t) __atomic_add_fetch(&loop->next_op_id, 1, __ATOMIC_RELAXED);
}

static void ops_add(loop_t *loop, loop_op_t *op) {
    op->next = loop->ops_head;
    loop->ops_head = op;
}

static void ops_remove(loop_t *loop, loop_op_t *op) {
    loop_op_t **pp = &loop->ops_head;
    while (*pp && *pp != op) pp = &(*pp)->next;
    if (*pp) *pp = op->next;
}

static loop_op_t *ops_find_by_id(loop_t *loop, loop_op_id_t id) {
    loop_op_t *p = loop->ops_head;
    while (p) {
        if (p->id == id) return p;
        p = p->next;
    }
    return NULL;
}

/* find ops for a given fd (returns head of list; not exclusive) */
static loop_op_t *ops_find_by_fd(loop_t *loop, int fd) {
    loop_op_t *p = loop->ops_head;
    loop_op_t *head = NULL;
    while (p) {
        if (p->fd == fd && !p->cancelled) {
            /* build a small temporary list by linking via next pointers of a copy struct? */
            /* To keep things simple, return first matching and caller should iterate original list. */
            return p;
        }
        p = p->next;
    }
    return NULL;
}

/* recompute epoll interest for a given fd based on existing ops */
static void recompute_epoll_interest(loop_t *loop, int fd) {
    uint32_t events = 0;
    loop_op_t *p = loop->ops_head;
    while (p) {
        if (p->fd == fd && !p->cancelled) {
            if (p->type == LOOP_OP_RECV || p->type == LOOP_OP_ACCEPT) events |= EPOLLIN;
            if (p->type == LOOP_OP_SEND || p->type == LOOP_OP_CONNECT) events |= EPOLLOUT;
        }
        p = p->next;
    }

    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = events | EPOLLET; /* use edge-triggered for efficiency */

    if (events == 0) {
        /* remove interest */
        epoll_ctl(loop->epfd, EPOLL_CTL_DEL, fd, NULL);
    } else {
        /* try modify, otherwise add */
        if (epoll_ctl(loop->epfd, EPOLL_CTL_MOD, fd, &ev) != 0) {
            if (errno == ENOENT) {
                epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &ev);
            }
        }
    }
}

/* -------------------- timers & soon tasks -------------------- */

timer_id_t loop_add_timer(uint64_t when_ms, loop_cb_t cb, void *userdata) {
    if (!cb) return 0;
    loop_t *loop = loop_get();
    timer_req_t *t = calloc(1, sizeof(timer_req_t));
    t->cb = cb;
    t->userdata = userdata;
    t->id = (timer_id_t) __atomic_add_fetch(&loop->next_timer_id, 1, __ATOMIC_RELAXED);
    t->expire_ms = when_ms;
    t->cancelled = 0;
    heap_push(loop, t);

    /* wake epoll */
    uint64_t one = 1;
    write(loop->event_fd, &one, sizeof(one));
    return t->id;
}

int loop_cancel_timer(timer_id_t id) {
    loop_t *loop = loop_get();
    if (id == 0) return -1;
    for (int i = 0; i < loop->heap_size; ++i) {
        if (loop->heap[i]->id == id) {
            loop->heap[i]->cancelled = 1;
            return 0;
        }
    }
    return -1;
}

void loop_call_soon(loop_cb_t cb, void *userdata) {
    if (!cb) return;
    loop_t *loop = loop_get();
    soon_task_t *t = malloc(sizeof(soon_task_t));
    t->cb = cb;
    t->userdata = userdata;
    t->next = NULL;
    if (!loop->soon_tail) {
        loop->soon_head = loop->soon_tail = t;
    } else {
        loop->soon_tail->next = t;
        loop->soon_tail = t;
    }
    /* wake epoll */
    uint64_t one = 1;
    write(loop->event_fd, &one, sizeof(one));
}

static void run_soon_tasks(loop_t *loop) {
    while (loop->soon_head) {
        soon_task_t *t = loop->soon_head;
        loop->soon_head = t->next;
        if (!loop->soon_head) loop->soon_tail = NULL;
        loop_cb_t cb = t->cb;
        void *ud = t->userdata;
        free(t);
        if (cb) cb(loop, ud);
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
            loop_cb_t cb = top->cb;
            void *ud = top->userdata;
            free(top);
            if (cb) cb(loop, ud);
            /* continue */
        } else {
            free(top);
        }
    }
}

/* -------------------- core IO ops (recv/send/connect/accept) -------------------- */

int loop_bind_handle(void *handle) {
    if (!handle) return -1;
    loop_t *loop = loop_get();
    int fd = (int)(intptr_t)handle;
    make_nonblocking(fd);
    set_close_on_exec(fd);

    /* Add to epoll with no events (will be modified later) */
    struct epoll_event ev;
    ev.data.fd = fd;
    ev.events = 0;
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        if (errno != EEXIST) return -1;
    }
    return 0;
}

loop_op_id_t loop_post_recv(void *handle, char *buf, unsigned long len, loop_io_cb_t cb, void *userdata) {
    if (!handle || !cb || !buf || len == 0) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    int fd = (int)(intptr_t)handle;

    loop_op_t *op = calloc(1, sizeof(loop_op_t));
    op->fd = fd;
    op->type = LOOP_OP_RECV;
    op->id = alloc_op_id(loop);
    op->cb = cb;
    op->userdata = userdata;
    op->recv.buf = buf;
    op->recv.len = len;
    op->cancelled = 0;

    ops_add(loop, op);

    /* ensure fd is registered for EPOLLIN */
    recompute_epoll_interest(loop, fd);

    /* wake epoll so it can pick up immediate state */
    uint64_t one = 1;
    write(loop->event_fd, &one, sizeof(one));

    return op->id;
}

loop_op_id_t loop_post_send(void *handle, const char *buf, unsigned long len, loop_io_cb_t cb, void *userdata) {
    if (!handle || !cb || !buf || len == 0) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    int fd = (int)(intptr_t)handle;

    loop_op_t *op = calloc(1, sizeof(loop_op_t));
    op->fd = fd;
    op->type = LOOP_OP_SEND;
    op->id = alloc_op_id(loop);
    op->cb = cb;
    op->userdata = userdata;
    op->send.buf = malloc((size_t)len);
    if (!op->send.buf) { free(op); return LOOP_INVALID_OP_ID; }
    memcpy(op->send.buf, buf, (size_t)len);
    op->send.len = len;
    op->send.sent_so_far = 0;
    op->cancelled = 0;

    ops_add(loop, op);

    /* ensure fd is registered for EPOLLOUT */
    recompute_epoll_interest(loop, fd);

    uint64_t one = 1;
    write(loop->event_fd, &one, sizeof(one));

    return op->id;
}

loop_op_id_t loop_connect_async(void *handle, const struct sockaddr *addr, int addrlen, loop_io_cb_t cb, void *userdata) {
    if (!handle || !addr || !cb) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    int fd = (int)(intptr_t)handle;

    make_nonblocking(fd);

    /* initiate non-blocking connect */
    int ret = connect(fd, addr, addrlen);
    if (ret == 0) {
        /* immediate connect success; emulate async completion by scheduling a soon task to call cb */
        loop_op_t *op = calloc(1, sizeof(loop_op_t));
        op->fd = fd;
        op->type = LOOP_OP_CONNECT;
        op->id = alloc_op_id(loop);
        op->cb = cb;
        op->userdata = userdata;
        op->cancelled = 0;
        ops_add(loop, op);

        /* schedule call_soon to invoke completion with success (err=0) */
        loop_call_soon((loop_cb_t) (void*) (intptr_t) (/* wrapper inline */ 0), NULL);
        /* Instead of complex wrapper, we'll directly call callback now (but keep op allocated to match semantics) */
        /* Note: to keep model consistent we'll call cb immediately and free op. */
        int err = 0;
        if (op->cb) op->cb(loop, op->userdata, op->type, err, 0, NULL);
        ops_remove(loop, op);
        free(op);
        return LOOP_INVALID_OP_ID; /* already completed, so returning invalid - keep compatibility with Windows which used ConnectEx to post pending op. */
    } else {
        if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
            return LOOP_INVALID_OP_ID;
        }
    }

    loop_op_t *op = calloc(1, sizeof(loop_op_t));
    op->fd = fd;
    op->type = LOOP_OP_CONNECT;
    op->id = alloc_op_id(loop);
    op->cb = cb;
    op->userdata = userdata;
    op->cancelled = 0;
    ops_add(loop, op);

    /* watch for writability */
    recompute_epoll_interest(loop, fd);

    uint64_t one = 1;
    write(loop->event_fd, &one, sizeof(one));

    return op->id;
}

loop_op_id_t loop_accept_async(void *listen_handle, void **accept_handle_out, loop_io_cb_t cb, void *userdata) {
    if (!listen_handle || !cb) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    int listen_fd = (int)(intptr_t)listen_handle;

    /* create a pre-created socket to be used as "accept handle" (mimic Windows AcceptEx behavior) */
    int acc_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (acc_fd < 0) return LOOP_INVALID_OP_ID;
    set_close_on_exec(acc_fd);

    loop_op_t *op = calloc(1, sizeof(loop_op_t));
    if (!op) { close(acc_fd); return LOOP_INVALID_OP_ID; }

    op->fd = listen_fd;
    op->type = LOOP_OP_ACCEPT;
    op->id = alloc_op_id(loop);
    op->cb = cb;
    op->userdata = userdata;
    op->acc.accept_fd = acc_fd;
    op->acc.got_accepted = 0;
    op->cancelled = 0;

    ops_add(loop, op);

    /* ensure listen fd is watched for EPOLLIN */
    recompute_epoll_interest(loop, listen_fd);

    if (accept_handle_out) *accept_handle_out = (void*)(intptr_t)acc_fd;

    uint64_t one = 1;
    write(loop->event_fd, &one, sizeof(one));

    return op->id;
}

int loop_cancel_op(loop_op_id_t id) {
    if (id == LOOP_INVALID_OP_ID) return -1;
    loop_t *loop = loop_get();
    loop_op_t *op = ops_find_by_id(loop, id);
    if (!op) return -1;
    op->cancelled = 1;

    /* cleanup immediate resources for SEND */
    if (op->type == LOOP_OP_SEND && op->send.buf) {
        free(op->send.buf);
        op->send.buf = NULL;
    }
    /* for ACCEPT, close pre-created accept fd */
    if (op->type == LOOP_OP_ACCEPT && op->acc.accept_fd >= 0) {
        close(op->acc.accept_fd);
        op->acc.accept_fd = -1;
    }

    ops_remove(loop, op);
    free(op);

    /* wake epoll so it notices changed interests */
    uint64_t one = 1;
    write(loop->event_fd, &one, sizeof(one));
    return 0;
}

/* -------------------- completion helpers -------------------- */

static void call_io_cb(loop_t *loop, loop_op_t *op, int err, unsigned long bytes, recv_data_t *rptr) {
    if (!op) return;
    if (op->cb) {
        op->cb(loop, op->userdata, op->type, err, bytes, rptr);
    }
}

/* -------------------- internal event handling -------------------- */

/* When fd is readable, handle ACCEPT and RECV ops.
   When fd is writable, handle SEND and CONNECT ops.
   We iterate through all ops and attempt each relevant op; for RECV and SEND we try to
   complete (may do partial sends). Completed ops are removed and freed. */
static void handle_fd_events(loop_t *loop, int fd, uint32_t events) {
    loop_op_t *p = loop->ops_head;
    /* We'll collect ops to process in an array to avoid modifying list while iterating wrongly.
       But for simplicity we'll iterate and for each matching op attempt the operation; ops_remove/free
       is safe because we always traverse via next saved earlier. */
    while (p) {
        loop_op_t *next = p->next;
        if (p->fd == fd && !p->cancelled) {
            if ((events & EPOLLIN) && (p->type == LOOP_OP_RECV || p->type == LOOP_OP_ACCEPT)) {
                if (p->type == LOOP_OP_RECV) {
                    ssize_t n = recv(fd, p->recv.buf, (size_t)p->recv.len, MSG_DONTWAIT);
                    if (n < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            /* nothing now */
                        } else {
                            /* error */
                            recv_data_t r;
                            r.userdata = p->userdata;
                            r.data = p->recv.buf;
                            r.len = 0;
                            call_io_cb(loop, p, errno, 0, &r);
                            ops_remove(loop, p);
                            free(p);
                        }
                    } else {
                        /* n >= 0, including 0 meaning EOF */
                        recv_data_t r;
                        r.userdata = p->userdata;
                        r.data = p->recv.buf;
                        r.len = (unsigned long)n;
                        call_io_cb(loop, p, 0, (unsigned long)n, &r);
                        ops_remove(loop, p);
                        free(p);
                    }
                } else { /* ACCEPT */
                    /* accept4 returns new fd */
                    struct sockaddr_storage addr;
                    socklen_t alen = sizeof(addr);
                    int afd = accept4(fd, (struct sockaddr*)&addr, &alen, SOCK_NONBLOCK);
                    if (afd < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK) {
                            /* nothing now */
                        } else {
                            /* error: report to callback with err */
                            call_io_cb(loop, p, errno, 0, NULL);
                            /* cleanup pre-created accept fd */
                            if (p->acc.accept_fd >= 0) { close(p->acc.accept_fd); p->acc.accept_fd = -1; }
                            ops_remove(loop, p);
                            free(p);
                        }
                    } else {
                        /* we got afd; now dup2 into pre-created accept_fd so caller sees expected fd */
                        if (p->acc.accept_fd >= 0) {
                            /* dup2(afd, prefd) makes prefd refer to afd's resource */
                            int prefd = p->acc.accept_fd;
                            if (dup2(afd, prefd) < 0) {
                                /* dup2 failed: close both and report error */
                                close(afd);
                                close(prefd);
                                call_io_cb(loop, p, errno, 0, NULL);
                                ops_remove(loop, p);
                                free(p);
                            } else {
                                /* dup2 success: close original afd (now duplicated) */
                                close(afd);
                                /* accepted socket is available at prefd */
                                p->acc.got_accepted = 1;
                                /* call callback: pass accepted socket via userdata? header expects rdata only for recv.
                                   The Windows implementation updated accept context and left accepted socket open and
                                   user had accept_handle_out set earlier to the precreated socket. So we just call cb with bytes==0 */
                                call_io_cb(loop, p, 0, 0, NULL);
                                /* free op but leave accepted socket open for user */
                                ops_remove(loop, p);
                                free(p);
                            }
                        } else {
                            /* no pre-created accept fd (shouldn't happen) */
                            close(afd);
                            call_io_cb(loop, p, EINVAL, 0, NULL);
                            ops_remove(loop, p);
                            free(p);
                        }
                    }
                }
            }
            if ((events & EPOLLOUT) && (p->type == LOOP_OP_SEND || p->type == LOOP_OP_CONNECT)) {
                if (p->type == LOOP_OP_SEND) {
                    /* attempt to send remaining */
                    unsigned long remaining = p->send.len - p->send.sent_so_far;
                    if (remaining > 0) {
                        ssize_t n = send(fd, p->send.buf + p->send.sent_so_far, (size_t)remaining, MSG_DONTWAIT);
                        if (n < 0) {
                            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                                /* try later */
                            } else {
                                /* error */
                                call_io_cb(loop, p, errno, 0, NULL);
                                free(p->send.buf);
                                ops_remove(loop, p);
                                free(p);
                            }
                        } else {
                            p->send.sent_so_far += (unsigned long)n;
                            if (p->send.sent_so_far >= p->send.len) {
                                /* done */
                                call_io_cb(loop, p, 0, p->send.sent_so_far, NULL);
                                free(p->send.buf);
                                ops_remove(loop, p);
                                free(p);
                            }
                        }
                    } else {
                        /* nothing to send (shouldn't happen) */
                        call_io_cb(loop, p, 0, 0, NULL);
                        ops_remove(loop, p);
                        free(p);
                    }
                } else { /* CONNECT */
                    int err = 0;
                    socklen_t elen = sizeof(err);
                    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0) {
                        err = errno;
                    }
                    if (err != 0) {
                        call_io_cb(loop, p, err, 0, NULL);
                        ops_remove(loop, p);
                        free(p);
                    } else {
                        /* success */
                        call_io_cb(loop, p, 0, 0, NULL);
                        ops_remove(loop, p);
                        free(p);
                    }
                }
            }
        }
        p = next;
    }

    /* After handling, recompute interest for this fd (maybe removed ops) */
    recompute_epoll_interest(loop, fd);
}

/* -------------------- main loop run_once & run -------------------- */

static void run_pending_soon_and_timers(loop_t *loop) {
    run_soon_tasks(loop);
    run_timers(loop);
}

static int calc_next_timeout_ms(loop_t *loop) {
    timer_req_t *top = heap_top(loop);
    if (!top) return -1; /* -1 for epoll_wait means infinite */
    uint64_t now = loop_time_ms();
    if (top->expire_ms <= now) return 0;
    uint64_t diff = top->expire_ms - now;
    if (diff > (uint64_t)INT_MAX) return -1;
    return (int)diff;
}

static void loop_run_once() {
    loop_t *loop = loop_get();
    run_pending_soon_and_timers(loop);

    int timeout = calc_next_timeout_ms(loop);

    const int MAX_EVENTS = 64;
    struct epoll_event events[MAX_EVENTS];

    int n = epoll_wait(loop->epfd, events, MAX_EVENTS, timeout);
    /* wake maybe due to event_fd writes or io events or timeout */
    /* First, drain eventfd if present */
    if (n < 0) {
        if (errno == EINTR) return;
        /* other error - treat as no events */
        return;
    }

    for (int i = 0; i < n; ++i) {
        int fd = events[i].data.fd;
        uint32_t ev = events[i].events;

        if (fd == loop->event_fd) {
            /* drain eventfd */
            uint64_t v;
            while (read(loop->event_fd, &v, sizeof(v)) > 0) { /* drain */ }
            /* run soon tasks/timers after this loop iteration below */
            continue;
        }

        handle_fd_events(loop, fd, ev);
    }

    /* After processing events, run soon tasks and timers again */
    run_pending_soon_and_timers(loop);
}

void loop_run(task_t *task) {
    loop_t *loop = loop_get();
    task_run(task);
    while (!loop->stop_flag || loop->heap_size != 0) {
        loop_run_once();
    }
}

/* -------------------- create / destroy / time -------------------- */

loop_t *loop_create_sub() {
    loop_t *loop = calloc(1, sizeof(loop_t));
    if (!loop) return NULL;

    loop->epfd = epoll_create1(EPOLL_CLOEXEC);
    if (loop->epfd < 0) { free(loop); return NULL; }

    loop->event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (loop->event_fd < 0) { close(loop->epfd); free(loop); return NULL; }

    /* add event_fd to epoll */
    struct epoll_event ev;
    ev.data.fd = loop->event_fd;
    ev.events = EPOLLIN | EPOLLET;
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, loop->event_fd, &ev) != 0) {
        close(loop->event_fd);
        close(loop->epfd);
        free(loop);
        return NULL;
    }

    loop->next_op_id = 0;
    loop->next_timer_id = 0;
    loop->heap = NULL;
    loop->heap_size = 0;
    loop->heap_cap = 0;
    loop->soon_head = loop->soon_tail = NULL;
    loop->ops_head = NULL;
    loop->stop_flag = 0;
    return loop;
}

void loop_stop() {
    loop_t *loop = loop_get();
    if (!loop) return;
    loop->stop_flag = 1;
    /* wake epoll */
    uint64_t one = 1;
    write(loop->event_fd, &one, sizeof(one));
}

void loop_destroy() {
    loop_t *loop = loop_get();
    if (!loop) return;

    loop_stop();

    /* cancel and free pending ops */
    loop_op_t *op = loop->ops_head;
    while (op) {
        loop_op_t *n = op->next;
        if (op->type == LOOP_OP_SEND && op->send.buf) free(op->send.buf);
        if (op->type == LOOP_OP_ACCEPT && op->acc.accept_fd >= 0) close(op->acc.accept_fd);
        free(op);
        op = n;
    }
    loop->ops_head = NULL;

    /* free timers */
    for (int i = 0; i < loop->heap_size; ++i) free(loop->heap[i]);
    free(loop->heap);

    /* free soon tasks */
    soon_task_t *s = loop->soon_head;
    while (s) {
        soon_task_t *n = s->next;
        free(s);
        s = n;
    }

    if (loop->event_fd >= 0) close(loop->event_fd);
    if (loop->epfd >= 0) close(loop->epfd);
    free(loop);
    current_loop = NULL;
}

uint64_t loop_time_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}
