/*
 * 阶段 2a + TLS: 嵌入式完整栈 TLS echo。
 *
 * 探针 (probe_wolfssl.c) 只验证了 wolfCrypt 能握手 (client<->server 内存缓冲
 * 直连)。本测试更进一步: 用真的 asyncweb wolfssl.c (async_ssl_*) 把 wolfSSL
 * 接进 libcoro loop 的 async socket —— TLS 密文真的过 lwIP 环回。
 *
 * 链路: server listen -> accept -> async_ssl_create_server_mem -> 握手 ->
 *       ssl_read -> ssl_write (echo);  client connect ->
 *       async_ssl_create_mem -> 握手 -> ssl_write -> ssl_read 校验。
 *
 * 证书: certs_test.h 的 ECC P-256 DER (内存, NO_FILESYSTEM)。
 *
 * 只 include libcoro/asyncweb/wolfSSL, 不碰 FreeRTOS 头 (见 test_fr_echo.c
 * 说明); bootstrap 复用 boot_fr.c 的 run_echo_loop()。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "libcoro.h"
#include "tls.h"
#include "sock/future_socket.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

/* certs_test.h 的 ECC 证书被 HAVE_ECC + USE_CERT_BUFFERS_256 gate;
 * 先 include wolfSSL 头 (经 user_settings 拉起 HAVE_ECC), 再 include 证书。 */
#define USE_CERT_BUFFERS_256
#include <wolfssl/ssl.h>
#include <wolfssl/certs_test.h>

#define MSG   "ping-over-embedded-tls"
#define PORT  55443

static int g_listen_fd = -1;
static int g_pass = 0;

/* ---- 服务端协程: accept -> TLS 握手 -> echo ---- */
task_t* task_arg(server_task) {
    gen_dec_vars(
        anet_listener_t *listener;
        future_t         *afut;
        anet_socket_t   *conn;
        async_ssl_t      *ssl;
        task_t           *t;
        char              buf[128];
        int               n;
    );
    gen_begin(ctx);

    gen_var(listener) = (anet_listener_t*)gen_userdata();

    gen_var(afut) = anet_socket_accept(gen_var(listener));
    gen_yield(gen_var(afut));
    gen_var(conn) = (anet_socket_t*)future_result(gen_var(afut));
    if (!gen_var(conn)) { printf("accept failed\n"); gen_return(1); }

    /* 服务端 SSL: 内存 ECC 证书链 + 私钥 (DER) */
    gen_var(ssl) = async_ssl_create_server_mem(
        serv_ecc_der_256, (int)sizeof_serv_ecc_der_256,
        ecc_key_der_256,  (int)sizeof_ecc_key_der_256, 1);
    if (!gen_var(ssl)) { printf("server ssl create failed\n"); gen_return(1); }
    async_ssl_attach_socket(gen_var(ssl), gen_var(conn));

    gen_var(t) = async_ssl_handshake(gen_var(ssl));
    gen_yield_from_task(gen_var(t));
    if (anet_code_of(future_result(gen_var(t)->future)) < 0) {
        printf("server handshake failed\n"); gen_return(1);
    }

    /* 收一段明文, 原样 echo */
    gen_var(t) = async_ssl_read(gen_var(ssl), gen_var(buf), sizeof(gen_var(buf)));
    gen_yield_from_task(gen_var(t));
    gen_var(n) = (int)anet_code_of(future_result(gen_var(t)->future));
    if (gen_var(n) <= 0) { printf("server ssl_read failed n=%d\n", gen_var(n)); gen_return(1); }

    gen_var(t) = async_ssl_write(gen_var(ssl), gen_var(buf), gen_var(n));
    gen_yield_from_task(gen_var(t));
    if (anet_code_of(future_result(gen_var(t)->future)) < 0) {
        printf("server ssl_write failed\n"); gen_return(1);
    }

    async_ssl_destroy(gen_var(ssl));   /* 连带关 conn socket */
    gen_end(0);
}

/* ---- 客户端协程: connect -> TLS 握手 -> write -> read 校验 ---- */
task_t* task_arg(client_task) {
    gen_dec_vars(
        anet_socket_t *sock;
        async_ssl_t    *ssl;
        future_t       *cfut;
        task_t         *t;
        char            buf[128];
        int             n;
    );
    gen_begin(ctx);

    {
        int fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
        gen_var(sock) = anet_socket_create(fd);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = PP_HTONS(PORT);
    addr.sin_addr.s_addr = PP_HTONL(INADDR_LOOPBACK);

    gen_var(cfut) = anet_socket_connect(gen_var(sock), (struct sockaddr*)&addr, sizeof(addr));
    gen_yield(gen_var(cfut));
    if (anet_code_of(future_result(gen_var(cfut))) < 0) {
        printf("connect failed\n"); gen_return(1);
    }

    /* 客户端 SSL: 内存 CA (ECC) 校验 server; 不校验 hostname (证书 CN 未必匹配) */
    gen_var(ssl) = async_ssl_create_mem(ASYNC_SSL_CLIENT, NULL,
        ca_ecc_cert_der_256, (int)sizeof_ca_ecc_cert_der_256, 1);
    if (!gen_var(ssl)) { printf("client ssl create failed\n"); gen_return(1); }
    async_ssl_attach_socket(gen_var(ssl), gen_var(sock));

    gen_var(t) = async_ssl_handshake(gen_var(ssl));
    gen_yield_from_task(gen_var(t));
    if (anet_code_of(future_result(gen_var(t)->future)) < 0) {
        printf("client handshake failed\n"); gen_return(1);
    }

    gen_var(t) = async_ssl_write(gen_var(ssl), MSG, strlen(MSG));
    gen_yield_from_task(gen_var(t));
    if (anet_code_of(future_result(gen_var(t)->future)) < 0) {
        printf("client ssl_write failed\n"); gen_return(1);
    }

    gen_var(t) = async_ssl_read(gen_var(ssl), gen_var(buf), sizeof(gen_var(buf)));
    gen_yield_from_task(gen_var(t));
    gen_var(n) = (int)anet_code_of(future_result(gen_var(t)->future));
    if (gen_var(n) == (int)strlen(MSG) && memcmp(gen_var(buf), MSG, gen_var(n)) == 0) {
        g_pass = 1;
        printf("tls echo ok: got '%.*s'\n", gen_var(n), gen_var(buf));
    } else {
        printf("tls echo mismatch n=%d\n", gen_var(n));
    }

    async_ssl_destroy(gen_var(ssl));
    loop_stop();
    gen_end(0);
}

int run_echo_loop(void) {   /* 复用 boot_fr.c 入口名 */
    printf("embedded TLS echo (wolfSSL over lwIP loopback)\n");

    /* 先拉起 lwIP 栈 (幂等): ensure_lwip_init 绑在 loop_get/loop_create,
     * 在此之前 lwip_socket/bind 会因栈未初始化而失败。 */
    loop_get();

    /* server listen socket */
    g_listen_fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    lwip_setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = PP_HTONS(PORT);
    a.sin_addr.s_addr = PP_HTONL(INADDR_LOOPBACK);
    if (lwip_bind(g_listen_fd, (struct sockaddr*)&a, sizeof(a)) < 0) { printf("bind fail\n"); return 1; }
    if (lwip_listen(g_listen_fd, 4) < 0) { printf("listen fail\n"); return 1; }

    anet_listener_t *listener = anet_listener_create(g_listen_fd);

    /* 启动 server + client 两个协程, 跑 loop */
    task_t *st = server_task(listener);
    task_t *ct = client_task(NULL);
    task_run(st);
    loop_run(ct);

    anet_listener_close(listener);
    loop_destroy();

    printf("%s\n", g_pass ? "STAGE2A TLS OK" : "STAGE2A TLS FAIL");
    return g_pass ? 0 : 1;
}
