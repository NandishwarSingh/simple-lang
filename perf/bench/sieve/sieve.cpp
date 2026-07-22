#include <cstdio>
#include <vector>
#include <algorithm>

const int N = 2000000;

int main() {
    std::vector<unsigned char> flags(N, 1);
    long long count = 0;
    for (int rep = 0; rep < 10; rep++) {
    count = 0;
    std::fill(flags.begin(), flags.end(), 1);
    for (long long i = 2; i < N; i++) {
        if (flags[i]) {
            count++;
            for (long long j = i + i; j < N; j += i) flags[j] = 0;
        }
    }
    }
    std::printf("%lld\n", count);
    return 0;
}
