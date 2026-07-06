/*
 * 阶段 2b: 裸机 (NO_SYS=1) 高层 HTTP server (明文) 环回测试。
 *
 * 证 asyncweb 的 anet_http_server_* (http_server.c: accept 循环 + 每连接协程 +
 * 请求解析 + handler + 响应 + keep-alive) 在裸机 raw 后端上端到端可用。
 * 客户端用 future_socket 发一个裸 HTTP/1.1 请求, 校验响应含预期 body。
 *
 * TLS 走桩 (tls_stub.c): 明文 server 不创建 SSL 会话, async_ssl_* 仅链接期
 * 需要。裸机 TLS 集成是后续步骤。
 *
 * bootstrap 复用 boot_raw.c 的 run_echo_loop()。
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "libcoro.h"
#include "http_server.h"
#include "sock/future_socket.h"
#include "sock/pal_socket.h"

#include "lwip/def.h"

#define PORT      55446
#define RESP_BODY "hello-from-bare-metal-http"

static int g_pass = 0;
static anet_http_server_t *g_server = NULL;

/* ---- server handler: 任何请求都回 RESP_BODY ---- */
static void handler(const anet_http_server_request_t *req,
                    anet_http_server_response_t *resp, void *ud) {
    (void)ud;
    printf("srv handler: %s %s\n", req->method, req->path);
    resp->status_code = 200;
    resp->status_text = "OK";
    resp->content_type = "text/plain";
    resp->body = RESP_BODY;
    resp->body_len = strlen(RESP_BODY);
}

/* ---- 客户端协程: connect -> 发 GET -> 收响应 -> 校验含 RESP_BODY ---- */
task_t* task_arg(client_task) {
    gen_dec_vars(
        async_socket_t *sock;
        future_t       *fut;
        char            buf[512];
        int             total;
        int             n;
    );
    gen_begin(ctx);

    gen_var(sock) = async_socket_create(anet_palsock_create(0,0,0,1));
    if (!gen_var(sock)) { printf("client create fail\n"); gen_return(1); }

    {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = PP_HTONS(PORT);
        addr.sin_addr.s_addr = PP_HTONL(INADDR_LOOPBACK);
        gen_var(fut) = async_socket_connect(gen_var(sock), (struct sockaddr*)&addr, sizeof(addr));
    }
    gen_yield(gen_var(fut));
    if (anet_code_of(future_result(gen_var(fut))) < 0) { printf("connect fail\n"); gen_return(1); }

    {
        static const char REQ[] =
            "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        gen_var(fut) = async_socket_send(gen_var(sock), REQ, sizeof(REQ)-1);
    }
    gen_yield(gen_var(fut));
    if (anet_code_of(future_result(gen_var(fut))) < 0) { printf("client send fail\n"); gen_return(1); }

    /* 读响应 (可能多段): 循环收到 EOF 或缓冲满 */
    gen_var(total) = 0;
    while (gen_var(total) < (int)sizeof(gen_var(buf)) - 1) {
        gen_var(fut) = async_socket_recv(gen_var(sock),
                          gen_var(buf) + gen_var(total),
                          sizeof(gen_var(buf)) - 1 - gen_var(total));
        gen_yield(gen_var(fut));
        gen_var(n) = (int)anet_code_of(future_result(gen_var(fut)));
        if (gen_var(n) <= 0) break;   /* EOF 或错误 */
        gen_var(total) += gen_var(n);
    }
    gen_var(buf)[gen_var(total)] = '\0';

    if (gen_var(total) > 0 && strstr(gen_var(buf), RESP_BODY)
        && strstr(gen_var(buf), "200")) {
        g_pass = 1;
        printf("http client ok: %d bytes, body matched\n", gen_var(total));
    } else {
        printf("http client mismatch: %d bytes\n", gen_var(total));
    }

    async_socket_close(gen_var(sock));
    anet_http_server_stop(g_server);
    loop_stop();
    gen_end(0);
}

int run_echo_loop(void) {   /* 复用 boot_raw.c 入口名 */
    printf("bare-metal HTTP server (NO_SYS over loopback)\n");
    loop_get();

    g_server = anet_http_server_create(PORT, handler, NULL);
    if (!g_server) { printf("server create fail\n"); return 1; }

    task_t *st = anet_http_server_run(g_server);
    task_t *ct = client_task(NULL);
    task_run(st);
    loop_run(ct);

    anet_http_server_destroy(g_server);
    loop_destroy();

    printf("%s\n", g_pass ? "STAGE2B HTTP OK" : "STAGE2B HTTP FAIL");
    return g_pass ? 0 : 1;
}
