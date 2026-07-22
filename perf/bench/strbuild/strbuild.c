#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    long long total = 0;
    const char* piece = "abcdefg";
    size_t plen = strlen(piece);
    for (int i = 0; i < 1000000; i++) {
        char* s = malloc(1);
        s[0] = 0;
        size_t slen = 0;
        for (int j = 0; j < 8; j++) {
            size_t n = slen + plen;
            char* t = malloc(n + 1);
            memcpy(t, s, slen);
            memcpy(t + slen, piece, plen + 1);
            free(s);
            s = t;
            slen = n;
            // barrier: keep the allocation observable so -O2 can't delete it
            __asm__ __volatile__("" : : "r"(s) : "memory");
        }
        total += (long long)slen;
        free(s);
    }
    printf("%lld\n", total);
    return 0;
}
