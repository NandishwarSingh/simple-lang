#include <cstdio>
#include <cstdint>

static long long popcount(uint64_t x) {
    long long n = 0;
    while (x != 0) {
        x = x & (x - 1);
        n++;
    }
    return n;
}

static uint64_t mix(uint64_t h) {
    h = h ^ (h >> 33);
    h = h * 18397679294719823053ULL;
    h = h ^ (h >> 29);
    return h;
}

int main() {
    long long total = 0;
    uint64_t h = 12345;
    for (int i = 0; i < 20000000; i++) {
        h = mix(h);
        total += popcount(h);
    }
    std::printf("%lld\n", total);
    return 0;
}
