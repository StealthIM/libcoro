#pragma once
/*
 * libcoro 内部 loop hook (不安装, 仅库自身编译用)。
 *
 * 这些是 loop 后端 (linux_epoll/select、windows、lwip_*) 与 offload 线程池
 * 之间的集成点, 不属于用户 API:
 *   - loop_wake: 从 worker 线程唤醒阻塞的 loop (offload 完成后 wake 主线程)。
 *   - loop_get_offload: 取该 loop 的 offload 池句柄 (仅主线程)。
 */

#include <libcoro/loop.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 线程安全地唤醒 loop (可从 worker 线程调用)。 */
void loop_wake(loop_t *loop);

/* 取该 loop 的 offload 线程池句柄 (仅主线程)。 */
struct offload_pool_s *loop_get_offload(loop_t *loop);

#ifdef __cplusplus
}
#endif
