#pragma once

/*
 * 平台无关的线程原语抽象 (PAL: Platform Abstraction Layer)。
 *
 * loop 后端 (epoll/select/win) 按平台分开编译;线程原语同理:
 * POSIX 走 pthread,Windows 走 Win32 原语。上层 (offload 线程池)
 * 只依赖本头文件,不直接接触 pthread_* / Win32 API。
 *
 * 句柄一律为不透明指针,由各平台实现内部堆分配,避免在公共头文件里
 * 泄漏 pthread_t / CRITICAL_SECTION 等平台类型。
 */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pal_thread_s pal_thread_t;
typedef struct pal_mutex_s  pal_mutex_t;
typedef struct pal_cond_s   pal_cond_t;

/* 线程。返回 0 成功;fn 的返回值当前不被回收 (worker 循环自行退出)。 */
int  pal_thread_create(pal_thread_t **out, void *(*fn)(void *), void *arg);
void pal_thread_join(pal_thread_t *t);

/* 互斥锁。 */
int  pal_mutex_init(pal_mutex_t **out);
void pal_mutex_lock(pal_mutex_t *m);
void pal_mutex_unlock(pal_mutex_t *m);
void pal_mutex_destroy(pal_mutex_t *m);

/* 条件变量。pal_cond_wait 调用时必须已持有 m,返回时仍持有 m。 */
int  pal_cond_init(pal_cond_t **out);
void pal_cond_wait(pal_cond_t *c, pal_mutex_t *m);
void pal_cond_signal(pal_cond_t *c);
void pal_cond_broadcast(pal_cond_t *c);
void pal_cond_destroy(pal_cond_t *c);

#ifdef __cplusplus
}
#endif
