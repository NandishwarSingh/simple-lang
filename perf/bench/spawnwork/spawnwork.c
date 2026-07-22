#include <stdio.h>
#include <pthread.h>

long long steps(long long n) {
    long long s = 0;
    while (n != 1) {
        if (n % 2 == 0) n = n / 2;
        else n = 3 * n + 1;
        s++;
    }
    return s;
}

typedef struct { long long start, end, total; } Job;

void* worker(void* arg) {
    Job* j = (Job*)arg;
    long long t = 0;
    for (long long i = j->start; i < j->end; i++) t += steps(i);
    j->total = t;
    return 0;
}

int main(void) {
    pthread_t tid[8];
    Job jobs[8];
    long long chunk = 625000;
    for (int w = 0; w < 8; w++) {
        jobs[w].start = w == 0 ? 1 : w * chunk;
        jobs[w].end = (w + 1) * chunk;
        pthread_create(&tid[w], 0, worker, &jobs[w]);
    }
    long long total = 0;
    for (int w = 0; w < 8; w++) {
        pthread_join(tid[w], 0);
        total += jobs[w].total;
    }
    printf("%lld\n", total);
    return 0;
}
