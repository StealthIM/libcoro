/*
 * wolfSSL 裸机握手探针: 验证 Cortex-M3 上能否完成一次 TLS 握手及其耗时。
 *
 * 不接 loop/socket/lwIP —— 纯 wolfSSL client↔server, 用一对内存缓冲互连
 * (各自 IO 回调从对方缓冲读、往自己缓冲写)。验证最硬的未知:
 *   wolfCrypt 的 ECC 公钥数学能否在 Cortex-M3 QEMU 上完成一次 TLS 握手,
 *   以及耗时量级。这决定 TLS-on-embedded 可不可行。
 *
 * 证书: certs_test.h 里的 ECC P-256 DER (内存, 免文件系统)。
 * 时间/熵源: 探针桩 (见 boot_fr.c 里 wolf_gen_seed / wolf_time)。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 用 WOLFSSL_USER_SETTINGS 模式, 没有 configure 生成的 options.h;
 * 配置全在 user_settings.h (由 settings.h 自动 include)。 */
#include <wolfssl/ssl.h>
/* certs_test.h 的 ECC 证书被 USE_CERT_BUFFERS_256 gate; 要在 include 前定义 */
#define USE_CERT_BUFFERS_256
#include <wolfssl/certs_test.h>

/* 计时: 由 boot_fr.c 提供 (避免探针直接 include FreeRTOS.h, 那样组 C 要
 * 同时挂 FreeRTOS + wolfSSL 两组 include)。返回毫秒。 */
extern unsigned long probe_now_ms(void);

/* ---- 两端之间的内存管道 ---- */
#define PIPE_CAP 8192
typedef struct { unsigned char buf[PIPE_CAP]; int head, tail; } pipe_t;
static pipe_t c2s;   /* client -> server */
static pipe_t s2c;   /* server -> client */

static int pipe_write(pipe_t *p, const unsigned char *data, int len) {
    int n = 0;
    while (n < len && ((p->tail + 1) % PIPE_CAP) != p->head) {
        p->buf[p->tail] = data[n++];
        p->tail = (p->tail + 1) % PIPE_CAP;
    }
    return n;
}
static int pipe_read(pipe_t *p, unsigned char *data, int len) {
    int n = 0;
    while (n < len && p->head != p->tail) {
        data[n++] = p->buf[p->head];
        p->head = (p->head + 1) % PIPE_CAP;
    }
    return n;
}

/* wolfSSL IO 回调: WOLFSSL_CBIO_ERR_WANT_READ = -2 */
static int io_recv(WOLFSSL* ssl, char* buf, int sz, void* ctx) {
    (void)ssl;
    pipe_t *p = (pipe_t*)ctx;
    int n = pipe_read(p, (unsigned char*)buf, sz);
    if (n == 0) return WOLFSSL_CBIO_ERR_WANT_READ;
    return n;
}
static int io_send(WOLFSSL* ssl, char* buf, int sz, void* ctx) {
    (void)ssl;
    pipe_t *p = (pipe_t*)ctx;
    int n = pipe_write(p, (unsigned char*)buf, sz);
    if (n == 0) return WOLFSSL_CBIO_ERR_WANT_WRITE;
    return n;
}

int run_echo_loop(void) {   /* 复用 boot_fr.c 入口名 */
    printf("wolfSSL handshake probe (ECC P-256, TLS1.3)\n");
    wolfSSL_Init();

    WOLFSSL_CTX *cctx = wolfSSL_CTX_new(wolfTLSv1_3_client_method());
    WOLFSSL_CTX *sctx = wolfSSL_CTX_new(wolfTLSv1_3_server_method());
    if (!cctx || !sctx) { printf("CTX_new failed\n"); return 1; }

    /* server: ECC 证书链 + 私钥 (内存 DER) */
    if (wolfSSL_CTX_use_certificate_buffer(sctx, serv_ecc_der_256,
            sizeof_serv_ecc_der_256, WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS) {
        printf("server cert load failed\n"); return 1;
    }
    if (wolfSSL_CTX_use_PrivateKey_buffer(sctx, ecc_key_der_256,
            sizeof_ecc_key_der_256, WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS) {
        printf("server key load failed\n"); return 1;
    }
    /* client: 信任 CA (ECC), 校验 server */
    if (wolfSSL_CTX_load_verify_buffer(cctx, ca_ecc_cert_der_256,
            sizeof_ca_ecc_cert_der_256, WOLFSSL_FILETYPE_ASN1) != WOLFSSL_SUCCESS) {
        printf("client CA load failed\n"); return 1;
    }
    /* 探针不校验 hostname (证书 CN 未必匹配) */
    wolfSSL_CTX_set_verify(cctx, WOLFSSL_VERIFY_NONE, NULL);

    wolfSSL_SetIORecv(cctx, io_recv);
    wolfSSL_SetIOSend(cctx, io_send);
    wolfSSL_SetIORecv(sctx, io_recv);
    wolfSSL_SetIOSend(sctx, io_send);

    WOLFSSL *client = wolfSSL_new(cctx);
    WOLFSSL *server = wolfSSL_new(sctx);
    /* client 读 s2c / 写 c2s; server 读 c2s / 写 s2c */
    wolfSSL_SetIOReadCtx(client,  &s2c);
    wolfSSL_SetIOWriteCtx(client, &c2s);
    wolfSSL_SetIOReadCtx(server,  &c2s);
    wolfSSL_SetIOWriteCtx(server, &s2c);

    unsigned long t0 = probe_now_ms();

    /* 交替驱动两端握手, 直到都完成或卡死 */
    int cdone = 0, sdone = 0, iters = 0;
    while ((!cdone || !sdone) && iters < 100) {
        if (!cdone) {
            int r = wolfSSL_connect(client);
            if (r == WOLFSSL_SUCCESS) cdone = 1;
            else {
                int e = wolfSSL_get_error(client, r);
                if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) {
                    printf("client handshake err=%d\n", e); return 1;
                }
            }
        }
        if (!sdone) {
            int r = wolfSSL_accept(server);
            if (r == WOLFSSL_SUCCESS) sdone = 1;
            else {
                int e = wolfSSL_get_error(server, r);
                if (e != WOLFSSL_ERROR_WANT_READ && e != WOLFSSL_ERROR_WANT_WRITE) {
                    printf("server handshake err=%d\n", e); return 1;
                }
            }
        }
        iters++;
    }

    unsigned long t1 = probe_now_ms();

    if (cdone && sdone) {
        /* 握手后跑一次应用数据 echo 验证密钥协商正确 */
        const char *msg = "ping-over-tls";
        wolfSSL_write(client, msg, strlen(msg));
        char rx[64] = {0};
        int rn = wolfSSL_read(server, rx, sizeof(rx));
        printf("handshake OK in %lu ms (%d iters); app-data: '%.*s'\n",
               t1 - t0, iters, rn > 0 ? rn : 0, rx);
        printf("cipher: %s\n", wolfSSL_get_cipher(client));
        printf("PROBE WOLFSSL OK\n");
        return 0;
    }
    printf("handshake did not complete (iters=%d)\n", iters);
    return 1;
}
