/*
 * 阶段 2b: 裸机 (NO_SYS=1) getsockname 测试。
 *
 * bind 到 port 0 (让 lwIP 选临时端口), 再 getsockname 读回实际端口, 校验非 0。
 * 这是 http_server (port==0 临时端口) 依赖的路径, 之前 raw 后端 getsockname
 * 返 -1 未实现。同时验固定端口 bind 后 getsockname 读回同端口。
 *
 * bootstrap 复用 boot_raw.c。
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "loop.h"

#include "lwip/def.h"

static int g_pass = 0;

int run_echo_loop(void) {   /* 复用 boot_raw.c 入口名 */
    printf("bare-metal getsockname (NO_SYS)\n");
    loop_get();

    int ok = 1;

    /* --- 固定端口: bind 后 getsockname 读回同端口 --- */
    {
        void *h = anet_raw_new_tcp();
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = PP_HTONS(55460);
        a.sin_addr.s_addr = PP_HTONL(INADDR_LOOPBACK);
        if (anet_raw_bind(h, (struct sockaddr*)&a, sizeof(a)) != 0) { printf("bind fixed fail\n"); ok = 0; }

        struct sockaddr_in got;
        int glen = sizeof(got);
        if (anet_raw_getsockname(h, (struct sockaddr*)&got, &glen) != 0) { printf("getsockname fixed fail\n"); ok = 0; }
        else {
            uint16_t port = PP_NTOHS(got.sin_port);
            printf("fixed: bound port=%u\n", port);
            if (port != 55460) { printf("fixed port mismatch\n"); ok = 0; }
        }
        anet_raw_close(h);
    }

    /* --- 临时端口: bind port 0, getsockname 读回非 0 端口 --- */
    {
        void *h = anet_raw_new_tcp();
        struct sockaddr_in a;
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_port = 0;                      /* 让 lwIP 选 */
        a.sin_addr.s_addr = PP_HTONL(INADDR_LOOPBACK);
        if (anet_raw_bind(h, (struct sockaddr*)&a, sizeof(a)) != 0) { printf("bind eph fail\n"); ok = 0; }

        struct sockaddr_in got;
        int glen = sizeof(got);
        if (anet_raw_getsockname(h, (struct sockaddr*)&got, &glen) != 0) { printf("getsockname eph fail\n"); ok = 0; }
        else {
            uint16_t port = PP_NTOHS(got.sin_port);
            printf("ephemeral: bound port=%u\n", port);
            if (port == 0) { printf("ephemeral port still 0\n"); ok = 0; }
        }
        anet_raw_close(h);
    }

    loop_destroy();

    g_pass = ok;
    printf("%s\n", g_pass ? "STAGE2B GETSOCKNAME OK" : "STAGE2B GETSOCKNAME FAIL");
    return g_pass ? 0 : 1;
}
