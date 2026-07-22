#include <stdio.h>

long long steps(long long n) {
    long long s = 0;
    while (n != 1) {
        if (n % 2 == 0) n = n / 2;
        else n = 3 * n + 1;
        s++;
    }
    return s;
}

int main(void) {
    long long total = 0;
    for (long long i = 1; i < 5000000; i++) total += steps(i);
    printf("%lld\n", total);
    return 0;
}
