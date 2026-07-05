#pragma once

/*
 * 通用 offload:把阻塞调用丢到后台 worker 线程,完成后在 loop 主线程
 * resolve 对应的 future。第一个用例是异步 DNS (阻塞 getaddrinfo),
 * 但机制本身与 DNS 无关。
 *
 * 线程安全模型:
 *   - worker 线程只跑 fn、把结果塞进加锁的完成队列、调 loop_wake 唤醒 loop。
 *   - worker 绝不触碰 future / soon 队列 / loop 内部结构 (它们非线程安全)。
 *   - future_done 只在 loop 主线程被唤醒后、经 offload_drain_completions 调用。
 */

#include "loop.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct future_s future_t;
typedef struct offload_pool_s offload_pool_t;

/* 由 loop_create/destroy 调用,和 loop 生命周期绑定。 */
offload_pool_t *offload_pool_create(loop_t *loop);
void            offload_pool_destroy(offload_pool_t *pool);

/*
 * 提交阻塞任务。fn(arg) 在某个 worker 线程执行,其返回值 (void*) 成为
 * future 的 result。调用方在 loop 主线程 gen_yield 返回的 future 即可。
 * 失败返回 NULL。
 */
future_t *loop_run_in_thread(void *(*fn)(void *arg), void *arg);

/* loop 主线程被唤醒后消费完成队列,对每个完成 job 调 future_done。 */
void offload_drain_completions(offload_pool_t *pool);

/* loop 存活判定:是否还有 job 在提交/执行/待排空。 */
int offload_has_pending(offload_pool_t *pool);

#ifdef __cplusplus
}
#endif
