/*
 * pal_thread 的 FreeRTOS 实现 (阶段 2a)。
 *
 * FreeRTOS 缺两样 pthread 有的东西, 这里补上:
 *   - join: FreeRTOS task 无 join。trampoline 在退出前 give 一个信号量,
 *     join 方 take 它即知 task 已跑完。
 *   - 条件变量: 用计数信号量 + waiter 计数模拟。offload 的 signal/broadcast/
 *     wait 都在持 mutex 时调用 (见 offload.c), 所以 waiter 计数天然被那把
 *     mutex 保护, 不需要额外锁。计数信号量保证不丢唤醒: 即便 signal 发生在
 *     waiter "waiters++ 解锁" 与 "take" 之间, give 会留在计数里, 后续 take
 *     立即成功。
 *
 * fn 签名是 pthread 风格 void*(*)(void*); FreeRTOS task 是 void(*)(void*)。
 * trampoline 负责适配, 并在 fn 返回后 vTaskDelete(NULL) (FreeRTOS task 不能
 * 直接 return)。
 */

#include "pal_thread.h"

#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"

#include <stdlib.h>

/* offload worker 栈 (字数): 要跑 lwip_getaddrinfo 等, 给宽裕些。 */
#define PAL_THREAD_STACK_WORDS   2048
#define PAL_THREAD_PRIORITY      ( tskIDLE_PRIORITY + 2 )

struct pal_thread_s {
    TaskHandle_t       task;
    SemaphoreHandle_t  done;     /* trampoline 退出前 give */
    void *(*fn)(void *);
    void *arg;
};

struct pal_mutex_s {
    SemaphoreHandle_t m;         /* xSemaphoreCreateMutex */
};

struct pal_cond_s {
    SemaphoreHandle_t sem;       /* 计数信号量 */
    int waiters;                 /* 只在持关联 mutex 时访问 */
};

/* -------------------- thread -------------------- */

static void thread_trampoline(void *param) {
    pal_thread_t *t = (pal_thread_t *)param;
    t->fn(t->arg);
    xSemaphoreGive(t->done);     /* 通知 join 方: 已跑完 */
    vTaskDelete(NULL);           /* task 不能 return; 自删 (栈/TCB 由 idle 回收) */
}

int pal_thread_create(pal_thread_t **out, void *(*fn)(void *), void *arg) {
    pal_thread_t *t = malloc(sizeof(*t));
    if (!t) return -1;
    t->fn = fn;
    t->arg = arg;
    t->done = xSemaphoreCreateBinary();
    if (!t->done) { free(t); return -1; }

    if (xTaskCreate(thread_trampoline, "pal_thr", PAL_THREAD_STACK_WORDS,
                    t, PAL_THREAD_PRIORITY, &t->task) != pdPASS) {
        vSemaphoreDelete(t->done);
        free(t);
        return -1;
    }
    *out = t;
    return 0;
}

void pal_thread_join(pal_thread_t *t) {
    if (!t) return;
    /* 等 trampoline 跑完 fn。give 发生在 vTaskDelete(NULL) 之前, 而后者只碰
     * task 自己的 TCB, 不碰这里将释放的 t/done, 所以之后 free 是安全的。 */
    xSemaphoreTake(t->done, portMAX_DELAY);
    vSemaphoreDelete(t->done);
    free(t);
}

/* -------------------- mutex -------------------- */

int pal_mutex_init(pal_mutex_t **out) {
    pal_mutex_t *m = malloc(sizeof(*m));
    if (!m) return -1;
    m->m = xSemaphoreCreateMutex();
    if (!m->m) { free(m); return -1; }
    *out = m;
    return 0;
}

void pal_mutex_lock(pal_mutex_t *m)   { xSemaphoreTake(m->m, portMAX_DELAY); }
void pal_mutex_unlock(pal_mutex_t *m) { xSemaphoreGive(m->m); }

void pal_mutex_destroy(pal_mutex_t *m) {
    if (!m) return;
    vSemaphoreDelete(m->m);
    free(m);
}

/* -------------------- cond -------------------- */

int pal_cond_init(pal_cond_t **out) {
    pal_cond_t *c = malloc(sizeof(*c));
    if (!c) return -1;
    /* 计数信号量, 初值 0; 上限给大, 够覆盖并发 waiter 数。 */
    c->sem = xSemaphoreCreateCounting(255, 0);
    if (!c->sem) { free(c); return -1; }
    c->waiters = 0;
    *out = c;
    return 0;
}

/* 调用时必须持 m; 返回时仍持 m (与 pthread_cond_wait 一致)。 */
void pal_cond_wait(pal_cond_t *c, pal_mutex_t *m) {
    c->waiters++;                       /* 在锁内: 安全 */
    xSemaphoreGive(m->m);               /* 放锁 */
    xSemaphoreTake(c->sem, portMAX_DELAY);
    xSemaphoreTake(m->m, portMAX_DELAY);/* 重新拿锁 */
}

void pal_cond_signal(pal_cond_t *c) {
    if (c->waiters > 0) {               /* 在锁内调用 (offload 保证) */
        c->waiters--;
        xSemaphoreGive(c->sem);
    }
}

void pal_cond_broadcast(pal_cond_t *c) {
    while (c->waiters > 0) {
        c->waiters--;
        xSemaphoreGive(c->sem);
    }
}

void pal_cond_destroy(pal_cond_t *c) {
    if (!c) return;
    vSemaphoreDelete(c->sem);
    free(c);
}
