#include <stdio.h>

int is_prime(long long n) {
    if (n < 2) return 0;
    for (long long d = 2; d * d <= n; d++) {
        if (n % d == 0) return 0;
    }
    return 1;
}

int main(void) {
    long long count = 0;
    for (long long n = 2; n < 3000000; n++) {
        if (is_prime(n)) count++;
    }
    printf("%lld\n", count);
    return 0;
}
