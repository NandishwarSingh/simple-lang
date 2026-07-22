#include <stdio.h>
#include <pthread.h>

typedef struct {
    pthread_mutex_t m;
    pthread_cond_t nf, ne;
    long long v;
    int count;
} Slot;

void slot_init(Slot* s) {
    pthread_mutex_init(&s->m, 0);
    pthread_cond_init(&s->nf, 0);
    pthread_cond_init(&s->ne, 0);
    s->count = 0;
}

void slot_send(Slot* s, long long v) {
    pthread_mutex_lock(&s->m);
    while (s->count == 1) pthread_cond_wait(&s->nf, &s->m);
    s->v = v;
    s->count = 1;
    pthread_cond_signal(&s->ne);
    pthread_mutex_unlock(&s->m);
}

long long slot_recv(Slot* s) {
    pthread_mutex_lock(&s->m);
    while (s->count == 0) pthread_cond_wait(&s->ne, &s->m);
    long long v = s->v;
    s->count = 0;
    pthread_cond_signal(&s->nf);
    pthread_mutex_unlock(&s->m);
    return v;
}

Slot ping, pong;

void* ponger(void* arg) {
    (void)arg;
    for (;;) {
        long long v = slot_recv(&ping);
        if (v == -1) return 0;
        slot_send(&pong, v + 1);
    }
}

int main(void) {
    slot_init(&ping);
    slot_init(&pong);
    pthread_t t;
    pthread_create(&t, 0, ponger, 0);
    long long total = 0;
    for (long long i = 0; i < 100000; i++) {
        slot_send(&ping, i);
        total += slot_recv(&pong);
    }
    slot_send(&ping, -1);
    pthread_join(t, 0);
    printf("%lld\n", total);
    return 0;
}
