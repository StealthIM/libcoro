#include "pal_thread.h"

#include <windows.h>
#include <process.h>
#include <stdlib.h>

struct pal_thread_s { HANDLE h; void *(*fn)(void *); void *arg; };
struct pal_mutex_s  { CRITICAL_SECTION cs; };
struct pal_cond_s   { CONDITION_VARIABLE cv; };

static unsigned __stdcall win_thread_trampoline(void *p) {
    pal_thread_t *t = (pal_thread_t *)p;
    t->fn(t->arg);
    return 0;
}

int pal_thread_create(pal_thread_t **out, void *(*fn)(void *), void *arg) {
    pal_thread_t *t = malloc(sizeof(*t));
    if (!t) return -1;
    t->fn = fn;
    t->arg = arg;
    uintptr_t h = _beginthreadex(NULL, 0, win_thread_trampoline, t, 0, NULL);
    if (h == 0) {
        free(t);
        return -1;
    }
    t->h = (HANDLE)h;
    *out = t;
    return 0;
}

void pal_thread_join(pal_thread_t *t) {
    if (!t) return;
    WaitForSingleObject(t->h, INFINITE);
    CloseHandle(t->h);
    free(t);
}

int pal_mutex_init(pal_mutex_t **out) {
    pal_mutex_t *m = malloc(sizeof(*m));
    if (!m) return -1;
    InitializeCriticalSection(&m->cs);
    *out = m;
    return 0;
}

void pal_mutex_lock(pal_mutex_t *m)   { EnterCriticalSection(&m->cs); }
void pal_mutex_unlock(pal_mutex_t *m) { LeaveCriticalSection(&m->cs); }

void pal_mutex_destroy(pal_mutex_t *m) {
    if (!m) return;
    DeleteCriticalSection(&m->cs);
    free(m);
}

int pal_cond_init(pal_cond_t **out) {
    pal_cond_t *c = malloc(sizeof(*c));
    if (!c) return -1;
    InitializeConditionVariable(&c->cv);
    *out = c;
    return 0;
}

void pal_cond_wait(pal_cond_t *c, pal_mutex_t *m) {
    SleepConditionVariableCS(&c->cv, &m->cs, INFINITE);
}
void pal_cond_signal(pal_cond_t *c)    { WakeConditionVariable(&c->cv); }
void pal_cond_broadcast(pal_cond_t *c) { WakeAllConditionVariable(&c->cv); }

void pal_cond_destroy(pal_cond_t *c) {
    /* Win32 CONDITION_VARIABLE 无需显式销毁 */
    if (c) free(c);
}
