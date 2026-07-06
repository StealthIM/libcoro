/*
 * 阶段 2b: 裸机 (NO_SYS=1) IPv6 环回明文 echo (::1)。
 *
 * test_fr_raw 的 v6 版: sockaddr_in6 + ::1 环回, 走 raw 后端的 v6 路径
 * (tcp_new_ip_type(ANY) 双栈 pcb + sockaddr_to_ip 的 AF_INET6 分支)。
 * 证 raw 后端 IPv6 connect/bind/accept 通。
 *
 * bootstrap 复用 boot_raw.c。
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "loop.h"

#include "lwip/def.h"

#define MSG   "ping6-bare-metal"
#define PORT  55461

static void *g_listener  = NULL;
static void *g_accept_out = NULL;
static void *g_accepted  = NULL;
static void *g_client    = NULL;
static char  g_srv_buf[128];
static char  g_cli_buf[128];
static int   g_pass = 0;

/* ::1 环回地址填入 sockaddr_in6 */
static void fill_loopback6(struct sockaddr_in6 *a) {
    memset(a, 0, sizeof(*a));
    a->sin6_family = AF_INET6;
    a->sin6_port = PP_HTONS(PORT);
    a->sin6_addr.s6_addr[15] = 1;   /* ::1 */
}

static void on_server_sent(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r);

static void on_server_recv(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)r;
    if (err != 0 || n == 0) { printf("server recv fail err=%d n=%lu\n", err, n); loop_stop(); return; }
    loop_post_send(g_accepted, g_srv_buf, n, on_server_sent, NULL);
}
static void on_server_sent(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)n;(void)r;
    if (err != 0) { printf("server send fail err=%d\n", err); loop_stop(); }
}
static void on_accept(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)n;(void)r;
    if (err != 0) { printf("accept fail err=%d\n", err); loop_stop(); return; }
    g_accepted = g_accept_out;
    loop_post_recv(g_accepted, g_srv_buf, sizeof(g_srv_buf), on_server_recv, NULL);
}
static void on_client_recv(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)r;
    if (err != 0 || n == 0) { printf("client recv fail err=%d n=%lu\n", err, n); loop_stop(); return; }
    if (n == strlen(MSG) && memcmp(g_cli_buf, MSG, n) == 0) {
        g_pass = 1; printf("raw6 echo ok: got '%.*s'\n", (int)n, g_cli_buf);
    } else printf("raw6 echo mismatch: %lu bytes\n", n);
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

int run_echo_loop(void) {   /* 复用 boot_raw.c 入口名 */
    printf("bare-metal IPv6 raw echo (::1 loopback)\n");
    loop_get();

    struct sockaddr_in6 addr;
    fill_loopback6(&addr);

    g_listener = anet_raw_new_tcp();
    if (!g_listener) { printf("listener new fail\n"); return 1; }
    if (anet_raw_bind(g_listener, (struct sockaddr*)&addr, sizeof(addr)) != 0) { printf("bind fail\n"); return 1; }
    if (anet_raw_listen(g_listener, 4) != 0) { printf("listen fail\n"); return 1; }
    loop_accept_async(g_listener, &g_accept_out, on_accept, NULL);

    g_client = anet_raw_new_tcp();
    if (!g_client) { printf("client new fail\n"); return 1; }
    loop_connect_async(g_client, (struct sockaddr*)&addr, sizeof(addr), on_connect, NULL);

    loop_run(NULL);

    if (g_accepted) anet_raw_close(g_accepted);
    if (g_client)   anet_raw_close(g_client);
    if (g_listener) anet_raw_close(g_listener);
    loop_destroy();

    printf("%s\n", g_pass ? "STAGE2B RAW6 OK" : "STAGE2B RAW6 FAIL");
    return g_pass ? 0 : 1;
}
