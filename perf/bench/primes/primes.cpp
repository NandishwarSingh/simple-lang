#include <cstdio>

bool is_prime(long long n) {
    if (n < 2) return false;
    for (long long d = 2; d * d <= n; d++) {
        if (n % d == 0) return false;
    }
    return true;
}

int main() {
    long long count = 0;
    for (long long n = 2; n < 3000000; n++) {
        if (is_prime(n)) count++;
    }
    std::printf("%lld\n", count);
    return 0;
}
