#include "pal_thread.h"

#include <pthread.h>
#include <stdlib.h>

struct pal_thread_s { pthread_t t; };
struct pal_mutex_s  { pthread_mutex_t m; };
struct pal_cond_s   { pthread_cond_t c; };

int pal_thread_create(pal_thread_t **out, void *(*fn)(void *), void *arg) {
    pal_thread_t *t = malloc(sizeof(*t));
    if (!t) return -1;
    if (pthread_create(&t->t, NULL, fn, arg) != 0) {
        free(t);
        return -1;
    }
    *out = t;
    return 0;
}

void pal_thread_join(pal_thread_t *t) {
    if (!t) return;
    pthread_join(t->t, NULL);
    free(t);
}

int pal_mutex_init(pal_mutex_t **out) {
    pal_mutex_t *m = malloc(sizeof(*m));
    if (!m) return -1;
    if (pthread_mutex_init(&m->m, NULL) != 0) {
        free(m);
        return -1;
    }
    *out = m;
    return 0;
}

void pal_mutex_lock(pal_mutex_t *m)    { pthread_mutex_lock(&m->m); }
void pal_mutex_unlock(pal_mutex_t *m)  { pthread_mutex_unlock(&m->m); }

void pal_mutex_destroy(pal_mutex_t *m) {
    if (!m) return;
    pthread_mutex_destroy(&m->m);
    free(m);
}

int pal_cond_init(pal_cond_t **out) {
    pal_cond_t *c = malloc(sizeof(*c));
    if (!c) return -1;
    if (pthread_cond_init(&c->c, NULL) != 0) {
        free(c);
        return -1;
    }
    *out = c;
    return 0;
}

void pal_cond_wait(pal_cond_t *c, pal_mutex_t *m) { pthread_cond_wait(&c->c, &m->m); }
void pal_cond_signal(pal_cond_t *c)               { pthread_cond_signal(&c->c); }
void pal_cond_broadcast(pal_cond_t *c)            { pthread_cond_broadcast(&c->c); }

void pal_cond_destroy(pal_cond_t *c) {
    if (!c) return;
    pthread_cond_destroy(&c->c);
    free(c);
}
