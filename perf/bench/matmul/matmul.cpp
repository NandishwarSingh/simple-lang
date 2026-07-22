#include <cstdio>

static long long a[200][200], b[200][200], c[200][200];

int main() {
    for (int i = 0; i < 200; i++)
        for (int j = 0; j < 200; j++) {
            a[i][j] = (i + j) % 7;
            b[i][j] = ((long long)i * j) % 5;
            c[i][j] = 0;
        }
    for (int rep = 0; rep < 20; rep++)
    for (int i = 0; i < 200; i++)
        for (int k = 0; k < 200; k++) {
            long long aik = a[i][k];
            for (int j = 0; j < 200; j++)
                c[i][j] += aik * b[k][j];
        }
    long long sum = 0;
    for (int i = 0; i < 200; i++) sum += c[i][i];
    std::printf("%lld\n", sum);
    return 0;
}
