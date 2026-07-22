#include <cstdio>
#include <string>

int main() {
    long long total = 0;
    for (int i = 0; i < 1000000; i++) {
        std::string s;
        for (int j = 0; j < 8; j++) {
            s = s + "abcdefg"; // fresh string each step
            // barrier: keep the allocation observable so -O2 can't delete it
            __asm__ __volatile__("" : : "r"(s.data()) : "memory");
        }
        total += (long long)s.size();
    }
    std::printf("%lld\n", total);
    return 0;
}
