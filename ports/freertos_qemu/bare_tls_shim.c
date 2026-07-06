/*
 * 裸机 HTTPS (真 wolfssl.c) 缺口垫片 (阶段 2b)。
 *
 * 裸机用真 wolfssl.c, 但它在 NO_FILESYSTEM 下 guard 掉了两组符号, 而
 * http_server.c / stream.c 链接期仍引用:
 *   - async_ssl_create_server(cert_path,key_path): file-based, 裸机走
 *     use_tls_mem 的 async_ssl_create_server_mem, 这个 file 版永不被调。
 *   - sync_ssl_*: stream.c 的 sync 段 (裸机只用 async stream, 不走)。
 * 这里补这几个符号: file-server 返 NULL (裸机不该走文件路径), sync 全 abort
 * (被调到即 bug)。真 async TLS 由 wolfssl.c 提供, 不在此垫。
 */

#include "tls.h"
#include <stdlib.h>
#include <stdio.h>

async_ssl_t* async_ssl_create_server(const char *cert_path, const char *key_path) {
    (void)cert_path; (void)key_path;
    return NULL;   /* 裸机走 use_tls_mem / async_ssl_create_server_mem */
}

/* 客户端 file 版 (ws.c 的 wss 分支引用): 裸机 wss 客户端应走 async_ssl_create_mem
 * (内存 CA)。这个 file 版在明文 ws / 无 CA 场景不被调, 返 NULL。 */
async_ssl_t* async_ssl_create(async_ssl_role_t role, const char *hostname) {
    (void)role; (void)hostname;
    return NULL;
}

static void shim_abort(const char *fn) {
    printf("bare tls shim: %s called (bug: 裸机不走 sync SSL)\n", fn);
    abort();
}

sync_ssl_t* sync_ssl_create(sync_ssl_role_t role, const char *hostname) { (void)role; (void)hostname; return NULL; }
void sync_ssl_attach_socket(sync_ssl_t *ssl, anet_palsock_t sock) { (void)ssl; (void)sock; shim_abort("sync_ssl_attach_socket"); }
int  sync_ssl_handshake(sync_ssl_t *ssl) { (void)ssl; shim_abort("sync_ssl_handshake"); return -1; }
int  sync_ssl_read(sync_ssl_t *ssl, void *buf, size_t len) { (void)ssl; (void)buf; (void)len; shim_abort("sync_ssl_read"); return -1; }
int  sync_ssl_write(sync_ssl_t *ssl, const void *buf, size_t len) { (void)ssl; (void)buf; (void)len; shim_abort("sync_ssl_write"); return -1; }
void sync_ssl_close(sync_ssl_t *ssl) { (void)ssl; shim_abort("sync_ssl_close"); }
void sync_ssl_destroy(sync_ssl_t *ssl) { (void)ssl; shim_abort("sync_ssl_destroy"); }
int  sync_ssl_is_closed(sync_ssl_t *ssl) { (void)ssl; return 1; }
