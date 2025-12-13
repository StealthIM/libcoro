#include "loop.h"

#include <winsock2.h>
#include <windows.h>
#include <mswsock.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "task.h"

#pragma comment(lib, "ws2_32.lib")

/* thread-local current loop */
__thread loop_t *current_loop = NULL;

/* -------------------- internal data -------------------- */

typedef enum {
    LOOP_OV_POST = 1,
    LOOP_OV_OP   = 2
} loop_ov_kind_t;

typedef struct loop_op_s {
    OVERLAPPED ov;
    loop_ov_kind_t kind; /* must be placed right after OVERLAPPED to read from raw OVERLAPPED* */
    loop_op_type_t type;
    loop_op_id_t id;
    SOCKET sock;

    loop_io_cb_t cb;
    void *userdata;

    union {
        struct { char *buf; unsigned long len; } recv;
        struct {
            char *buf;
            unsigned long len;
            WSABUF wbuf;
        } send;
        struct { char addr_buf[256]; int addr_len; } conn;
        struct { SOCKET accept_sock; } acc;
    };

    struct loop_op_s *next;
} loop_op_t;

typedef struct loop_post_s {
    OVERLAPPED ov;
    loop_ov_kind_t kind; /* placed right after OVERLAPPED */
    loop_cb_t cb;
    void *userdata;
} loop_post_t;

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
    HANDLE iocp;
    volatile LONG next_op_id;
    volatile LONG next_timer_id;
    int stop_flag;

    /* timer heap */
    timer_req_t **heap;
    int heap_size;
    int heap_cap;

    /* microtask queue */
    soon_task_t *soon_head;
    soon_task_t *soon_tail;

    /* active ops linked list (for cancel lookup) */
    struct loop_op_s *ops_head;

    /* extension ptrs */
    LPFN_CONNECTEX lpConnectEx;
    LPFN_ACCEPTEX lpAcceptEx;
};

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
        if (!new_heap) {
            abort();
        }
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
    return (loop_op_id_t)InterlockedIncrement(&loop->next_op_id);
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

/* -------------------- extension loader -------------------- */

static void load_extensions_if_needed(loop_t *loop, SOCKET s) {
    DWORD bytes = 0;
    if (!loop->lpConnectEx) {
        GUID guid = WSAID_CONNECTEX;
        WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &guid, sizeof(guid),
                 &loop->lpConnectEx, sizeof(loop->lpConnectEx),
                 &bytes, NULL, NULL);
    }
    if (!loop->lpAcceptEx) {
        GUID guid = WSAID_ACCEPTEX;
        WSAIoctl(s, SIO_GET_EXTENSION_FUNCTION_POINTER,
                 &guid, sizeof(guid),
                 &loop->lpAcceptEx, sizeof(loop->lpAcceptEx),
                 &bytes, NULL, NULL);
    }
}

/* -------------------- timers & soon tasks -------------------- */

timer_id_t loop_add_timer(uint64_t when_ms, loop_cb_t cb, void *userdata) {
    if (!cb) return 0;
    loop_t *loop = loop_get();
    timer_req_t *t = calloc(1, sizeof(timer_req_t));
    t->cb = cb;
    t->userdata = userdata;
    t->id = (timer_id_t)InterlockedIncrement(&loop->next_timer_id);
    t->expire_ms = when_ms;
    t->cancelled = 0;
    heap_push(loop, t);

    /* wake IOCP so that loop recalculates timeout */
    PostQueuedCompletionStatus(loop->iocp, 0, 0, NULL);
    return t->id;
}

int loop_cancel_timer(timer_id_t id) {
    loop_t *loop = loop_get();
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
    /* wake up IOCP */
    PostQueuedCompletionStatus(loop->iocp, 0, 0, NULL);
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
            /* continue to next timer */
        } else {
            free(top);
        }
    }
}

/* -------------------- core IO ops (post/recv/send/connect/accept) -------------------- */

int loop_bind_handle(void *handle) {
    if (!handle) return -1;
    loop_t *loop = loop_get();
    SOCKET s = (SOCKET)(intptr_t)handle;
    HANDLE h = CreateIoCompletionPort((HANDLE)s, loop->iocp, (ULONG_PTR)s, 0);
    return h ? 0 : -1;
}

loop_op_id_t loop_post_recv(void *handle, char *buf, unsigned long len, loop_io_cb_t cb, void *userdata) {
    if (!handle || !cb || !buf || len == 0) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    SOCKET s = (SOCKET)(intptr_t)handle;

    loop_op_t *op = calloc(1, sizeof(loop_op_t));
    ZeroMemory(&op->ov, sizeof(op->ov));
    op->kind = LOOP_OV_OP;
    op->type = LOOP_OP_RECV;
    op->id = alloc_op_id(loop);
    op->sock = s;
    op->cb = cb;
    op->userdata = userdata;
    op->recv.buf = buf;
    op->recv.len = len;

    ops_add(loop, op);

    WSABUF ws = { len, buf };
    DWORD flags = 0, bytes = 0;
    int ret = WSARecv(s, &ws, 1, &bytes, &flags, &op->ov, NULL);
    if (ret != 0) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            ops_remove(loop, op);
            free(op);
            return LOOP_INVALID_OP_ID;
        }
    }
    return op->id;
}

loop_op_id_t loop_post_send(void *handle, const char *buf, unsigned long len, loop_io_cb_t cb, void *userdata) {
    if (!handle || !cb || !buf || len == 0) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    SOCKET s = (SOCKET)(intptr_t)handle;

    loop_op_t *op = calloc(1, sizeof(loop_op_t));
    ZeroMemory(&op->ov, sizeof(op->ov));
    op->kind = LOOP_OV_OP;
    op->type = LOOP_OP_SEND;
    op->id = alloc_op_id(loop);
    op->sock = s;
    op->cb = cb;
    op->userdata = userdata;

    /* copy buffer to simplify lifetime */
    op->send.buf = malloc((size_t)len);
    if (!op->send.buf) { free(op); return LOOP_INVALID_OP_ID; }
    memcpy(op->send.buf, buf, (size_t)len);
    op->send.len = len;
    op->send.wbuf.buf = op->send.buf;
    op->send.wbuf.len = (ULONG)len;

    ops_add(loop, op);

    DWORD sent = 0;
    int ret = WSASend(s, &op->send.wbuf, 1, &sent, 0, &op->ov, NULL);
    if (ret != 0) {
        int err = WSAGetLastError();
        if (err != WSA_IO_PENDING) {
            ops_remove(loop, op);
            if (op->send.buf) free(op->send.buf);
            free(op);
            return LOOP_INVALID_OP_ID;
        }
    }
    return op->id;
}

loop_op_id_t loop_connect_async(void *handle, const struct sockaddr *addr, int addrlen, loop_io_cb_t cb, void *userdata) {
    if (!handle || !addr || !cb) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    SOCKET s = (SOCKET)(intptr_t)handle;

    /* load ConnectEx pointer */
    load_extensions_if_needed(loop, s);

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    if (bind(s, (struct sockaddr*)&local, sizeof(local)) != 0) {
        return LOOP_INVALID_OP_ID;
    }

    loop_op_t *op = calloc(1, sizeof(loop_op_t));
    ZeroMemory(&op->ov, sizeof(op->ov));
    op->kind = LOOP_OV_OP;
    op->type = LOOP_OP_CONNECT;
    op->id = alloc_op_id(loop);
    op->sock = s;
    op->cb = cb;
    op->userdata = userdata;

    ops_add(loop, op);

    if (loop->lpConnectEx) {
        BOOL ok = loop->lpConnectEx(s, addr, addrlen, NULL, 0, NULL, &op->ov);
        if (ok) {
            PostQueuedCompletionStatus(loop->iocp, 0, (ULONG_PTR)s, &op->ov);
        } else {
            int err = WSAGetLastError();
            if (err != WSA_IO_PENDING) {
                ops_remove(loop, op);
                free(op);
                return LOOP_INVALID_OP_ID;
            }
        }
    }
    else {
        /* fallback: non-blocking connect; user must ensure socket non-blocking */
        int ret = connect(s, addr, addrlen);
        if (ret != 0) {
            int err = WSAGetLastError();
            if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
                ops_remove(loop, op);
                free(op);
                return LOOP_INVALID_OP_ID;
            }
            /* We don't have explicit wait for writability here without extra machinery; prefer ConnectEx. */
        }
    }
    return op->id;
}

loop_op_id_t loop_accept_async(void *listen_handle, void **accept_handle_out, loop_io_cb_t cb, void *userdata) {
    if (!listen_handle || !cb) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    SOCKET listen_sock = (SOCKET)(intptr_t)listen_handle;

    load_extensions_if_needed(loop, listen_sock);
    if (!loop->lpAcceptEx) return LOOP_INVALID_OP_ID;

    SOCKET acc = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (acc == INVALID_SOCKET) return LOOP_INVALID_OP_ID;

    struct sockaddr_in local = {0};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = 0;
    if (bind(acc, (struct sockaddr*)&local, sizeof(local)) != 0) {
        return LOOP_INVALID_OP_ID;
    }

    loop_op_t *op = calloc(1, sizeof(loop_op_t));
    ZeroMemory(&op->ov, sizeof(op->ov));
    op->kind = LOOP_OV_OP;
    op->type = LOOP_OP_ACCEPT;
    op->id = alloc_op_id(loop);
    op->sock = listen_sock;
    op->cb = cb;
    op->userdata = userdata;
    op->acc.accept_sock = acc;

    ops_add(loop, op);

    DWORD bytes = 0;
    BOOL ok = loop->lpAcceptEx(listen_sock, acc, op->conn.addr_buf, 0,
                               sizeof(struct sockaddr_storage), sizeof(struct sockaddr_storage),
                               &bytes, &op->ov);
    int err = WSAGetLastError();
    if (err != WSA_IO_PENDING) {
        ops_remove(loop, op);
        closesocket(acc);
        free(op);
        return LOOP_INVALID_OP_ID;
    }

    if (accept_handle_out) *accept_handle_out = (void*)(intptr_t)acc;
    return op->id;
}

int loop_cancel_op(loop_op_id_t id) {
    if (id == LOOP_INVALID_OP_ID) return -1;
    loop_t *loop = loop_get();
    loop_op_t *op = ops_find_by_id(loop, id);
    if (!op) return -1;
    BOOL ok = CancelIoEx((HANDLE)op->sock, &op->ov);
    PostQueuedCompletionStatus(loop->iocp, 0, 0, NULL);
    return ok ? 0 : -1;
}

/* -------------------- completion handling -------------------- */

static void complete_and_free_op(loop_t *loop, loop_op_t *op, int err, unsigned long bytes) {
    /* remove from list */
    ops_remove(loop, op);

    recv_data_t r;
    recv_data_t *rptr = NULL;
    if (op->type == LOOP_OP_RECV) {
        r.userdata = op->userdata;
        r.data = op->recv.buf;
        r.len = bytes;
        rptr = &r;
    }

    /* ConnectEx success: update connect context */
    if (op->type == LOOP_OP_CONNECT && err == 0) {
        setsockopt(op->sock, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);
    }

    /* AcceptEx success: update accept context for the accepted socket */
    if (op->type == LOOP_OP_ACCEPT && err == 0) {
        setsockopt(op->acc.accept_sock, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&op->sock, sizeof(op->sock));
    }

    if (op->cb) {
        op->cb(loop, op->userdata, op->type, err, bytes, rptr);
    }

    if (op->type == LOOP_OP_SEND && op->send.buf) {
        free(op->send.buf);
    }

    /* Note: for accept, we leave accepted socket open and hand it to user via callback */
    free(op);
}

/* -------------------- main loop run_once & run -------------------- */

static void run_pending_soon_and_timers(loop_t *loop) {
    run_soon_tasks(loop);
    run_timers(loop);
}

static DWORD calc_next_timeout_ms(loop_t *loop) {
    timer_req_t *top = heap_top(loop);
    if (!top) return INFINITE;
    uint64_t now = loop_time_ms();
    if (top->expire_ms <= now) return 0;
    uint64_t diff = top->expire_ms - now;
    if (diff > (uint64_t)INFINITE - 1) return INFINITE;
    return (DWORD)diff;
}

static void loop_run_once() {
    loop_t *loop = loop_get();
    run_pending_soon_and_timers(loop);

    DWORD timeout = calc_next_timeout_ms(loop);

    DWORD bytes = 0;
    ULONG_PTR key = 0;
    OVERLAPPED *ov = NULL;

    BOOL ok = GetQueuedCompletionStatus(loop->iocp, &bytes, &key, &ov, timeout);

    /* After wake, first run timers/soon again in case wake came for them */
    run_pending_soon_and_timers(loop);

    if (!ov) return;

    /* read kind field placed right after OVERLAPPED */
    loop_ov_kind_t kind = *(loop_ov_kind_t *)((char*)ov + sizeof(OVERLAPPED));
    if (kind == LOOP_OV_POST) {
        loop_post_t *p = (loop_post_t*)ov;
        loop_cb_t cb = p->cb;
        void *ud = p->userdata;
        free(p);
        if (cb) cb(loop, ud);
        return;
    }
    if (kind != LOOP_OV_OP) {
        /* unknown overlapped, ignore */
        return;
    }

    loop_op_t *op = (loop_op_t*)ov;
    int err = ok ? 0 : (int)GetLastError();

    /* handle special cases: recv zero => peer closed */
    if (op->type == LOOP_OP_RECV && err == 0 && bytes == 0) {
        /* indicate EOF (caller receives bytes==0) */
        complete_and_free_op(loop, op, 0, 0);
        return;
    }

    complete_and_free_op(loop, op, err, bytes);
}

void loop_run(task_t *task) {
    loop_t *loop = loop_get();
    task_run(task);
    while (!loop->stop_flag && 
           (loop->heap_size != 0 || 
            loop->soon_head != NULL || 
            loop->ops_head != NULL)) {
        loop_run_once();
    }
}

/* -------------------- create / destroy / time -------------------- */

loop_t *loop_create_sub() {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2,2), &wsa) != 0) {
        return NULL;
    }
    loop_t *loop = calloc(1, sizeof(loop_t));
    loop->iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 1);
    loop->next_op_id = 0;
    loop->next_timer_id = 0;
    loop->heap = NULL;
    loop->heap_size = 0;
    loop->heap_cap = 0;
    loop->soon_head = loop->soon_tail = NULL;
    loop->ops_head = NULL;
    loop->lpConnectEx = NULL;
    loop->lpAcceptEx = NULL;
    loop->stop_flag = 0;
    return loop;
}

void loop_stop() {
    loop_t *loop = loop_get();
    loop->stop_flag = 1;
    PostQueuedCompletionStatus(loop->iocp, 0, 0, NULL);
}

void loop_destroy() {
    loop_t *loop = loop_get();
    if (!loop) return;

    loop_stop();

    /* cancel and free pending ops */
    loop_op_t *op = loop->ops_head;
    while (op) {
        loop_op_t *n = op->next;
        CancelIoEx((HANDLE)op->sock, &op->ov);
        if (op->type == LOOP_OP_SEND && op->send.buf) free(op->send.buf);
        if (op->type == LOOP_OP_ACCEPT && op->acc.accept_sock) closesocket(op->acc.accept_sock);
        free(op);
        op = n;
    }

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

    CloseHandle(loop->iocp);
    WSACleanup();
    free(loop);
    current_loop = NULL;
}

uint64_t loop_time_ms() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER li;
    li.LowPart = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    return li.QuadPart / 10000ULL;
}
