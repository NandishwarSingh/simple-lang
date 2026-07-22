#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N 2000000

int main(void) {
    unsigned char *flags = malloc(N);
    memset(flags, 1, N);
    long long count = 0;
    for (int rep = 0; rep < 10; rep++) {
    count = 0;
    memset(flags, 1, N);
    for (long long i = 2; i < N; i++) {
        if (flags[i]) {
            count++;
            for (long long j = i + i; j < N; j += i) flags[j] = 0;
        }
    }
    }
    printf("%lld\n", count);
    free(flags);
    return 0;
}
