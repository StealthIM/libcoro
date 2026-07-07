#pragma once
/*
 * 裸机 raw lwIP socket setup (内部, 不安装; lwip_raw.c 实现)。
 *
 * NO_SYS=1 下没有 lwip_socket()/fd; 这几个函数建/配 raw_conn_t 包装, 返回它
 * 作 loop 的 void* handle。只做非阻塞 setup, 不做 sync IO (裸机决策)。
 * connect/recv/send/accept 走 loop.h 的异步 op API, handle 传这里返回的指针。
 *
 * 仅在 LIBCORO_LWIP_RAW 后端下有意义。asyncweb 的 pal_socket/lwip_raw.c 与
 * QEMU 裸机测试通过本头消费这些符号。
 */

#include <libcoro/loop.h>

#ifdef __cplusplus
extern "C" {
#endif

void *anet_raw_new_tcp(void);
int   anet_raw_bind(void *handle, const struct sockaddr *addr, int addrlen);
int   anet_raw_listen(void *handle, int backlog);
void  anet_raw_close(void *handle);
/* 读 raw_conn 的本地绑定地址 (bind 后, 含 port==0 时内核选的临时端口)。成功返 0。 */
int   anet_raw_getsockname(void *handle, struct sockaddr *addr, int *addrlen);

#ifdef __cplusplus
}
#endif
