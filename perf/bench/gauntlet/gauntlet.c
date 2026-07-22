#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static uint64_t rng_step(uint64_t s) {
    return s * 6364136223846793005ULL + 1442695040888963407ULL;
}
static int64_t rng_val(uint64_t s) { return (int64_t)(s >> 33); }

typedef struct { int64_t x, y, z, vx, vy, vz; } Body;

static Body interact(Body a, Body b) {
    int64_t dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
    int64_t d2 = dx*dx + dy*dy + dz*dz + 1;
    int64_t inv = 1000000 / (d2/1000 + 1);
    Body r = a;
    r.vx = a.vx + (dx*inv)/100000;
    r.vy = a.vy + (dy*inv)/100000;
    r.vz = a.vz + (dz*inv)/100000;
    return r;
}

static int64_t phase_a(void) {
    Body bs[32];
    memset(bs, 0, sizeof bs);
    uint64_t r = 88172645463325252ULL;
    for (int i = 0; i < 32; i++) {
        r = rng_step(r); int64_t x = rng_val(r) % 1000;
        r = rng_step(r); int64_t y = rng_val(r) % 1000;
        r = rng_step(r); int64_t z = rng_val(r) % 1000;
        bs[i] = (Body){x, y, z, 0, 0, 0};
    }
    for (int step = 0; step < 30000; step++)
        for (int i = 0; i < 32; i++)
            for (int j = 0; j < 32; j++)
                if (i != j) bs[i] = interact(bs[i], bs[j]);
    int64_t sum = 0;
    for (int i = 0; i < 32; i++) sum += bs[i].vx + bs[i].vy + bs[i].vz;
    return sum;
}

static uint64_t phase_b(void) {
    char **items = malloc(500000 * sizeof(char*));
    for (int k = 0; k < 500000; k++) {
        char buf[64];
        snprintf(buf, sizeof buf, "k%d_%d", k, (k*13) % 97);
        items[k] = strdup(buf);
    }
    uint64_t acc = 0;
    for (int i = 0; i < 500000; i++) {
        const char *s = items[i];
        uint64_t h = 14695981039346656037ULL;
        for (size_t c = 0; s[c]; c++)
            h = (h ^ (uint64_t)(unsigned char)s[c]) * 1099511628211ULL;
        acc ^= h;
        free(items[i]);
    }
    free(items);
    return acc + (uint64_t)500000;
}

static int64_t phase_c(void) {
    static int64_t a[25000];
    uint64_t r = 99887766554433ULL;
    for (int i = 0; i < 25000; i++) { r = rng_step(r); a[i] = rng_val(r) % 1000000; }
    for (int i = 1; i < 25000; i++) {
        int64_t v = a[i]; int j = i - 1;
        while (j >= 0 && a[j] > v) { a[j+1] = a[j]; j--; }
        a[j+1] = v;
    }
    int64_t s = 0;
    for (int i = 0; i < 25000; i++) s += a[i] * i;
    return s;
}

static int64_t fib(int64_t n) { return n < 2 ? n : fib(n-1) + fib(n-2); }
static int64_t phase_d(void) { return fib(40); }

static int64_t phase_e(void) {
    static int64_t a[100][100], b[100][100], c[100][100];
    for (int i = 0; i < 100; i++)
        for (int j = 0; j < 100; j++) {
            a[i][j] = (i*7 + j) % 13;
            b[i][j] = (i*5 + j*3) % 11;
        }
    int64_t acc = 0;
    for (int rep = 0; rep < 150; rep++) {
        memset(c, 0, sizeof c);
        for (int i = 0; i < 100; i++)
            for (int k = 0; k < 100; k++) {
                int64_t aik = a[i][k];
                for (int j = 0; j < 100; j++) c[i][j] += aik * b[k][j];
            }
        for (int i = 0; i < 100; i++) acc += c[i][i];
    }
    return acc;
}

int main(void) {
    int64_t a = phase_a();
    uint64_t b = phase_b();
    int64_t c = phase_c();
    int64_t d = phase_d();
    int64_t e = phase_e();
    uint64_t sum = 0;
    sum += (uint64_t)a;
    sum ^= b;
    sum += (uint64_t)c;
    sum ^= (uint64_t)d;
    sum += (uint64_t)e;
    printf("%lld\n", (long long)(sum % 1000000000000ULL));
    return 0;
}
