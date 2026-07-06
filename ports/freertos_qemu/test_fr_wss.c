/*
 * 阶段 2b + TLS: 裸机 (NO_SYS=1) WebSocket over TLS (wss) server 环回测试。
 *
 * 证 asyncweb 的 anet_async_ws_accept_tls_mem (服务端 wss 握手, 内存证书) +
 * anet_async_ws_connect_mem (客户端 wss, 内存 CA) 在裸机 raw 后端端到端可用。
 * 是 test_fr_ws (明文) + test_fr_tls_raw (TLS) 的合流。
 *
 * 证书: certs_test.h 的 ECC P-256 DER。bootstrap 复用 boot_raw.c。
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "libcoro.h"
#include "ws.h"
#include "sock/future_socket.h"
#include "sock/pal_socket.h"

#include "lwip/def.h"

#define USE_CERT_BUFFERS_256
#include <wolfssl/ssl.h>
#include <wolfssl/certs_test.h>

#define PORT  55450
#define MSG   "wss-over-bare-metal"

static int g_pass = 0;
static anet_listener_t *g_listener = NULL;

/* ---- 服务端: accept -> wss 握手(内存证书) -> recv -> echo ---- */
task_t* task_arg(server_task) {
    gen_dec_vars(
        anet_listener_t *listener;
        future_t         *afut;
        anet_socket_t   *conn;
        anet_async_ws_t  *ws;
        task_t           *t;
        anet_ws_message_t msg;
    );
    gen_begin(ctx);
    gen_var(listener) = (anet_listener_t*)gen_userdata();

    gen_var(afut) = anet_socket_accept(gen_var(listener));
    gen_yield(gen_var(afut));
    gen_var(conn) = (anet_socket_t*)future_result(gen_var(afut));
    if (!gen_var(conn)) { printf("accept failed\n"); gen_return(1); }

    gen_var(t) = anet_async_ws_accept_tls_mem(gen_var(conn),
        serv_ecc_der_256, (int)sizeof_serv_ecc_der_256,
        ecc_key_der_256,  (int)sizeof_ecc_key_der_256, 1, &gen_var(ws));
    gen_yield_from_task(gen_var(t));
    if (anet_status_of(future_result(gen_var(t)->future)) != ANET_OK) {
        printf("server wss accept failed\n"); gen_return(1);
    }

    memset(&gen_var(msg), 0, sizeof(gen_var(msg)));
    gen_var(t) = anet_async_ws_recv(gen_var(ws), &gen_var(msg));
    gen_yield_from_task(gen_var(t));
    if (anet_status_of(future_result(gen_var(t)->future)) != ANET_OK) {
        printf("server wss recv failed\n"); gen_return(1);
    }

    gen_var(t) = anet_async_ws_send(gen_var(ws), gen_var(msg).type,
                                    gen_var(msg).data, gen_var(msg).len);
    gen_yield_from_task(gen_var(t));

    gen_end(0);
}

/* ---- 客户端: connect (wss://, 内存 CA) -> send -> recv 校验 ---- */
task_t* task_arg(client_task) {
    gen_dec_vars(
        anet_async_ws_t   *ws;
        task_t            *t;
        anet_ws_message_t  msg;
    );
    gen_begin(ctx);

    gen_var(t) = anet_async_ws_connect_mem("wss://127.0.0.1:55450/",
        ca_ecc_cert_der_256, (int)sizeof_ca_ecc_cert_der_256, 1, &gen_var(ws));
    gen_yield_from_task(gen_var(t));
    if (anet_status_of(future_result(gen_var(t)->future)) != ANET_OK) {
        printf("client wss connect failed\n"); gen_return(1);
    }

    gen_var(t) = anet_async_ws_send(gen_var(ws), ANET_WS_TEXT, MSG, strlen(MSG));
    gen_yield_from_task(gen_var(t));
    if (anet_status_of(future_result(gen_var(t)->future)) != ANET_OK) {
        printf("client wss send failed\n"); gen_return(1);
    }

    memset(&gen_var(msg), 0, sizeof(gen_var(msg)));
    gen_var(t) = anet_async_ws_recv(gen_var(ws), &gen_var(msg));
    gen_yield_from_task(gen_var(t));
    if (anet_status_of(future_result(gen_var(t)->future)) != ANET_OK) {
        printf("client wss recv failed\n"); gen_return(1);
    }

    if (gen_var(msg).len == strlen(MSG) && memcmp(gen_var(msg).data, MSG, gen_var(msg).len) == 0) {
        g_pass = 1;
        printf("wss echo ok: got '%.*s'\n", (int)gen_var(msg).len, gen_var(msg).data);
    } else {
        printf("wss echo mismatch len=%d\n", (int)gen_var(msg).len);
    }

    loop_stop();
    gen_end(0);
}

int run_echo_loop(void) {   /* 复用 boot_raw.c 入口名 */
    printf("bare-metal WebSocket-over-TLS server (wolfSSL over raw loopback)\n");
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

    g_listener = anet_listener_create(lsock);

    task_t *st = server_task(g_listener);
    task_t *ct = client_task(NULL);
    task_run(st);
    loop_run(ct);

    anet_listener_close(g_listener);
    loop_destroy();

    printf("%s\n", g_pass ? "STAGE2B WSS OK" : "STAGE2B WSS FAIL");
    return g_pass ? 0 : 1;
}
