/*
 * libcoro lwIP 后端 —— 阶段 2b: 裸机 (NO_SYS=1, raw/callback API)。
 *
 * 和 lwip_select.c (socket 模式) 的根本差异: raw API 是"推模型"——没有 fd,
 * 没有 select, 数据由 lwIP 的 tcp_recv/tcp_sent/tcp_connected/accept 回调
 * 不请自来。本后端把这套回调映射回 loop.h 的一次性 op 完成契约:
 *
 *   handle       = raw_conn_t* (裹 tcp_pcb + 每连接 rx pbuf 队列 + 待决 op)
 *   loop_run     = 手动 poll: sys_check_timeouts() + netif_poll_all() 泵栈,
 *                  直到没有待决 op / timer / soon 任务。
 *   recv 完成    = tcp_recv 回调缓冲 pbuf, 满足待决 recv op (契约走 bytes)。
 *   accept 完成  = accept 回调包新 pcb 成 raw_conn_t, 经 *accept_handle_out
 *                  交付 (契约: bytes=0)。
 *   connect 完成 = tcp_connected 回调。
 *   send 完成    = tcp_write(COPY)+tcp_output 后经 call_soon 异步完成。
 *
 * 裸机只有单一执行上下文: 没有真线程 → offload 编译关闭 (loop_get_offload
 * 返 NULL); DNS 走 lwIP 原生异步 dns_gethostbyname (不在此文件)。
 * 不提供 sync pal_socket (裸机决策)。
 */

#include <libcoro/loop.h>
#include <libcoro/task.h>
#include <libcoro/raw_setup.h>

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "lwip/init.h"
#include "lwip/tcp.h"
#include "lwip/timeouts.h"
#include "lwip/netif.h"
#include "lwip/ip_addr.h"
#include "lwip/pbuf.h"
#include "lwip/sys.h"

__thread loop_t *current_loop = NULL;

/* -------------------- 每连接包装 -------------------- */

typedef struct raw_conn_s {
    struct tcp_pcb *pcb;
    int is_listener;

    /* rx: tcp_recv 回调缓冲到这里 (pbuf_cat 成一条链), recv op 从中满足。
     * rx_off = 队首链已消费的字节数 (避免拆链, 用偏移 + pbuf_copy_partial)。 */
    struct pbuf *rx_head;
    u16_t rx_off;
    int rx_closed;             /* 收到远端 FIN */
    int has_err;               /* tcp_err 回调过: pcb 已被 lwIP 释放 */
    int err_code;              /* 映射后的 errno */

    /* 待决 recv op (future_socket 每 socket 同一时刻至多一个) */
    int          r_pending;
    char        *r_buf;
    unsigned long r_len;
    loop_io_cb_t r_cb;
    void        *r_ud;

    /* 待决 connect op */
    int          c_pending;
    loop_io_cb_t c_cb;
    void        *c_ud;

    /* listener: accept backlog + 待决 accept op */
    struct raw_conn_s **acc_q;
    int          acc_n, acc_cap;
    int          a_pending;
    void       **a_out;
    loop_io_cb_t a_cb;
    void        *a_ud;
} raw_conn_t;

/* -------------------- timer / soon / loop 结构 -------------------- */

typedef struct timer_req_s {
    loop_cb_t cb; void *userdata;
    timer_id_t id; uint64_t expire_ms; int cancelled;
} timer_req_t;

typedef struct soon_task_s {
    loop_cb_t cb; void *userdata; struct soon_task_s *next;
} soon_task_t;

struct loop_s {
    int stop_flag;
    volatile int next_timer_id;

    timer_req_t **heap; int heap_size, heap_cap;
    soon_task_t *soon_head, *soon_tail;

    int n_pending;   /* 待决 IO op 数 (recv/connect/accept/send-inflight) */
};

/* 前向声明 */
static void try_complete_recv(loop_t *loop, raw_conn_t *c);
static int sockaddr_to_ip(const struct sockaddr *addr, ip_addr_t *ip, u16_t *port);

/* -------------------- lwIP 协议栈初始化 (一次) -------------------- */

static int g_lwip_inited = 0;

static void ensure_lwip_init(void) {
    if (g_lwip_inited) return;
    g_lwip_inited = 1;
    lwip_init();   /* NO_SYS: 同步初始化, 无 tcpip_thread; loopif 自动挂 */
}

uint64_t loop_time_ms(void) { return (uint64_t)sys_now(); }

/* -------------------- heap (timer) -------------------- */

static void heap_swap(timer_req_t **a, timer_req_t **b) { timer_req_t *t=*a; *a=*b; *b=t; }
static void heap_up(timer_req_t **h, int i) {
    while (i>0){int p=(i-1)/2; if(h[p]->expire_ms<=h[i]->expire_ms)break; heap_swap(&h[p],&h[i]); i=p;}
}
static void heap_down(timer_req_t **h, int n, int i) {
    while(1){int l=i*2+1,r=i*2+2,s=i;
        if(l<n&&h[l]->expire_ms<h[s]->expire_ms)s=l;
        if(r<n&&h[r]->expire_ms<h[s]->expire_ms)s=r;
        if(s==i)break; heap_swap(&h[i],&h[s]); i=s;}
}
static void heap_push(loop_t *loop, timer_req_t *t) {
    if(loop->heap_size==loop->heap_cap){
        loop->heap_cap = loop->heap_cap?loop->heap_cap*2:16;
        loop->heap = realloc(loop->heap, loop->heap_cap*sizeof(timer_req_t*));
        if(!loop->heap) abort();
    }
    loop->heap[loop->heap_size]=t; heap_up(loop->heap, loop->heap_size); loop->heap_size++;
}
static timer_req_t* heap_top(loop_t *loop){ return loop->heap_size?loop->heap[0]:NULL; }
static timer_req_t* heap_pop(loop_t *loop){
    if(!loop->heap_size)return NULL;
    timer_req_t *ret=loop->heap[0]; loop->heap_size--;
    if(loop->heap_size>0){ loop->heap[0]=loop->heap[loop->heap_size]; heap_down(loop->heap,loop->heap_size,0);}
    return ret;
}

/* -------------------- timers / soon -------------------- */

timer_id_t loop_add_timer(uint64_t when_ms, loop_cb_t cb, void *userdata) {
    if (!cb) return 0;
    loop_t *loop = loop_get();
    timer_req_t *t = calloc(1, sizeof(*t));
    t->cb=cb; t->userdata=userdata; t->id=(timer_id_t)++loop->next_timer_id;
    t->expire_ms=when_ms; t->cancelled=0;
    heap_push(loop, t);
    return t->id;
}

int loop_cancel_timer(timer_id_t id) {
    loop_t *loop = loop_get();
    for (int i=0;i<loop->heap_size;++i)
        if (loop->heap[i]->id==id){ loop->heap[i]->cancelled=1; return 0; }
    return -1;
}

void loop_call_soon(loop_cb_t cb, void *userdata) {
    if (!cb) return;
    loop_t *loop = loop_get();
    soon_task_t *t = malloc(sizeof(*t));
    t->cb=cb; t->userdata=userdata; t->next=NULL;
    if(!loop->soon_tail) loop->soon_head=loop->soon_tail=t;
    else { loop->soon_tail->next=t; loop->soon_tail=t; }
}

static void run_soon_tasks(loop_t *loop) {
    while (loop->soon_head) {
        soon_task_t *t=loop->soon_head; loop->soon_head=t->next;
        if(!loop->soon_head) loop->soon_tail=NULL;
        loop_cb_t cb=t->cb; void *ud=t->userdata; free(t); if(cb)cb(loop,ud);
    }
}

static void run_timers(loop_t *loop) {
    uint64_t now = loop_time_ms();
    while (1) {
        timer_req_t *top = heap_top(loop);
        if(!top || top->expire_ms>now) break;
        heap_pop(loop);
        if(!top->cancelled){ loop_cb_t cb=top->cb; void*ud=top->userdata; free(top); if(cb)cb(loop,ud);}
        else free(top);
    }
}

/* -------------------- 错误码映射 -------------------- */

static int err_to_errno(err_t e) {
    switch (e) {
        case ERR_OK:    return 0;
        case ERR_MEM:   return ENOMEM;
        case ERR_TIMEOUT: return ETIMEDOUT;
        case ERR_RST:   return ECONNRESET;
        case ERR_CLSD:  return ENOTCONN;
        case ERR_CONN:  return ENOTCONN;
        case ERR_USE:   return EADDRINUSE;
        case ERR_RTE:   return ENETUNREACH;
        case ERR_ABRT:  return ECONNABORTED;
        default:        return EIO;
    }
}

/* -------------------- recv 完成: 从 rx pbuf 队列满足待决 recv -------------------- */

/* 把 rx_head 里的字节拷进待决 recv 的 buf, 完成 op。契约: recv 走 bytes,
 * bytes=0 表示对端关闭 (EOF)。 */
static void try_complete_recv(loop_t *loop, raw_conn_t *c) {
    if (!c->r_pending) return;

    if (c->has_err) {
        c->r_pending = 0; loop->n_pending--;
        loop_io_cb_t cb=c->r_cb; void *ud=c->r_ud;
        if (cb) cb(loop, ud, LOOP_OP_RECV, c->err_code, 0, NULL);
        return;
    }

    if (c->rx_head) {
        /* rx_head 是一条 pbuf_cat 链; rx_off 是已消费的前缀。用 copy_partial
         * 拷出最多 r_len 字节, 再按消费量前进 rx_off / dequeue 用完的 pbuf。
         * 不拆链 (拆 pbuf_cat 链会破坏 tot_len/ref 计数)。 */
        u16_t avail = c->rx_head->tot_len - c->rx_off;
        unsigned long want = c->r_len;
        u16_t take = (avail <= want) ? avail : (u16_t)want;

        u16_t got = pbuf_copy_partial(c->rx_head, c->r_buf, take, c->rx_off);
        c->rx_off += got;

        /* 从链头 dequeue 已完全消费的 pbuf */
        while (c->rx_head && c->rx_off >= c->rx_head->len) {
            c->rx_off -= c->rx_head->len;
            struct pbuf *next = c->rx_head->next;
            if (next) pbuf_ref(next);        /* 保住后继, 再 free 队首 */
            pbuf_free(c->rx_head);           /* 只释放这一个 pbuf (ref--) */
            c->rx_head = next;
        }

        if (c->pcb) tcp_recved(c->pcb, got);

        c->r_pending = 0; loop->n_pending--;
        loop_io_cb_t cb=c->r_cb; void *ud=c->r_ud;
        if (cb) cb(loop, ud, LOOP_OP_RECV, 0, got, NULL);
        return;
    }

    if (c->rx_closed) {
        /* 无缓冲数据 + 已收 FIN: EOF */
        c->r_pending = 0; loop->n_pending--;
        loop_io_cb_t cb=c->r_cb; void *ud=c->r_ud;
        if (cb) cb(loop, ud, LOOP_OP_RECV, 0, 0, NULL);
    }
    /* 否则: 数据还没到, 留着 op, 等下个 tcp_recv 回调 */
}

/* -------------------- lwIP raw 回调 -------------------- */

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    raw_conn_t *c = (raw_conn_t*)arg;
    if (!c) { if(p) pbuf_free(p); return ERR_OK; }

    if (err != ERR_OK) {
        if (p) pbuf_free(p);
        c->has_err = 1; c->err_code = err_to_errno(err);
        try_complete_recv(current_loop, c);
        return ERR_OK;
    }
    if (p == NULL) {
        /* 对端关闭 */
        c->rx_closed = 1;
        try_complete_recv(current_loop, c);
        return ERR_OK;
    }

    /* 追加到 rx 队列尾 */
    if (!c->rx_head) c->rx_head = p;
    else pbuf_cat(c->rx_head, p);

    try_complete_recv(current_loop, c);
    return ERR_OK;
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t len) {
    (void)arg; (void)pcb; (void)len;
    return ERR_OK;   /* send 完成走 call_soon (tcp_write COPY 已入队即算完成) */
}

static void on_err(void *arg, err_t err) {
    raw_conn_t *c = (raw_conn_t*)arg;
    if (!c) return;
    /* lwIP 已释放 pcb; 不能再碰它 */
    c->pcb = NULL;
    c->has_err = 1; c->err_code = err_to_errno(err);

    if (c->r_pending) try_complete_recv(current_loop, c);
    if (c->c_pending) {
        c->c_pending = 0; current_loop->n_pending--;
        loop_io_cb_t cb=c->c_cb; void *ud=c->c_ud;
        if (cb) cb(current_loop, ud, LOOP_OP_CONNECT, c->err_code, 0, NULL);
    }
}

static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)pcb;
    raw_conn_t *c = (raw_conn_t*)arg;
    if (!c || !c->c_pending) return ERR_OK;
    c->c_pending = 0; current_loop->n_pending--;
    loop_io_cb_t cb=c->c_cb; void *ud=c->c_ud;
    if (cb) cb(current_loop, ud, LOOP_OP_CONNECT, err_to_errno(err), 0, NULL);
    return ERR_OK;
}

/* accept 完成经 *accept_handle_out 交付 (bytes=0)。若无待决 accept op, 新连接
 * 入 backlog 队列, 等 loop_accept_async 来取。 */
static void deliver_accept(loop_t *loop, raw_conn_t *lc, raw_conn_t *nc) {
    if (lc->a_out) *lc->a_out = (void*)nc;
    lc->a_pending = 0; loop->n_pending--;
    loop_io_cb_t cb=lc->a_cb; void *ud=lc->a_ud;
    if (cb) cb(loop, ud, LOOP_OP_ACCEPT, 0, 0, NULL);
}

static err_t on_accept(void *arg, struct tcp_pcb *newpcb, err_t err) {
    raw_conn_t *lc = (raw_conn_t*)arg;
    if (!lc || err != ERR_OK || !newpcb) return ERR_VAL;

    raw_conn_t *nc = calloc(1, sizeof(*nc));
    if (!nc) return ERR_MEM;
    nc->pcb = newpcb;
    tcp_arg(newpcb, nc);
    tcp_recv(newpcb, on_recv);
    tcp_sent(newpcb, on_sent);
    tcp_err(newpcb, on_err);

    if (lc->a_pending) {
        deliver_accept(current_loop, lc, nc);
    } else {
        /* 入 backlog */
        if (lc->acc_n == lc->acc_cap) {
            lc->acc_cap = lc->acc_cap ? lc->acc_cap*2 : 4;
            lc->acc_q = realloc(lc->acc_q, lc->acc_cap*sizeof(raw_conn_t*));
            if (!lc->acc_q) abort();
        }
        lc->acc_q[lc->acc_n++] = nc;
    }
    return ERR_OK;
}

/* -------------------- loop.h IO ops -------------------- */
/* handle 是 raw_conn_t*。裸机没有 fd, 也没有 op-id 取消粒度: 每 socket 同一
 * 时刻至多一个 recv / connect 待决 (future_socket 的用法保证)。op-id 只用来
 * 表示"成功投递", 返回连接指针的低位无意义, 用固定非 INVALID 值即可。 */

#define OK_OP_ID ((loop_op_id_t)1)

int loop_bind_handle(void *handle) {
    (void)handle;   /* raw 模式无需绑 fd 到 selector */
    return 0;
}

loop_op_id_t loop_post_recv(void *handle, char *buf, unsigned long len, loop_io_cb_t cb, void *userdata) {
    if (!handle || !cb || !buf || len==0) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    raw_conn_t *c = (raw_conn_t*)handle;
    c->r_pending=1; c->r_buf=buf; c->r_len=len; c->r_cb=cb; c->r_ud=userdata;
    loop->n_pending++;
    /* 数据可能已在队列里 (上一次没读完 / FIN 已到): 立即尝试 */
    try_complete_recv(loop, c);
    return OK_OP_ID;
}

typedef struct { raw_conn_t *c; int err; unsigned long bytes; loop_io_cb_t cb; void *ud; } send_done_t;

static void send_done_cb(loop_t *loop, void *ud) {
    send_done_t *d = (send_done_t*)ud;
    loop->n_pending--;
    if (d->cb) d->cb(loop, d->ud, LOOP_OP_SEND, d->err, d->bytes, NULL);
    free(d);
}

loop_op_id_t loop_post_send(void *handle, const char *buf, unsigned long len, loop_io_cb_t cb, void *userdata) {
    if (!handle || !cb || !buf || len==0) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    raw_conn_t *c = (raw_conn_t*)handle;
    if (c->has_err || !c->pcb) return LOOP_INVALID_OP_ID;

    /* raw tcp_write 一次最多 tcp_sndbuf() 字节; 简化: 只写能放下的部分,
     * 返回实际字节 (future_socket / stream 上层按短写重试, 语义同 BSD send)。 */
    u16_t space = tcp_sndbuf(c->pcb);
    unsigned long n = (len <= space) ? len : space;
    if (n == 0) return LOOP_INVALID_OP_ID;   /* 发送缓冲满: 上层稍后重试 */

    err_t e = tcp_write(c->pcb, buf, (u16_t)n, TCP_WRITE_FLAG_COPY);
    if (e != ERR_OK) return LOOP_INVALID_OP_ID;
    tcp_output(c->pcb);

    send_done_t *d = malloc(sizeof(*d));
    if (!d) return LOOP_INVALID_OP_ID;
    d->c=c; d->err=0; d->bytes=n; d->cb=cb; d->ud=userdata;
    loop->n_pending++;
    loop_call_soon(send_done_cb, d);
    return OK_OP_ID;
}

loop_op_id_t loop_connect_async(void *handle, const struct sockaddr *addr, int addrlen, loop_io_cb_t cb, void *userdata) {
    if (!handle || !addr || !cb) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    raw_conn_t *c = (raw_conn_t*)handle;
    if (!c->pcb) return LOOP_INVALID_OP_ID;

    (void)addrlen;
    ip_addr_t ip;
    u16_t port;
    if (sockaddr_to_ip(addr, &ip, &port) != 0) return LOOP_INVALID_OP_ID;

    c->c_pending=1; c->c_cb=cb; c->c_ud=userdata;
    loop->n_pending++;
    err_t e = tcp_connect(c->pcb, &ip, port, on_connected);
    if (e != ERR_OK) {
        c->c_pending=0; loop->n_pending--;
        return LOOP_INVALID_OP_ID;
    }
    return OK_OP_ID;
}

loop_op_id_t loop_accept_async(void *listen_handle, void **accept_handle_out, loop_io_cb_t cb, void *userdata) {
    if (!listen_handle || !cb) return LOOP_INVALID_OP_ID;
    loop_t *loop = loop_get();
    raw_conn_t *lc = (raw_conn_t*)listen_handle;

    lc->a_pending=1; lc->a_out=accept_handle_out; lc->a_cb=cb; lc->a_ud=userdata;
    loop->n_pending++;
    /* backlog 里已有连接: 立即交付 */
    if (lc->acc_n > 0) {
        raw_conn_t *nc = lc->acc_q[0];
        memmove(&lc->acc_q[0], &lc->acc_q[1], (lc->acc_n-1)*sizeof(raw_conn_t*));
        lc->acc_n--;
        deliver_accept(loop, lc, nc);
    }
    return OK_OP_ID;
}

int loop_cancel_op(loop_op_id_t id) {
    (void)id;
    return -1;   /* 裸机 echo 路径不用取消; 未实现 */
}

/* -------------------- wake / offload (裸机: 无线程) -------------------- */

void loop_wake(loop_t *loop) { (void)loop; }   /* 单上下文, 无需跨线程唤醒 */
struct offload_pool_s *loop_get_offload(loop_t *loop) { (void)loop; return NULL; }

/* -------------------- 主循环 -------------------- */

static int has_work(loop_t *loop) {
    return loop->heap_size != 0 || loop->soon_head != NULL || loop->n_pending != 0;
}

/* 泵一轮 lwIP: 处理定时器 + 排空 loopback netif 排队的 pbuf (单线程模式下
 * loopback 输出不会自动 input, 要手动 poll)。 */
static void pump_lwip(void) {
    sys_check_timeouts();
#if !LWIP_NETIF_LOOPBACK_MULTITHREADING
    netif_poll_all();
#endif
}

void loop_run(task_t *task) {
    loop_t *loop = loop_get();
    if (task) task_run(task);

    while (!loop->stop_flag && has_work(loop)) {
        run_soon_tasks(loop);
        run_timers(loop);
        pump_lwip();
        /* soon 任务 (含 send 完成 / task resume) 可能在 pump 后产生 */
        run_soon_tasks(loop);
    }
}

void loop_stop(void) { loop_get()->stop_flag = 1; }

/* -------------------- raw socket setup (供 pal_socket/lwip_raw.c 用) --------------------
 * 裸机没有 lwip_socket()/fd。这几个函数建/配 raw_conn_t 包装, 返回它作 handle。
 * 只做非阻塞 setup, 不做 sync IO (裸机决策: 不提供 sync pal_socket)。 */

void *anet_raw_new_tcp(void) {
    ensure_lwip_init();
    raw_conn_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    /* 双栈 pcb (IPADDR_TYPE_ANY): 同一 pcb 可 bind/connect v4 或 v6。 */
    c->pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!c->pcb) { free(c); return NULL; }
    tcp_arg(c->pcb, c);
    tcp_recv(c->pcb, on_recv);
    tcp_sent(c->pcb, on_sent);
    tcp_err(c->pcb, on_err);
    return c;
}

/* sockaddr (v4/v6) -> lwIP ip_addr_t + port。成功返 0。 */
static int sockaddr_to_ip(const struct sockaddr *addr, ip_addr_t *ip, u16_t *port) {
    const struct sockaddr_in *sin = (const struct sockaddr_in*)addr;
    if (sin->sin_family == AF_INET) {
        ip_addr_set_ip4_u32(ip, sin->sin_addr.s_addr);
        *port = lwip_ntohs(sin->sin_port);
        return 0;
    }
#if LWIP_IPV6
    if (sin->sin_family == AF_INET6) {
        const struct sockaddr_in6 *s6 = (const struct sockaddr_in6*)addr;
        /* sin6_addr.s6_addr 是 16 字节网络序; ip6 的 addr[4] 也存网络序。 */
        memset(ip, 0, sizeof(*ip));
        IP_SET_TYPE(ip, IPADDR_TYPE_V6);
        memcpy(ip_2_ip6(ip)->addr, s6->sin6_addr.s6_addr, 16);
        *port = lwip_ntohs(s6->sin6_port);
        return 0;
    }
#endif
    return -1;
}

int anet_raw_bind(void *handle, const struct sockaddr *addr, int addrlen) {
    (void)addrlen;
    raw_conn_t *c = (raw_conn_t*)handle;
    if (!c || !c->pcb) return -1;
    ip_addr_t ip;
    u16_t port;
    if (sockaddr_to_ip(addr, &ip, &port) != 0) return -1;
    return tcp_bind(c->pcb, &ip, port) == ERR_OK ? 0 : -1;
}

int anet_raw_listen(void *handle, int backlog) {
    raw_conn_t *c = (raw_conn_t*)handle;
    if (!c || !c->pcb) return -1;
    struct tcp_pcb *lp = tcp_listen_with_backlog(c->pcb, (u8_t)backlog);
    if (!lp) return -1;
    c->pcb = lp;              /* tcp_listen 换了 pcb (释放旧的, 返回 listen pcb) */
    c->is_listener = 1;
    tcp_arg(lp, c);
    tcp_accept(lp, on_accept);
    return 0;
}

int anet_raw_getsockname(void *handle, struct sockaddr *addr, int *addrlen) {
    raw_conn_t *c = (raw_conn_t*)handle;
    if (!c || !c->pcb || !addr || !addrlen || *addrlen < (int)sizeof(struct sockaddr_in))
        return -1;
    /* IPv4 only (raw echo 路径)。tcp_pcb 的 local_ip/local_port 是本地绑定地址。 */
    if (!IP_IS_V4_VAL(c->pcb->local_ip)) return -1;
    struct sockaddr_in *sin = (struct sockaddr_in*)addr;
    memset(sin, 0, sizeof(*sin));
    sin->sin_family = AF_INET;
    sin->sin_port = lwip_htons(c->pcb->local_port);
    sin->sin_addr.s_addr = ip_addr_get_ip4_u32(&c->pcb->local_ip);
    *addrlen = (int)sizeof(*sin);
    return 0;
}

void anet_raw_close(void *handle) {
    raw_conn_t *c = (raw_conn_t*)handle;
    if (!c) return;
    if (c->rx_head) { pbuf_free(c->rx_head); c->rx_head = NULL; }
    for (int i=0;i<c->acc_n;i++) anet_raw_close(c->acc_q[i]);
    free(c->acc_q);
    if (c->pcb) {
        tcp_arg(c->pcb, NULL);
        if (c->is_listener) {
            /* LISTEN pcb: 只能清 accept 回调; tcp_recv/sent 在 LISTEN 态会 assert。 */
            tcp_accept(c->pcb, NULL);
            tcp_close(c->pcb);
        } else {
            tcp_recv(c->pcb, NULL); tcp_sent(c->pcb, NULL); tcp_err(c->pcb, NULL);
            if (tcp_close(c->pcb) != ERR_OK) tcp_abort(c->pcb);
        }
    }
    free(c);
}

/* -------------------- create / destroy -------------------- */

loop_t *loop_create_sub(void) {
    ensure_lwip_init();
    loop_t *loop = calloc(1, sizeof(*loop));
    return loop;
}

void loop_destroy(void) {
    loop_t *loop = loop_get();
    if (!loop) return;
    for (int i=0;i<loop->heap_size;++i) free(loop->heap[i]);
    free(loop->heap);
    soon_task_t *s=loop->soon_head;
    while(s){ soon_task_t *n=s->next; free(s); s=n; }
    free(loop);
    current_loop = NULL;
}




