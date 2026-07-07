/*
 * lwip 后端的环回 echo 测试 (阶段 1)。
 *
 * 纯栈内 127.0.0.1, 不需要 tap/root。链路:
 *   server listen -> loop_accept_async
 *   client        -> loop_connect_async
 *   client send "hello" -> server recv -> server echo -> client recv 校验
 *
 * 真正压到 loop 后端的 accept/connect/send/recv 四条路径 + UDP 自唤醒
 * (每次 loop_post_* 都会 wake_self 打断 lwip_select)。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <libcoro/libcoro.h>
#include "lwip/sockets.h"
#include "lwip/inet.h"

#define MSG   "hello-lwip"
#define PORT  54321

static int g_listen_fd = -1;
static int g_client_fd = -1;
static int g_accepted_fd = -1;
static void *g_accept_out = NULL;   /* loop 把 accepted fd 写这里 */

static int g_pass = 0;

static char g_srv_buf[64];
static char g_cli_buf[64];

static void on_server_sent(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r);
static void on_client_recv(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r);

/* --- 服务端: 收到数据后原样 echo 回去 --- */
static void on_server_recv(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l; (void)ud; (void)t; (void)r;
    if (err != 0 || n == 0) { printf("server recv fail err=%d n=%lu\n", err, n); loop_stop(); return; }
    loop_post_send((void*)(intptr_t)g_accepted_fd, g_srv_buf, n, on_server_sent, NULL);
}

static void on_server_sent(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l; (void)ud; (void)t; (void)n; (void)r;
    if (err != 0) { printf("server send fail err=%d\n", err); loop_stop(); }
    /* echo 完成, 等客户端收 */
}

/* accept 完成: loop 把 accepted fd 写进 g_accept_out */
static void on_accept(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l; (void)ud; (void)t; (void)n; (void)r;
    if (err != 0) { printf("accept fail err=%d\n", err); loop_stop(); return; }
    g_accepted_fd = (int)(intptr_t)g_accept_out;
    loop_post_recv((void*)(intptr_t)g_accepted_fd, g_srv_buf, sizeof(g_srv_buf), on_server_recv, NULL);
}

/* --- 客户端 --- */
static void on_client_sent(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l; (void)ud; (void)t; (void)n; (void)r;
    if (err != 0) { printf("client send fail err=%d\n", err); loop_stop(); return; }
    loop_post_recv((void*)(intptr_t)g_client_fd, g_cli_buf, sizeof(g_cli_buf), on_client_recv, NULL);
}

static void on_client_recv(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l; (void)ud; (void)t; (void)r;
    if (err != 0 || n == 0) { printf("client recv fail err=%d n=%lu\n", err, n); loop_stop(); return; }
    if (n == strlen(MSG) && memcmp(g_cli_buf, MSG, n) == 0) {
        g_pass = 1;
        printf("echo ok: got '%.*s'\n", (int)n, g_cli_buf);
    } else {
        printf("echo mismatch: got %lu bytes\n", n);
    }
    loop_stop();
}

static void on_connect(loop_t *l, void *ud, loop_op_type_t t, int err, unsigned long n, recv_data_t *r) {
    (void)l; (void)ud; (void)t; (void)n; (void)r;
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

    /* 服务端 listen */
    g_listen_fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    lwip_setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (lwip_bind(g_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        printf("bind fail\n"); gen_return(1);
    }
    if (lwip_listen(g_listen_fd, 4) < 0) { printf("listen fail\n"); gen_return(1); }
    loop_bind_handle((void*)(intptr_t)g_listen_fd);
    loop_accept_async((void*)(intptr_t)g_listen_fd, &g_accept_out, on_accept, NULL);

    /* 客户端 connect */
    g_client_fd = lwip_socket(AF_INET, SOCK_STREAM, 0);
    loop_bind_handle((void*)(intptr_t)g_client_fd);
    loop_connect_async((void*)(intptr_t)g_client_fd, (struct sockaddr*)&addr, sizeof(addr), on_connect, NULL);

    gen_end(0);
}

int test_lwip_echo() {
    task_t *t = echo_task();
    loop_run(t);

    if (g_accepted_fd >= 0) lwip_close(g_accepted_fd);
    if (g_client_fd >= 0)   lwip_close(g_client_fd);
    if (g_listen_fd >= 0)   lwip_close(g_listen_fd);

    loop_destroy();
    return g_pass ? 0 : 1;
}
