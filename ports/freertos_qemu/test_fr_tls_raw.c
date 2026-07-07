/*
 * 裸机 (NO_SYS=1, raw lwIP) 上的 wolfSSL TLS echo。
 *
 * socket 模式版是 test_fr_tls.c; 本测试是 raw (NO_SYS) 版 —— 同样的
 * async_ssl_* + future_socket, 但底层是 lwip_raw.c 回调后端 +
 * pal_socket/lwip_raw (anet_palsock_t=void*)。密文过 lwIP 环回。
 * 证 tls_stub 可换成真 wolfSSL。
 *
 * 证书: certs_test.h 的 ECC P-256 DER (内存, NO_FILESYSTEM)。
 * bootstrap 复用 boot_raw.c 的 run_echo_loop()。
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <libcoro/libcoro.h>
#include <asyncweb_internal/tls.h>
#include <asyncweb/socket.h>
#include <asyncweb/palsock.h>

#include "lwip/def.h"

#define USE_CERT_BUFFERS_256
#include <wolfssl/ssl.h>
#include <wolfssl/certs_test.h>

#define MSG   "ping-over-bare-metal-tls"
#define PORT  55447

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

    gen_var(t) = async_ssl_read(gen_var(ssl), gen_var(buf), sizeof(gen_var(buf)));
    gen_yield_from_task(gen_var(t));
    gen_var(n) = (int)anet_code_of(future_result(gen_var(t)->future));
    if (gen_var(n) <= 0) { printf("server ssl_read failed n=%d\n", gen_var(n)); gen_return(1); }

    gen_var(t) = async_ssl_write(gen_var(ssl), gen_var(buf), gen_var(n));
    gen_yield_from_task(gen_var(t));
    if (anet_code_of(future_result(gen_var(t)->future)) < 0) {
        printf("server ssl_write failed\n"); gen_return(1);
    }

    async_ssl_destroy(gen_var(ssl));
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

    gen_var(sock) = anet_socket_create(anet_palsock_create(0,0,0,1));
    if (!gen_var(sock)) { printf("client create fail\n"); gen_return(1); }

    {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = PP_HTONS(PORT);
        addr.sin_addr.s_addr = PP_HTONL(INADDR_LOOPBACK);
        gen_var(cfut) = anet_socket_connect(gen_var(sock), (struct sockaddr*)&addr, sizeof(addr));
    }
    gen_yield(gen_var(cfut));
    if (anet_code_of(future_result(gen_var(cfut))) < 0) { printf("connect failed\n"); gen_return(1); }

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

int run_echo_loop(void) {   /* 复用 boot_raw.c 入口名 */
    printf("bare-metal TLS echo (wolfSSL over raw lwIP loopback)\n");
    loop_get();

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = PP_HTONS(PORT);
    a.sin_addr.s_addr = PP_HTONL(INADDR_LOOPBACK);

    anet_palsock_t lsock = anet_palsock_create(0,0,0,1);
    if (!lsock) { printf("listen sock create fail\n"); return 1; }
    if (anet_palsock_bind(lsock, (struct sockaddr*)&a, sizeof(a)) != 0) { printf("bind fail\n"); return 1; }
    if (anet_palsock_listen(lsock, 4) != 0) { printf("listen fail\n"); return 1; }

    anet_listener_t *listener = anet_listener_create(lsock);

    task_t *st = server_task(listener);
    task_t *ct = client_task(NULL);
    task_run(st);
    loop_run(ct);

    anet_listener_close(listener);
    loop_destroy();

    printf("%s\n", g_pass ? "STAGE2B TLS OK" : "STAGE2B TLS FAIL");
    return g_pass ? 0 : 1;
}
