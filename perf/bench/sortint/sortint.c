#include <stdio.h>
#include <stdint.h>

int main(void) {
    long long a[800];
    long long check = 0;
    uint64_t seed = 987654321;
    for (int round = 0; round < 600; round++) {
        for (int i = 0; i < 800; i++) {
            seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
            a[i] = (long long)(seed >> 40);
        }
        for (int i = 1; i < 800; i++) {
            long long v = a[i];
            long long j = i - 1;
            while (j >= 0 && a[j] > v) {
                a[j + 1] = a[j];
                j--;
            }
            a[j + 1] = v;
        }
        check += a[0] + a[799];
    }
    printf("%lld\n", check);
    return 0;
}
