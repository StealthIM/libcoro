/*
 * 阶段 2b: 裸机 (NO_SYS=1) HTTP keep-alive 环回测试。
 *
 * 单连接串行发 2 个请求 (第 1 个 keep-alive, 第 2 个 Connection: close), 校验
 * 两个响应都拿到。这压 raw 后端的两条之前只单发验过的路径:
 *   - 同一连接多次 loop_post_recv (server 每请求一轮 stream_read)
 *   - rx pbuf 链的部分消费 / 跨请求残留 (rx_off 偏移逻辑)
 *   - http_server 的 keep-alive 外层循环 + pipelined memmove
 *
 * 客户端用 future_socket 手动发裸 HTTP/1.1。TLS 走桩。
 * bootstrap 复用 boot_raw.c。
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "libcoro.h"
#include "http_server.h"
#include "sock/future_socket.h"
#include "sock/pal_socket.h"

#include "lwip/def.h"

#define PORT      55451
#define RESP_BODY "ka-resp"

static int g_pass = 0;
static int g_req_count = 0;
static anet_http_server_t *g_server = NULL;


static void handler(const anet_http_server_request_t *req,
                    anet_http_server_response_t *resp, void *ud) {
    (void)ud;
    g_req_count++;
    printf("srv handler #%d: %s %s\n", g_req_count, req->method, req->path);
    resp->status_code = 200;
    resp->body = RESP_BODY;
    resp->body_len = strlen(RESP_BODY);
}

/* 用协程发 2 个请求, 每步 yield 等 future。 */
task_t* task_arg(client_task) {
    gen_dec_vars(
        anet_socket_t *sock;
        future_t       *fut;
        char            buf[512];
        int             n;
        int             got1;
        int             got2;
    );
    gen_begin(ctx);

    gen_var(sock) = anet_socket_create(anet_palsock_create(0,0,0,1));
    if (!gen_var(sock)) { printf("client create fail\n"); gen_return(1); }

    {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = PP_HTONS(PORT);
        addr.sin_addr.s_addr = PP_HTONL(INADDR_LOOPBACK);
        gen_var(fut) = anet_socket_connect(gen_var(sock), (struct sockaddr*)&addr, sizeof(addr));
    }
    gen_yield(gen_var(fut));
    if (anet_code_of(future_result(gen_var(fut))) < 0) { printf("connect fail\n"); gen_return(1); }

    /* --- 请求 1: keep-alive --- */
    {
        static const char R1[] = "GET /one HTTP/1.1\r\nHost: x\r\n\r\n";
        gen_var(fut) = anet_socket_send(gen_var(sock), R1, sizeof(R1)-1);
    }
    gen_yield(gen_var(fut));
    if (anet_code_of(future_result(gen_var(fut))) < 0) { printf("send1 fail\n"); gen_return(1); }

    /* 收响应 1 (循环 recv 到含 RESP_BODY) */
    gen_var(got1) = 0;
    while (gen_var(got1) < (int)sizeof(gen_var(buf)) - 1) {
        gen_var(fut) = anet_socket_recv(gen_var(sock), gen_var(buf)+gen_var(got1),
                                         sizeof(gen_var(buf))-1-gen_var(got1));
        gen_yield(gen_var(fut));
        gen_var(n) = (int)anet_code_of(future_result(gen_var(fut)));
        if (gen_var(n) <= 0) break;
        gen_var(got1) += gen_var(n);
        gen_var(buf)[gen_var(got1)] = '\0';
        if (strstr(gen_var(buf), RESP_BODY)) break;   /* 响应 1 到齐 */
    }
    if (!(gen_var(got1) > 0 && strstr(gen_var(buf), RESP_BODY))) {
        printf("resp1 fail got=%d\n", gen_var(got1)); gen_return(1);
    }

    /* --- 请求 2: Connection: close --- */
    {
        static const char R2[] = "GET /two HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
        gen_var(fut) = anet_socket_send(gen_var(sock), R2, sizeof(R2)-1);
    }
    gen_yield(gen_var(fut));
    if (anet_code_of(future_result(gen_var(fut))) < 0) { printf("send2 fail\n"); gen_return(1); }

    gen_var(got2) = 0;
    while (gen_var(got2) < (int)sizeof(gen_var(buf)) - 1) {
        gen_var(fut) = anet_socket_recv(gen_var(sock), gen_var(buf)+gen_var(got2),
                                         sizeof(gen_var(buf))-1-gen_var(got2));
        gen_yield(gen_var(fut));
        gen_var(n) = (int)anet_code_of(future_result(gen_var(fut)));
        if (gen_var(n) <= 0) break;
        gen_var(got2) += gen_var(n);
        gen_var(buf)[gen_var(got2)] = '\0';
        if (strstr(gen_var(buf), RESP_BODY)) break;
    }
    if (!(gen_var(got2) > 0 && strstr(gen_var(buf), RESP_BODY))) {
        printf("resp2 fail got=%d\n", gen_var(got2)); gen_return(1);
    }

    if (g_req_count == 2) {
        g_pass = 1;
        printf("keepalive ok: server saw %d requests on 1 conn\n", g_req_count);
    } else {
        printf("keepalive mismatch: req_count=%d\n", g_req_count);
    }

    anet_socket_close(gen_var(sock));
    anet_http_server_stop(g_server);
    loop_stop();
    gen_end(0);
}

int run_echo_loop(void) {   /* 复用 boot_raw.c 入口名 */
    printf("bare-metal HTTP keep-alive (NO_SYS over loopback)\n");
    loop_get();

    g_server = anet_http_server_create(PORT, handler, NULL);
    if (!g_server) { printf("server create fail\n"); return 1; }

    task_t *st = anet_http_server_run(g_server);
    task_t *ct = client_task(NULL);
    task_run(st);
    loop_run(ct);

    anet_http_server_destroy(g_server);
    loop_destroy();

    printf("%s\n", g_pass ? "STAGE2B KEEPALIVE OK" : "STAGE2B KEEPALIVE FAIL");
    return g_pass ? 0 : 1;
}
