#pragma once
/*
 * 裸机 raw lwIP 后端的 socket setup 入口 (阶段 2b)。
 *
 * NO_SYS=1 下没有 lwip_socket()/fd; 这几个函数在 lwip_raw.c 里建/配 raw_conn_t
 * 包装并返回它作 loop 的 void* handle。只做非阻塞 setup, 不做 sync IO
 * (裸机决策: 不提供 sync pal_socket)。connect/recv/send/accept 走 loop.h 的
 * 异步 op API, handle 传下面返回的指针。
 */

#include <stdint.h>

struct sockaddr;

/* 建一个 TCP raw_conn (tcp_new + 挂回调)。失败返 NULL。 */
void *anet_raw_new_tcp(void);

/* bind / listen (仅 IPv4)。成功返 0。listen 会替换内部 pcb 为 listen pcb。 */
int   anet_raw_bind(void *handle, const struct sockaddr *addr, int addrlen);
int   anet_raw_listen(void *handle, int backlog);

/* 关闭并释放 raw_conn (连带释放 backlog 里未取的连接 + rx pbuf)。 */
void  anet_raw_close(void *handle);
