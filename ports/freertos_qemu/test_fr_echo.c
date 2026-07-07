/*
 * 阶段 2a 完整栈冒烟测试: FreeRTOS + lwIP + libcoro loop 的环回 echo。
 *
 * 逻辑和阶段 1 的 test_lwip_echo.c 一致 (server listen / client connect /
 * echo / 校验), 但跑在 FreeRTOS task 里 —— loop 需要 task 上下文, 因为
 * lwIP 的 tcpip_thread 和 libcoro 的 offload worker 都是 FreeRTOS task。
 *
 * 验证: 裸机启动 + FreeRTOS 调度 + lwIP socket 栈 + libcoro lwip_select
 * loop (含 UDP 环回自唤醒) 端到端打通。
 */

/* 注意: 本文件只 include libcoro + lwIP, 不 include FreeRTOS.h。
 * 原因: libcoro 私有头 task.h 与 FreeRTOS 的 task.h 同名, 同一翻译单元里
 * 两者的 -I 路径无法共存 (谁在前另一个就拿错头)。FreeRTOS bootstrap
 * (main/xTaskCreate) 拆到 boot_fr.c, 本文件只暴露 run_echo_loop()。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <libcoro/libcoro.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"

#define MSG   "hello-freertos-lwip"
#define PORT  54321

static int g_listen_fd = -1;
static int g_client_fd = -1;
static int g_accepted_fd = -1;
static void *g_accept_out = NULL;
static int g_pass = 0;
static char g_srv_buf[64];
static char g_cli_buf[64];

static void on_server_sent(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r);
static void on_client_recv(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r);

static void on_server_recv(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)r;
    if (err != 0 || n == 0) { printf("server recv fail err=%d n=%lu\n", err, n); loop_stop(); return; }
    loop_post_send((void*)(intptr_t)g_accepted_fd, g_srv_buf, n, on_server_sent, NULL);
}
static void on_server_sent(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)n;(void)r;
    if (err != 0) { printf("server send fail err=%d\n", err); loop_stop(); }
}
static void on_accept(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)n;(void)r;
    if (err != 0) { printf("accept fail err=%d\n", err); loop_stop(); return; }
    g_accepted_fd = (int)(intptr_t)g_accept_out;
    loop_post_recv((void*)(intptr_t)g_accepted_fd, g_srv_buf, sizeof(g_srv_buf), on_server_recv, NULL);
}
static void on_client_sent(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)n;(void)r;
    if (err != 0) { printf("client send fail err=%d\n", err); loop_stop(); return; }
    loop_post_recv((void*)(intptr_t)g_client_fd, g_cli_buf, sizeof(g_cli_buf), on_client_recv, NULL);
}
static void on_client_recv(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)r;
    if (err != 0 || n == 0) { printf("client recv fail err=%d n=%lu\n", err, n); loop_stop(); return; }
    if (n == strlen(MSG) && memcmp(g_cli_buf, MSG, n) == 0) {
        g_pass = 1; printf("echo ok: got '%.*s'\n", (int)n, g_cli_buf);
    } else printf("echo mismatch: %lu bytes\n", n);
    loop_stop();
}
static void on_connect(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l;(void)ud;(void)t;(void)n;(void)r;
    if (err != 0) { printf("connect fail err=%d\n", err); loop_stop(); return; }
    loop_post_send((void*)(intptr_t)g_client_fd, MSG, strlen(MSG), on_client_sent, NULL);
}

task_t* task(echo_task) {
    gen_dec_vars();
    gen_begin(ctx);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = PP_HTONS(PORT);
    addr.sin_addr.s_addr = PP_HTONL(INADDR_LOOPBACK);

    g_listen_fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    lwip_setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (lwip_bind(g_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) { printf("bind fail\n"); gen_return(1); }
    if (lwip_listen(g_listen_fd, 4) < 0) { printf("listen fail\n"); gen_return(1); }
    loop_bind_handle((void*)(intptr_t)g_listen_fd);
    loop_accept_async((void*)(intptr_t)g_listen_fd, &g_accept_out, on_accept, NULL);

    g_client_fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    loop_bind_handle((void*)(intptr_t)g_client_fd);
    loop_connect_async((void*)(intptr_t)g_client_fd, (struct sockaddr*)&addr, sizeof(addr), on_connect, NULL);

    gen_end(0);
}

/* 由 boot_fr.c 的 FreeRTOS task 调用。返回后调 exit()。 */
int run_echo_loop(void) {
    printf("loop task: starting libcoro loop\n");
    task_t *t = echo_task();
    loop_run(t);      /* 首次 loop_get 会 tcpip_init + 挂 loopif */

    if (g_accepted_fd >= 0) lwip_close(g_accepted_fd);
    if (g_client_fd >= 0)   lwip_close(g_client_fd);
    if (g_listen_fd >= 0)   lwip_close(g_listen_fd);
    loop_destroy();

    printf("%s\n", g_pass ? "STAGE2A ECHO OK" : "STAGE2A ECHO FAIL");
    return g_pass ? 0 : 1;
}
