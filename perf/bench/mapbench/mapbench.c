// hand-rolled open addressing — the idiomatic C answer to "use a hash map"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

typedef struct { int64_t key; int64_t val; int used; } IEnt;
typedef struct { IEnt* e; int64_t cap; int64_t n; } IMap;

static uint64_t imix(uint64_t x) {
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ULL;
    x ^= x >> 27; x *= 0x94D049BB133111EBULL;
    x ^= x >> 31; return x;
}
static void iinit(IMap* m) { m->cap = 16; m->n = 0; m->e = calloc(16, sizeof(IEnt)); }
static void igrow(IMap* m);
static IEnt* islot(IMap* m, int64_t k) {
    uint64_t s = imix((uint64_t)k) & (m->cap - 1);
    while (m->e[s].used && m->e[s].key != k) s = (s + 1) & (m->cap - 1);
    return &m->e[s];
}
static void iput(IMap* m, int64_t k, int64_t v) {
    if ((m->n + 1) * 3 >= m->cap * 2) igrow(m);
    IEnt* e = islot(m, k);
    if (!e->used) { e->used = 1; e->key = k; m->n++; }
    e->val = v;
}
static void igrow(IMap* m) {
    IEnt* old = m->e; int64_t oc = m->cap;
    m->cap *= 2; m->e = calloc(m->cap, sizeof(IEnt)); m->n = 0;
    for (int64_t i = 0; i < oc; i++)
        if (old[i].used == 1) iput(m, old[i].key, old[i].val);
    free(old);
}
static int ihas(IMap* m, int64_t k) { return islot(m, k)->used == 1; }
static int64_t iget(IMap* m, int64_t k) { return islot(m, k)->val; }
static void idel(IMap* m, int64_t k) {
    IEnt* e = islot(m, k);
    if (e->used == 1) { e->used = 2; m->n--; }   // tombstone
}
// tombstone-aware probing: reuse used==2 as "keep probing"
static IEnt* islot2(IMap* m, int64_t k) {
    uint64_t s = imix((uint64_t)k) & (m->cap - 1);
    while (m->e[s].used) {
        if (m->e[s].used == 1 && m->e[s].key == k) return &m->e[s];
        s = (s + 1) & (m->cap - 1);
    }
    return &m->e[s];
}

typedef struct { char* key; int64_t val; int used; } SEnt;
typedef struct { SEnt* e; int64_t cap; int64_t n; } SMap;
static uint64_t shash(const char* s) {
    uint64_t h = 14695981039346656037ULL;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}
static void sinit(SMap* m) { m->cap = 16; m->n = 0; m->e = calloc(16, sizeof(SEnt)); }
static void sgrow(SMap* m);
static SEnt* sslot(SMap* m, const char* k) {
    uint64_t s = shash(k) & (m->cap - 1);
    while (m->e[s].used && strcmp(m->e[s].key, k) != 0) s = (s + 1) & (m->cap - 1);
    return &m->e[s];
}
static void sputmove(SMap* m, char* k, int64_t v) {
    SEnt* e = sslot(m, k);
    if (!e->used) { e->used = 1; e->key = k; m->n++; }
    else { free(k); }
    e->val = v;
}
static void sgrow(SMap* m) {
    SEnt* old = m->e; int64_t oc = m->cap;
    m->cap *= 2; m->e = calloc(m->cap, sizeof(SEnt)); m->n = 0;
    for (int64_t i = 0; i < oc; i++)
        if (old[i].used) sputmove(m, old[i].key, old[i].val);
    free(old);
}
static void sput(SMap* m, const char* k, int64_t v) {
    if ((m->n + 1) * 3 >= m->cap * 2) sgrow(m);
    SEnt* e = sslot(m, k);
    if (!e->used) { e->used = 1; e->key = strdup(k); m->n++; }
    e->val = v;
}
static SEnt* sfind(SMap* m, const char* k) {
    SEnt* e = sslot(m, k);
    return e->used ? e : NULL;
}

int main(void) {
    IMap a; iinit(&a);
    for (int64_t i = 0; i < 10000000; i++) iput(&a, (i * 2654435761LL) % 4000037, i);
    long long suma = 0;
    for (int64_t i = 0; i < 10000000; i++) {
        int64_t k = (i * 7919) % 4000037;
        IEnt* e = islot2(&a, k);
        if (e->used == 1) suma += e->val;
    }
    for (int64_t i = 0; i < 1000000; i++) {
        int64_t k = (i * 31) % 4000037;
        IEnt* e = islot2(&a, k);
        if (e->used == 1) { e->used = 2; a.n--; }
    }
    suma += a.n * 17;

    SMap wc; sinit(&wc);
    char buf[32];
    for (int64_t i = 0; i < 1500000; i++) {
        snprintf(buf, sizeof buf, "w%lld", (long long)((i * 131) % 9973));
        SEnt* e = sfind(&wc, buf);
        if (e) e->val++;
        else sput(&wc, buf, 1);
    }
    long long sumb = 0;
    for (int64_t i = 0; i < 9973; i++) {
        snprintf(buf, sizeof buf, "w%lld", (long long)i);
        SEnt* e = sfind(&wc, buf);
        if (e) sumb += e->val * (i + 1);
    }
    long long sumc = 0;
    for (int64_t i = 0; i < wc.cap; i++)
        if (wc.e[i].used) sumc += wc.e[i].val;

    printf("%lld\n", suma + sumb * 3 + sumc);
    return 0;
}
