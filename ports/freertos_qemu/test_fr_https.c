/*
 * 阶段 2b + TLS: 裸机 (NO_SYS=1) 高层 HTTPS server 环回测试。
 *
 * 证 asyncweb 的 anet_http_server_* + anet_http_server_use_tls_mem (内存证书)
 * 在裸机 raw 后端跑 HTTPS: server 每连接先做 wolfSSL 服务端握手, 再走统一
 * async_stream 读请求/写响应。客户端用 async_ssl (内存 CA 校验) + future_socket
 * 手动发一个 HTTP/1.1 请求, 校验响应含预期 body。
 *
 * 证书: certs_test.h 的 ECC P-256 DER (内存, NO_FILESYSTEM)。
 * bootstrap 复用 boot_raw.c 的 run_echo_loop()。
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "libcoro.h"
#include "http_server.h"
#include "tls.h"
#include "sock/future_socket.h"
#include "sock/pal_socket.h"

#include "lwip/def.h"

#define USE_CERT_BUFFERS_256
#include <wolfssl/ssl.h>
#include <wolfssl/certs_test.h>

#define PORT      55448
#define RESP_BODY "hello-from-bare-metal-https"

static int g_pass = 0;
static anet_http_server_t *g_server = NULL;

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

/* ---- 客户端协程: connect -> TLS 握手 -> 发 GET -> 收响应 -> 校验 ---- */
task_t* task_arg(client_task) {
    gen_dec_vars(
        async_socket_t *sock;
        async_ssl_t    *ssl;
        future_t       *cfut;
        task_t         *t;
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
        gen_var(cfut) = async_socket_connect(gen_var(sock), (struct sockaddr*)&addr, sizeof(addr));
    }
    gen_yield(gen_var(cfut));
    if (anet_code_of(future_result(gen_var(cfut))) < 0) { printf("connect fail\n"); gen_return(1); }

    gen_var(ssl) = async_ssl_create_mem(ASYNC_SSL_CLIENT, NULL,
        ca_ecc_cert_der_256, (int)sizeof_ca_ecc_cert_der_256, 1);
    if (!gen_var(ssl)) { printf("client ssl create fail\n"); gen_return(1); }
    async_ssl_attach_socket(gen_var(ssl), gen_var(sock));

    gen_var(t) = async_ssl_handshake(gen_var(ssl));
    gen_yield_from_task(gen_var(t));
    if (anet_code_of(future_result(gen_var(t)->future)) < 0) { printf("client handshake fail\n"); gen_return(1); }

    {
        static const char REQ[] =
            "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        gen_var(t) = async_ssl_write(gen_var(ssl), REQ, sizeof(REQ)-1);
    }
    gen_yield_from_task(gen_var(t));
    if (anet_code_of(future_result(gen_var(t)->future)) < 0) { printf("client ssl_write fail\n"); gen_return(1); }

    gen_var(total) = 0;
    while (gen_var(total) < (int)sizeof(gen_var(buf)) - 1) {
        gen_var(t) = async_ssl_read(gen_var(ssl),
                        gen_var(buf) + gen_var(total),
                        sizeof(gen_var(buf)) - 1 - gen_var(total));
        gen_yield_from_task(gen_var(t));
        gen_var(n) = (int)anet_code_of(future_result(gen_var(t)->future));
        if (gen_var(n) <= 0) break;
        gen_var(total) += gen_var(n);
    }
    gen_var(buf)[gen_var(total)] = '\0';

    if (gen_var(total) > 0 && strstr(gen_var(buf), RESP_BODY) && strstr(gen_var(buf), "200")) {
        g_pass = 1;
        printf("https client ok: %d bytes, body matched\n", gen_var(total));
    } else {
        printf("https client mismatch: %d bytes\n", gen_var(total));
    }

    async_ssl_destroy(gen_var(ssl));
    anet_http_server_stop(g_server);
    loop_stop();
    gen_end(0);
}

int run_echo_loop(void) {   /* 复用 boot_raw.c 入口名 */
    printf("bare-metal HTTPS server (wolfSSL over raw lwIP loopback)\n");
    loop_get();

    g_server = anet_http_server_create(PORT, handler, NULL);
    if (!g_server) { printf("server create fail\n"); return 1; }
    if (anet_http_server_use_tls_mem(g_server,
            serv_ecc_der_256, (int)sizeof_serv_ecc_der_256,
            ecc_key_der_256,  (int)sizeof_ecc_key_der_256, 1) != 0) {
        printf("use_tls_mem fail\n"); return 1;
    }

    task_t *st = anet_http_server_run(g_server);
    task_t *ct = client_task(NULL);
    task_run(st);
    loop_run(ct);

    anet_http_server_destroy(g_server);
    loop_destroy();

    printf("%s\n", g_pass ? "STAGE2B HTTPS OK" : "STAGE2B HTTPS FAIL");
    return g_pass ? 0 : 1;
}
