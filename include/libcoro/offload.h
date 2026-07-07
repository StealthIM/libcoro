#pragma once

/*
 * 通用 offload (公开面):把阻塞调用丢到后台 worker 线程,完成后在 loop 主线程
 * resolve 对应的 future。用户 API 只有 loop_run_in_thread —— 线程池的生命周期
 * 与排空由各 loop 后端内部驱动 (见 internal/offload_internal.h)。
 *
 * 典型用例:把阻塞的 getaddrinfo offload 出事件循环线程做异步 DNS,
 * 但机制本身与 DNS 无关。
 */

#include <libcoro/loop.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct future_s future_t;

/*
 * 提交阻塞任务。fn(arg) 在某个 worker 线程执行,其返回值 (void*) 成为
 * future 的 result。调用方在 loop 主线程 gen_yield 返回的 future 即可。
 * 失败返回 NULL (如无 offload 能力的构建 —— LIBCORO_HAVE_OFFLOAD=OFF)。
 */
future_t *loop_run_in_thread(void *(*fn)(void *arg), void *arg);

#ifdef __cplusplus
}
#endif
