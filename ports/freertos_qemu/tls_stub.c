/*
 * 明文裸机 HTTP server 测试用的 TLS 桩 (阶段 2b)。
 *
 * http_server.c / stream.c 在链接期引用 async_ssl_* 符号 (TLS 路径用), 但明文
 * 服务端从不创建 SSL 会话, 运行期不会调到。为不把整个 wolfSSL 链进明文测试
 * (省 flash), 这里给一组桩: 创建类返回 NULL / 其余 abort (被调到即 bug)。
 *
 * 裸机 TLS 集成是后续单独步骤, 那时换成真的 wolfssl.c (见 2a-TLS 接法)。
 */

#include "tls.h"
#include <stdlib.h>
#include <stdio.h>

static void tls_stub_unreachable(const char *fn) {
    printf("tls stub: %s called on plaintext path (bug)\n", fn);
    abort();
}

async_ssl_t* async_ssl_create_server(const char *cert_path, const char *key_path) {
    (void)cert_path; (void)key_path;
    return NULL;   /* 明文 server 不该走到 TLS 分支; 返回 NULL 让上层报错 */
}

async_ssl_t* async_ssl_create_server_mem(const unsigned char *cert, int cert_len,
                                         const unsigned char *key, int key_len, int is_der) {
    (void)cert; (void)cert_len; (void)key; (void)key_len; (void)is_der;
    return NULL;   /* 明文 server 不走 TLS 分支 */
}

async_ssl_t* async_ssl_create_mem(async_ssl_role_t role, const char *hostname,
                                  const unsigned char *ca, int ca_len, int is_der) {
    (void)role; (void)hostname; (void)ca; (void)ca_len; (void)is_der;
    return NULL;
}

void    async_ssl_attach_socket(async_ssl_t *ssl, async_socket_t *sock) { (void)ssl; (void)sock; tls_stub_unreachable("attach_socket"); }
task_t* async_ssl_handshake(async_ssl_t *ssl) { (void)ssl; tls_stub_unreachable("handshake"); return NULL; }
task_t* async_ssl_read(async_ssl_t *ssl, void *buf, size_t len) { (void)ssl; (void)buf; (void)len; tls_stub_unreachable("read"); return NULL; }
task_t* async_ssl_write(async_ssl_t *ssl, const void *buf, size_t len) { (void)ssl; (void)buf; (void)len; tls_stub_unreachable("write"); return NULL; }
task_t* async_ssl_close(async_ssl_t *ssl) { (void)ssl; tls_stub_unreachable("close"); return NULL; }
void    async_ssl_destroy(async_ssl_t *ssl) { (void)ssl; tls_stub_unreachable("destroy"); }

/* ---- sync SSL 桩: stream.c 的 sync 段引用 (明文/裸机不走) ---- */
sync_ssl_t* sync_ssl_create(sync_ssl_role_t role, const char *hostname) { (void)role; (void)hostname; return NULL; }
void sync_ssl_attach_socket(sync_ssl_t *ssl, anet_palsock_t sock) { (void)ssl; (void)sock; tls_stub_unreachable("sync attach"); }
int  sync_ssl_handshake(sync_ssl_t *ssl) { (void)ssl; tls_stub_unreachable("sync handshake"); return -1; }
int  sync_ssl_read(sync_ssl_t *ssl, void *buf, size_t len) { (void)ssl; (void)buf; (void)len; tls_stub_unreachable("sync read"); return -1; }
int  sync_ssl_write(sync_ssl_t *ssl, const void *buf, size_t len) { (void)ssl; (void)buf; (void)len; tls_stub_unreachable("sync write"); return -1; }
void sync_ssl_close(sync_ssl_t *ssl) { (void)ssl; tls_stub_unreachable("sync close"); }
void sync_ssl_destroy(sync_ssl_t *ssl) { (void)ssl; tls_stub_unreachable("sync destroy"); }
int  sync_ssl_is_closed(sync_ssl_t *ssl) { (void)ssl; return 1; }
