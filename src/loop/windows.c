#include "loop.h"

#include <winsock2.h>
#include <windows.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>

#include "future.h"

#pragma comment(lib, "ws2_32.lib")

__thread loop_t *current_loop = NULL;

typedef struct timer_req_s {
    loop_cb_t cb;
    void *userdata;
    timer_id_t id;
    uint64_t expire_ms;
    int cancelled;
} timer_req_t;

typedef struct handle_req_s {
    SOCKET sock;
    struct handle_req_s *next;
    char *buf;
    int buf_len;
} handle_req_t;

struct loop_s {
    HANDLE iocp;
    volatile LONG next_timer_id;
    int stop_flag;

    timer_req_t **heap;   // 最小堆存储定时器
    int heap_size;
    int heap_cap;

    handle_req_t *handles;
};

typedef enum { EV_POST, EV_SOCKET } loop_ev_type_t;

typedef struct loop_event_s {
    OVERLAPPED ov;             // 用于 IOCP
    loop_ev_type_t type;       // 事件类型
    void *userdata;            // 回调要用的 userdata
    union {
        struct {
            loop_cb_t cb;
        } post;
        struct {
            loop_cb_t cb;
            handle_req_t *handle;
        } sock;
    };
} loop_event_t;

// -------------------- 最小堆操作 --------------------

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
        loop->heap = realloc(loop->heap, loop->heap_cap * sizeof(timer_req_t*));
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

// -------------------- 定时器 API --------------------

timer_id_t loop_add_timer(uint64_t when_ms, loop_cb_t cb, void *userdata) {
    timer_req_t *req = calloc(1, sizeof(timer_req_t));
    req->cb = cb;
    req->userdata = userdata;
    loop_t *loop = loop_get();
    req->id = InterlockedIncrement(&loop->next_timer_id);
    req->expire_ms = loop_time_ms() + when_ms;

    heap_push(loop, req);

    // 唤醒 IOCP 线程以重新计算超时时间
    PostQueuedCompletionStatus(loop->iocp, 0, 0, NULL);
    return req->id;
}

int loop_cancel_timer(timer_id_t id) {
    loop_t *loop = loop_get();
    for (int i = 0; i < loop->heap_size; i++) {
        if (loop->heap[i]->id == id) {
            loop->heap[i]->cancelled = 1;
            return 0;
        }
    }
    return -1;
}

// -------------------- posted 事件 --------------------

void loop_call_soon(loop_cb_t cb, void *userdata) {
    loop_event_t *ev = calloc(1, sizeof(loop_event_t));
    ev->type = EV_POST;
    ev->userdata = userdata;
    ev->post.cb = cb;

    loop_t *loop = loop_get();
    PostQueuedCompletionStatus(loop->iocp, 0, 0, &ev->ov);
}

// -------------------- socket HANDLE --------------------

int loop_register_handle(void *handle, loop_cb_t cb, void *userdata) {
    SOCKET s = (SOCKET)handle;

    loop_t *loop = loop_get();

    if (!CreateIoCompletionPort((HANDLE)s, loop->iocp, s, 0))
        return -1;

    handle_req_t *req = calloc(1, sizeof(handle_req_t));
    req->sock = s;
    req->buf_len = 1024;
    req->buf = (char*)malloc(req->buf_len);

    req->next = loop->handles;
    loop->handles = req;

    loop_event_t *ev = calloc(1, sizeof(loop_event_t));
    ev->type = EV_SOCKET;
    ev->userdata = userdata;
    ev->sock.cb = cb;
    ev->sock.handle = req;

    DWORD flags = 0;
    if (
        WSARecv(s,
            &(WSABUF){.buf=req->buf,.len=req->buf_len},
            1,
            NULL,
            &flags,
            &ev->ov,
            NULL
            ) == SOCKET_ERROR
    ) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            free(ev);
            free(req->buf);
            free(req);
            return -1;
        }
    }

    return 0;
}

int loop_unregister_handle(void *handle, bool close_socket) {
    SOCKET s = (SOCKET)handle;
    loop_t *loop = loop_get();
    handle_req_t **p = &loop->handles;
    while (*p && (*p)->sock != s) p = &(*p)->next;
    if (*p) {
        handle_req_t *req = *p;
        *p = req->next;

        CancelIoEx((HANDLE)s, NULL);
        if (close_socket) closesocket(s);
        free(req->buf);
        free(req);
        return 0;
    }
    return -1;
}

// -------------------- loop 主循环 --------------------

void loop_run_once() {
    DWORD bytes;
    ULONG_PTR key;
    OVERLAPPED *ov;

    uint64_t now = loop_time_ms();
    DWORD timeout = INFINITE;
    loop_t *loop = loop_get();

    timer_req_t *top = heap_top(loop);
    if (top) {
        if (top->expire_ms <= now) {
            heap_pop(loop);
            if (!top->cancelled) {
                loop_cb_t cb = top->cb;
                void *ud = top->userdata;
                cb(loop, ud);
                free(top);
                return;
            }
            free(top);
        } else {
            uint64_t diff = top->expire_ms - now;
            timeout = (DWORD)diff;
        }
    }

    BOOL ok = GetQueuedCompletionStatus(loop->iocp, &bytes, &key, &ov, timeout);
    if (!ok && ov == NULL) return;
    if (!ov) return;

    loop_event_t *ev = (loop_event_t*)ov;

    switch (ev->type) {
        case EV_POST:
            ev->post.cb(loop, ev->userdata);
            free(ev);
            break;
        case EV_SOCKET: {
            recv_data_t data = {
                .userdata = ev->userdata,
                .data = ev->sock.handle->buf,
                .len = bytes
            };
            if (bytes == 0) {
                loop_unregister_handle((void*)ev->sock.handle->sock, true);
                break;
            }
            ev->sock.cb(loop, &data);

            DWORD flags = 0;
            DWORD rec_bytes;
            ZeroMemory(ev->sock.handle->buf, ev->sock.handle->buf_len);
            ZeroMemory(&ev->ov, sizeof(OVERLAPPED));
            int ret = WSARecv(ev->sock.handle->sock,
                              &(WSABUF){ .buf = ev->sock.handle->buf, .len = ev->sock.handle->buf_len },
                              1,
                              &rec_bytes,
                              &flags,
                              &ev->ov,
                              NULL);
            if (ret == SOCKET_ERROR && WSAGetLastError() != WSA_IO_PENDING) {
                loop_unregister_handle((void*)ev->sock.handle->sock, false);
                break;
            }
            break;
        }
    }
}

void loop_stop_callback(future_t *_, void *userdata) {
    loop_stop();
}

void loop_run(task_t *task) {
    loop_t *loop = loop_get();
    future_add_done_callback(task->future, loop_stop_callback, loop);
    task_run(task);
    while (!loop->stop_flag || loop->heap_size!=0 || loop->handles) {
        loop_run_once();
    }
}
// -------------------- loop 控制 --------------------

loop_t *loop_create_sub() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    loop_t *loop = calloc(1, sizeof(loop_t));
    loop->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    loop->next_timer_id = 1;
    return loop;
}

void loop_stop() {
    loop_t *loop = loop_get();
    loop->stop_flag = 1;
}

void loop_destroy() {
    loop_stop();

    loop_t *loop = loop_get();

    CloseHandle(loop->iocp);

    for (int i = 0; i < loop->heap_size; i++) {
        free(loop->heap[i]);
    }
    free(loop->heap);

    handle_req_t *h = loop->handles;
    while (h) {
        handle_req_t *next = h->next;
        closesocket(h->sock);
        free(h->buf);
        free(h);
        h = next;
    }
    WSACleanup();
    free(loop);
}

uint64_t loop_time_ms() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER li;
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    return li.QuadPart / 10000ULL;
}
