/*
 * 阶段 2b: 裸机 (NO_SYS=1) 经 asyncweb future_socket 层的环回明文 echo。
 *
 * test_fr_raw.c 直连 libcoro 的 raw loop API; 本测试更进一步, 走 asyncweb 的
 * anet_socket_* / anet_listener_* (future_socket.c) —— 证 pal_socket/lwip_raw.c
 * (anet_palsock_t=void*, setup 转发到 anet_raw_*) + future_socket 在裸机后端上
 * 端到端可用。协程用 gen_* (同 test_fr_tls.c 的服务端/客户端结构)。
 *
 * 只 include libcoro + asyncweb 私有头, 不碰 FreeRTOS 头; bootstrap 复用
 * boot_raw.c 的 run_echo_loop()。
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "libcoro.h"
#include "sock/future_socket.h"
#include "sock/pal_socket.h"

#include "lwip/def.h"   /* PP_HTONS / PP_HTONL */

#define MSG   "ping-over-bare-metal-future"
#define PORT  55445

static int g_pass = 0;

/* ---- 服务端协程: accept -> recv -> echo ---- */
task_t* task_arg(server_task) {
    gen_dec_vars(
        anet_listener_t *listener;
        future_t         *fut;
        anet_socket_t   *conn;
        char              buf[128];
        int               n;
    );
    gen_begin(ctx);
    gen_var(listener) = (anet_listener_t*)gen_userdata();

    gen_var(fut) = anet_socket_accept(gen_var(listener));
    gen_yield(gen_var(fut));
    gen_var(conn) = (anet_socket_t*)future_result(gen_var(fut));
    if (!gen_var(conn)) { printf("accept failed\n"); gen_return(1); }

    gen_var(fut) = anet_socket_recv(gen_var(conn), gen_var(buf), sizeof(gen_var(buf)));
    gen_yield(gen_var(fut));
    gen_var(n) = (int)anet_code_of(future_result(gen_var(fut)));
    if (gen_var(n) <= 0) { printf("server recv fail n=%d\n", gen_var(n)); gen_return(1); }

    gen_var(fut) = anet_socket_send(gen_var(conn), gen_var(buf), gen_var(n));
    gen_yield(gen_var(fut));
    if (anet_code_of(future_result(gen_var(fut))) < 0) { printf("server send fail\n"); gen_return(1); }

    anet_socket_close(gen_var(conn));
    gen_end(0);
}

/* ---- 客户端协程: connect -> send -> recv 校验 ---- */
task_t* task_arg(client_task) {
    gen_dec_vars(
        anet_socket_t *sock;
        future_t       *fut;
        char            buf[128];
        int             n;
    );
    gen_begin(ctx);

    gen_var(sock) = anet_socket_create(anet_palsock_create(0,0,0,1));
    if (!gen_var(sock)) { printf("client create fail\n"); gen_return(1); }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = PP_HTONS(PORT);
    addr.sin_addr.s_addr = PP_HTONL(INADDR_LOOPBACK);

    gen_var(fut) = anet_socket_connect(gen_var(sock), (struct sockaddr*)&addr, sizeof(addr));
    gen_yield(gen_var(fut));
    if (anet_code_of(future_result(gen_var(fut))) < 0) { printf("connect failed\n"); gen_return(1); }

    gen_var(fut) = anet_socket_send(gen_var(sock), MSG, strlen(MSG));
    gen_yield(gen_var(fut));
    if (anet_code_of(future_result(gen_var(fut))) < 0) { printf("client send fail\n"); gen_return(1); }

    gen_var(fut) = anet_socket_recv(gen_var(sock), gen_var(buf), sizeof(gen_var(buf)));
    gen_yield(gen_var(fut));
    gen_var(n) = (int)anet_code_of(future_result(gen_var(fut)));
    if (gen_var(n) == (int)strlen(MSG) && memcmp(gen_var(buf), MSG, gen_var(n)) == 0) {
        g_pass = 1;
        printf("future echo ok: got '%.*s'\n", gen_var(n), gen_var(buf));
    } else {
        printf("future echo mismatch n=%d\n", gen_var(n));
    }

    anet_socket_close(gen_var(sock));
    loop_stop();
    gen_end(0);
}

int run_echo_loop(void) {   /* 复用 boot_raw.c 入口名 */
    printf("bare-metal future_socket echo (NO_SYS over loopback)\n");
    loop_get();

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = PP_HTONS(PORT);
    addr.sin_addr.s_addr = PP_HTONL(INADDR_LOOPBACK);

    anet_palsock_t lsock = anet_palsock_create(0,0,0,1);
    if (!lsock) { printf("listen sock create fail\n"); return 1; }
    if (anet_palsock_bind(lsock, (struct sockaddr*)&addr, sizeof(addr)) != 0) { printf("bind fail\n"); return 1; }
    if (anet_palsock_listen(lsock, 4) != 0) { printf("listen fail\n"); return 1; }

    anet_listener_t *listener = anet_listener_create(lsock);

    task_t *st = server_task(listener);
    task_t *ct = client_task(NULL);
    task_run(st);
    loop_run(ct);

    anet_listener_close(listener);
    loop_destroy();

    printf("%s\n", g_pass ? "STAGE2B FUTURE OK" : "STAGE2B FUTURE FAIL");
    return g_pass ? 0 : 1;
}
