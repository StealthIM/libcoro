#include "loop.h"

#ifdef LIBCORO_USE_LINUX_EPULL

#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <limits.h>

#include "future.h"
#include "task.h"

// -------------------- 数据结构 --------------------

typedef struct timer_req_s {
    loop_cb_t cb;
    void *userdata;
    timer_id_t id;
    uint64_t expire_ms;
    int cancelled;
} timer_req_t;

typedef struct handle_req_s {
    int fd;
    loop_cb_t cb;           // 用户回调（当收到数据时，回调传入 recv_data_t*）
    void *userdata;         // 用户 userdata，与 Windows 版本语义保持一致
    struct handle_req_s *next;
    char *buf;
    int buf_len;
    int closing;            // 标记正在注销
    struct handle_req_s *next_free; // 延迟释放链表
} handle_req_t;

typedef struct posted_event_s {
    loop_cb_t cb;
    void *userdata;
    struct posted_event_s *next;
} posted_event_t;

struct loop_s {
    int epfd;               // epoll fd
    int wakefd;             // eventfd 用来唤醒 / posted
    volatile long next_timer_id;
    int stop_flag;

    timer_req_t **heap;
    int heap_size;
    int heap_cap;

    handle_req_t *handles;      // 链表（active handles）
    handle_req_t *to_free;      // 延迟释放链表（仅在 loop 线程释放）

    posted_event_t *posted_head;
    posted_event_t *posted_tail;
};

__thread loop_t *current_loop = NULL;

// -------------------- 时间辅助 --------------------

static uint64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000ULL);
}

uint64_t loop_time_ms() {
    return now_ms();
}

// -------------------- 最小堆操作（单线程，无锁） --------------------

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
        int newcap = loop->heap_cap ? loop->heap_cap * 2 : 16;
        timer_req_t **tmp = realloc(loop->heap, (size_t)newcap * sizeof(timer_req_t *));
        if (!tmp) return; // OOM: 忍痛不插入
        loop->heap = tmp;
        loop->heap_cap = newcap;
    }
    loop->heap[loop->heap_size] = t;
    heap_up(loop->heap, loop->heap_size);
    loop->heap_size++;
}

static timer_req_t *heap_top(loop_t *loop) {
    return loop->heap_size ? loop->heap[0] : NULL;
}

static timer_req_t *heap_pop(loop_t *loop) {
    if (!loop->heap_size) return NULL;
    timer_req_t *ret = loop->heap[0];
    loop->heap_size--;
    if (loop->heap_size > 0) {
        loop->heap[0] = loop->heap[loop->heap_size];
        heap_down(loop->heap, loop->heap_size, 0);
    }
    return ret;
}

// -------------------- posted 事件队列（单线程，无锁） --------------------

void loop_call_soon(loop_cb_t cb, void *userdata) {
    if (!cb) return;
    loop_t *loop = loop_get();
    if (!loop) return;
    posted_event_t *p = calloc(1, sizeof(posted_event_t));
    if (!p) return;
    p->cb = cb;
    p->userdata = userdata;
    p->next = NULL;

    if (loop->posted_tail) {
        loop->posted_tail->next = p;
        loop->posted_tail = p;
    } else {
        loop->posted_head = loop->posted_tail = p;
    }

    // 唤醒 epoll（写 eventfd）
    uint64_t one = 1;
    ssize_t r = write(loop->wakefd, &one, sizeof(one));
    (void)r;
}

// -------------------- handle (socket) 的注册/注销（单线程） --------------------

// helper: set non-blocking
static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return -1;
    return 0;
}

int loop_register_handle(void *handle, loop_cb_t cb, void *userdata) {
    if (!handle || !cb) return -1;
    loop_t *loop = loop_get();
    if (!loop) return -1;
    int fd = (int)(intptr_t)handle;

    if (set_nonblocking(fd) != 0) {
        return -1;
    }

    handle_req_t *req = calloc(1, sizeof(handle_req_t));
    if (!req) return -1;
    req->fd = fd;
    req->cb = cb;
    req->userdata = userdata;
    req->buf_len = 4096;
    req->buf = malloc((size_t)req->buf_len);
    if (!req->buf) { free(req); return -1; }
    req->closing = 0;
    req->next = NULL;
    req->next_free = NULL;

    // insert to handles list (single-threaded)
    req->next = loop->handles;
    loop->handles = req;

    // register to epoll
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.data.ptr = req; // store pointer
    ev.events = EPOLLIN | EPOLLET | EPOLLERR | EPOLLHUP;
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
        // rollback
        handle_req_t **pp = &loop->handles;
        while (*pp && *pp != req) pp = &(*pp)->next;
        if (*pp) *pp = req->next;

        free(req->buf);
        free(req);
        return -1;
    }

    return 0;
}

int loop_unregister_handle(void *handle, bool close_socket) {
    if (!handle) return -1;
    loop_t *loop = loop_get();
    if (!loop) return -1;
    int fd = (int)(intptr_t)handle;

    handle_req_t **pp = &loop->handles;
    handle_req_t *found = NULL;
    while (*pp) {
        if ((*pp)->fd == fd) {
            found = *pp;
            *pp = (*pp)->next;
            break;
        }
        pp = &(*pp)->next;
    }
    if (!found) {
        return -1;
    }

    // mark closing and add to to_free list (main loop will free)
    found->closing = 1;
    found->next_free = loop->to_free;
    loop->to_free = found;

    // remove from epoll and optionally close fd
    epoll_ctl(loop->epfd, EPOLL_CTL_DEL, fd, NULL);
    if (close_socket) close(fd);

    // wake up loop to process free/reclaim quickly
    uint64_t one = 1;
    write(loop->wakefd, &one, sizeof(one));

    return 0;
}

// -------------------- 定时器 API（单线程） --------------------

timer_id_t loop_add_timer(uint64_t when_ms, loop_cb_t cb, void *userdata) {
    if (!cb) return -1;
    loop_t *loop = loop_get();
    if (!loop) return -1;
    timer_req_t *req = calloc(1, sizeof(timer_req_t));
    if (!req) return -1;
    req->cb = cb;
    req->userdata = userdata;
    req->id = (timer_id_t)++loop->next_timer_id;
    req->expire_ms = now_ms() + when_ms;
    req->cancelled = 0;

    heap_push(loop, req);

    // 唤醒 epoll_wait 重新计算超时
    uint64_t one = 1;
    write(loop->wakefd, &one, sizeof(one));
    return req->id;
}

int loop_cancel_timer(timer_id_t id) {
    loop_t *loop = loop_get();
    if (!loop) return -1;
    for (int i = 0; i < loop->heap_size; i++) {
        if (loop->heap[i]->id == id) {
            loop->heap[i]->cancelled = 1;
            // wake up to let loop re-evaluate
            uint64_t one = 1;
            write(loop->wakefd, &one, sizeof(one));
            return 0;
        }
    }
    return -1;
}

// -------------------- 主循环内部辅助（单线程） --------------------

static void process_posted(loop_t *loop) {
    posted_event_t *p = loop->posted_head;
    loop->posted_head = loop->posted_tail = NULL;

    while (p) {
        posted_event_t *next = p->next;
        if (p->cb) p->cb(loop, p->userdata);
        free(p);
        p = next;
    }
}

static void free_pending_handles(loop_t *loop) {
    handle_req_t *p = loop->to_free;
    loop->to_free = NULL;

    while (p) {
        handle_req_t *next = p->next_free;
        free(p->buf);
        free(p);
        p = next;
    }
}

static void handle_epoll_handle_event(loop_t *loop, handle_req_t *req) {
    if (!req) return;
    if (req->closing) return;

    for (;;) {
        ssize_t r = recv(req->fd, req->buf, (size_t)req->buf_len, 0);
        if (r > 0) {
            recv_data_t data;
            data.userdata = req->userdata;
            data.data = req->buf;
            data.len = (unsigned long)r;
            if (req->cb) req->cb(loop, &data);
            continue;
        } else if (r == 0) {
            // peer closed
            loop_unregister_handle((void *)(intptr_t)req->fd, false);
            break;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break; // no more data
            } else {
                // error
                loop_unregister_handle((void *)(intptr_t)req->fd, true);
                break;
            }
        }
    }
}

// -------------------- loop 主循环（单线程） --------------------

static void loop_run_main(loop_t *loop) {
    const int MAX_EVENTS = 32;
    struct epoll_event events[MAX_EVENTS];

    while (!loop->stop_flag || loop->heap_size!=0 || loop->posted_head) {
        // 计算下一个定时器到期时间
        int timeout_int = -1; // -1 -> infinite
        timer_req_t *top = heap_top(loop);
        if (top) {
            uint64_t now = now_ms();
            if (top->expire_ms <= now) {
                // 触发它（pop）
                timer_req_t *t = heap_pop(loop);
                if (!t->cancelled) {
                    loop_cb_t cb = t->cb;
                    void *ud = t->userdata;
                    // 调用回调（单线程，因此无需解锁）
                    if (cb) cb(loop, ud);
                }
                free(t);
                // 立刻回到循环，重新计算下一个定时器
                continue;
            } else {
                uint64_t diff = top->expire_ms - now;
                if (diff > (uint64_t)INT_MAX) timeout_int = INT_MAX;
                else timeout_int = (int)diff;
            }
        }

        int n = epoll_wait(loop->epfd, events, MAX_EVENTS, timeout_int);
        if (n < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        } else if (n == 0) {
            // timeout -> 下次循环会处理超时定时器
            continue;
        }

        for (int i = 0; i < n; i++) {
            void *ptr = events[i].data.ptr;
            if (!ptr) continue;

            if (ptr == (void *)loop) {
                // wakefd 事件：清空 eventfd
                uint64_t cnt;
                while (read(loop->wakefd, &cnt, sizeof(cnt)) > 0) { }
                // 处理 posted 事件
                process_posted(loop);
                // 回收待释放的 handles
                free_pending_handles(loop);
            } else {
                handle_req_t *req = (handle_req_t *)ptr;
                handle_epoll_handle_event(loop, req);
                free_pending_handles(loop);
            }
        }
    }

    // loop 停止：释放 pending 资源
    free_pending_handles(loop);
}

// -------------------- loop 控制 --------------------

loop_t *loop_create_sub() {
    loop_t *loop = calloc(1, sizeof(loop_t));
    if (!loop) return NULL;

    loop->epfd = epoll_create1(0);
    if (loop->epfd < 0) { free(loop); return NULL; }

    loop->wakefd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (loop->wakefd < 0) { close(loop->epfd); free(loop); return NULL; }

    loop->next_timer_id = 1;
    loop->stop_flag = 0;
    loop->heap = NULL;
    loop->heap_size = loop->heap_cap = 0;
    loop->handles = NULL;
    loop->to_free = NULL;
    loop->posted_head = loop->posted_tail = NULL;

    // 将 wakefd 注册到 epoll。用 loop 指针作为 data.ptr 以便区分
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.ptr = (void *)loop;
    if (epoll_ctl(loop->epfd, EPOLL_CTL_ADD, loop->wakefd, &ev) != 0) {
        close(loop->wakefd);
        close(loop->epfd);
        free(loop);
        return NULL;
    }

    return loop;
}

void loop_stop_callback(future_t *_, void *userdata) {
    loop_stop();
}

void loop_run(task_t *task) {
    loop_t *loop = loop_get();
    if (!loop) return;

    // 将 task 的完成与 stop 关联（与 Windows 实现保持一致）
    if (task && task->future) {
        future_add_done_callback(task->future, loop_stop_callback, loop);
    }

    // 启动任务（同步）
    if (task) task_run(task);

    // 进入主循环（单线程）
    loop_run_main(loop);
}

void loop_stop() {
    loop_t *loop = loop_get();
    if (!loop) return;
    loop->stop_flag = 1;
    // 唤醒 epoll_wait
    uint64_t one = 1;
    write(loop->wakefd, &one, sizeof(one));
}

void loop_destroy() {
    loop_t *loop = loop_get();
    if (!loop) return;

    // 确保停止
    loop_stop();

    // 从 epoll 删除 wakefd，然后关闭
    epoll_ctl(loop->epfd, EPOLL_CTL_DEL, loop->wakefd, NULL);
    close(loop->wakefd);
    close(loop->epfd);

    // 释放堆上的定时器
    for (int i = 0; i < loop->heap_size; i++) {
        free(loop->heap[i]);
    }
    free(loop->heap);
    loop->heap = NULL;
    loop->heap_size = loop->heap_cap = 0;

    // 清空 active handles（与 Windows 保持一致：关闭 fd 并释放）
    handle_req_t *h = loop->handles;
    while (h) {
        handle_req_t *next = h->next;
        epoll_ctl(loop->epfd, EPOLL_CTL_DEL, h->fd, NULL);
        close(h->fd);
        free(h->buf);
        free(h);
        h = next;
    }
    loop->handles = NULL;

    // 清空 to_free
    handle_req_t *f = loop->to_free;
    while (f) {
        handle_req_t *next = f->next_free;
        free(f->buf);
        free(f);
        f = next;
    }
    loop->to_free = NULL;

    // 清空 posted
    posted_event_t *p = loop->posted_head;
    while (p) {
        posted_event_t *next = p->next;
        free(p);
        p = next;
    }
    loop->posted_head = loop->posted_tail = NULL;

    free(loop);
}

#endif
