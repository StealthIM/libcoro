/*
 * 裸机 (NO_SYS=1, raw lwIP) 环回明文 echo。
 *
 * 直连 libcoro loop 的异步 IO API (loop_accept_async / loop_connect_async /
 * loop_post_recv / loop_post_send), handle 是 raw_setup.h 的 anet_raw_new_tcp
 * 返回的 raw_conn_t*。不经 asyncweb 层。证 lwip_raw.c 后端端到端。
 *
 * 只 include libcoro + lwIP raw 头, 不碰 FreeRTOS 头 (同 test_fr_echo.c);
 * bootstrap 复用 boot_fr.c 的 run_echo_loop()。
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <libcoro/loop.h>
#include <libcoro/raw_setup.h>       /* anet_raw_* setup 声明在 LIBCORO_LWIP_RAW 分支里 */

#include "lwip/def.h"   /* PP_HTONS / PP_HTONL (sockaddr 用 raw_inet.h) */

#define MSG   "ping-over-bare-metal-raw"
#define PORT  55444

static void *g_listener  = NULL;
static void *g_accept_out = NULL;
static void *g_accepted  = NULL;
static void *g_client    = NULL;
static char  g_srv_buf[128];
static char  g_cli_buf[128];
static int   g_pass = 0;

/* ---- 服务端: accept -> recv -> echo send ---- */
static void on_server_sent(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)n;(void)r;
    if (err != 0) { printf("server send fail err=%d\n", err); loop_stop(); }
}
static void on_server_recv(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)r;
    if (err != 0 || n == 0) { printf("server recv fail err=%d n=%lu\n", err, n); loop_stop(); return; }
    loop_post_send(g_accepted, g_srv_buf, n, on_server_sent, NULL);  /* echo */
}
static void on_accept(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)n;(void)r;
    if (err != 0) { printf("accept fail err=%d\n", err); loop_stop(); return; }
    g_accepted = g_accept_out;
    loop_post_recv(g_accepted, g_srv_buf, sizeof(g_srv_buf), on_server_recv, NULL);
}

/* ---- 客户端: connect -> send -> recv 校验 ---- */
static void on_client_recv(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)r;
    if (err != 0 || n == 0) { printf("client recv fail err=%d n=%lu\n", err, n); loop_stop(); return; }
    if (n == strlen(MSG) && memcmp(g_cli_buf, MSG, n) == 0) {
        g_pass = 1; printf("raw echo ok: got '%.*s'\n", (int)n, g_cli_buf);
    } else printf("raw echo mismatch: %lu bytes\n", n);
    loop_stop();
}
static void on_client_sent(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)n;(void)r;
    if (err != 0) { printf("client send fail err=%d\n", err); loop_stop(); return; }
    loop_post_recv(g_client, g_cli_buf, sizeof(g_cli_buf), on_client_recv, NULL);
}
static void on_connect(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)n;(void)r;
    if (err != 0) { printf("connect fail err=%d\n", err); loop_stop(); return; }
    loop_post_send(g_client, MSG, strlen(MSG), on_client_sent, NULL);
}

int run_echo_loop(void) {   /* 复用 boot_fr.c 入口名 */
    printf("bare-metal raw echo (lwIP NO_SYS over loopback)\n");
    loop_get();   /* 拉起 lwIP 栈 (lwip_init, 幂等) */

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = PP_HTONS(PORT);
    addr.sin_addr.s_addr = PP_HTONL(INADDR_LOOPBACK);

    /* server: new -> bind -> listen -> accept */
    g_listener = anet_raw_new_tcp();
    if (!g_listener) { printf("listener new fail\n"); return 1; }
    if (anet_raw_bind(g_listener, (struct sockaddr*)&addr, sizeof(addr)) != 0) { printf("bind fail\n"); return 1; }
    if (anet_raw_listen(g_listener, 4) != 0) { printf("listen fail\n"); return 1; }
    loop_accept_async(g_listener, &g_accept_out, on_accept, NULL);

    /* client: new -> connect */
    g_client = anet_raw_new_tcp();
    if (!g_client) { printf("client new fail\n"); return 1; }
    loop_connect_async(g_client, (struct sockaddr*)&addr, sizeof(addr), on_connect, NULL);

    loop_run(NULL);

    if (g_accepted) anet_raw_close(g_accepted);
    if (g_client)   anet_raw_close(g_client);
    if (g_listener) anet_raw_close(g_listener);
    loop_destroy();

    printf("%s\n", g_pass ? "STAGE2B RAW OK" : "STAGE2B RAW FAIL");
    return g_pass ? 0 : 1;
}
