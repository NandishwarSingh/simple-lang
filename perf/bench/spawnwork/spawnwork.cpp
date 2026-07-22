#include <cstdio>
#include <thread>

long long steps(long long n) {
    long long s = 0;
    while (n != 1) {
        if (n % 2 == 0) n = n / 2;
        else n = 3 * n + 1;
        s++;
    }
    return s;
}

int main() {
    long long totals[8] = {0};
    std::thread ts[8];
    long long chunk = 625000;
    for (int w = 0; w < 8; w++) {
        long long lo = w == 0 ? 1 : w * chunk;
        long long hi = (w + 1) * chunk;
        ts[w] = std::thread([&totals, w, lo, hi] {
            long long t = 0;
            for (long long i = lo; i < hi; i++) t += steps(i);
            totals[w] = t;
        });
    }
    long long total = 0;
    for (int w = 0; w < 8; w++) {
        ts[w].join();
        total += totals[w];
    }
    std::printf("%lld\n", total);
    return 0;
}
