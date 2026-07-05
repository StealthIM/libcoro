#include "offload.h"
#include "pal_thread.h"
#include "future.h"

#include <stdlib.h>

#define OFFLOAD_NUM_WORKERS 4

typedef struct offload_job_s {
    void *(*fn)(void *);
    void *arg;
    void *result;          /* worker 跑完 fn 后填入 */
    future_t *future;      /* 主线程建、主线程 resolve */
    struct offload_job_s *next;
} offload_job_t;

struct offload_pool_s {
    loop_t *loop;

    pal_mutex_t *mtx;
    pal_cond_t  *cv;       /* worker 等待提交队列非空 */

    offload_job_t *submit_head, *submit_tail;   /* 待执行 */
    offload_job_t *done_head,   *done_tail;     /* 已完成待排空 */

    int pending;           /* 提交+执行+待排空 总数,存活判定用 */
    int shutdown;

    pal_thread_t *workers[OFFLOAD_NUM_WORKERS];
    int num_workers;
};

static void submit_push(offload_pool_t *p, offload_job_t *j) {
    j->next = NULL;
    if (!p->submit_tail) p->submit_head = p->submit_tail = j;
    else { p->submit_tail->next = j; p->submit_tail = j; }
}

static offload_job_t *submit_pop(offload_pool_t *p) {
    offload_job_t *j = p->submit_head;
    if (!j) return NULL;
    p->submit_head = j->next;
    if (!p->submit_head) p->submit_tail = NULL;
    return j;
}

static void done_push(offload_pool_t *p, offload_job_t *j) {
    j->next = NULL;
    if (!p->done_tail) p->done_head = p->done_tail = j;
    else { p->done_tail->next = j; p->done_tail = j; }
}

static offload_job_t *done_pop(offload_pool_t *p) {
    offload_job_t *j = p->done_head;
    if (!j) return NULL;
    p->done_head = j->next;
    if (!p->done_head) p->done_tail = NULL;
    return j;
}

static void *worker_main(void *arg) {
    offload_pool_t *p = (offload_pool_t *)arg;
    for (;;) {
        pal_mutex_lock(p->mtx);
        while (!p->submit_head && !p->shutdown) {
            pal_cond_wait(p->cv, p->mtx);
        }
        if (p->shutdown && !p->submit_head) {
            pal_mutex_unlock(p->mtx);
            return NULL;
        }
        offload_job_t *j = submit_pop(p);
        pal_mutex_unlock(p->mtx);

        /* 阻塞调用在锁外执行 */
        j->result = j->fn(j->arg);

        pal_mutex_lock(p->mtx);
        done_push(p, j);
        pal_mutex_unlock(p->mtx);

        /* 唤醒 loop 主线程去排空完成队列 (eventfd 等本身线程安全) */
        loop_wake(p->loop);
    }
}

offload_pool_t *offload_pool_create(loop_t *loop) {
    offload_pool_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->loop = loop;
    if (pal_mutex_init(&p->mtx) != 0) { free(p); return NULL; }
    if (pal_cond_init(&p->cv) != 0)  { pal_mutex_destroy(p->mtx); free(p); return NULL; }

    for (int i = 0; i < OFFLOAD_NUM_WORKERS; ++i) {
        if (pal_thread_create(&p->workers[i], worker_main, p) != 0) break;
        p->num_workers++;
    }
    return p;
}

void offload_pool_destroy(offload_pool_t *pool) {
    if (!pool) return;

    pal_mutex_lock(pool->mtx);
    pool->shutdown = 1;
    pal_cond_broadcast(pool->cv);
    pal_mutex_unlock(pool->mtx);

    for (int i = 0; i < pool->num_workers; ++i) {
        pal_thread_join(pool->workers[i]);
    }

    /* worker 全部退出后无并发,排空残余:未执行的 submit 和已完成的 done。
       对应 future 由各自 awaiting 的 task 驱动持有,这里只 reject+释放 job。 */
    offload_job_t *j;
    while ((j = submit_pop(pool)) != NULL) {
        if (j->future) future_reject(j->future, NULL);
        free(j);
    }
    while ((j = done_pop(pool)) != NULL) {
        if (j->future) future_done(j->future, j->result);
        free(j);
    }

    pal_cond_destroy(pool->cv);
    pal_mutex_destroy(pool->mtx);
    free(pool);
}

future_t *loop_run_in_thread(void *(*fn)(void *arg), void *arg) {
    if (!fn) return NULL;
    loop_t *loop = loop_get();
    offload_pool_t *p = loop_get_offload(loop);
    if (!p) return NULL;

    offload_job_t *j = calloc(1, sizeof(*j));
    if (!j) return NULL;
    j->fn = fn;
    j->arg = arg;
    j->future = future_create();
    if (!j->future) { free(j); return NULL; }

    pal_mutex_lock(p->mtx);
    submit_push(p, j);
    p->pending++;
    pal_cond_signal(p->cv);
    pal_mutex_unlock(p->mtx);

    return j->future;
}

void offload_drain_completions(offload_pool_t *pool) {
    if (!pool) return;
    for (;;) {
        pal_mutex_lock(pool->mtx);
        offload_job_t *j = done_pop(pool);
        pal_mutex_unlock(pool->mtx);
        if (!j) break;

        future_done(j->future, j->result);

        pal_mutex_lock(pool->mtx);
        pool->pending--;
        pal_mutex_unlock(pool->mtx);

        free(j);
    }
}

int offload_has_pending(offload_pool_t *pool) {
    if (!pool) return 0;
    pal_mutex_lock(pool->mtx);
    int n = pool->pending;
    pal_mutex_unlock(pool->mtx);
    return n;
}
