#include <stdio.h>

#define N 8192
static double x[N], y[N];
static double a[128][128], b[128][128];

int main(void) {
    for (int i = 0; i < N; i++) {
        x[i] = (double)(i * 7 % 1000) / 1000.0;
        y[i] = (double)(i * 13 % 1000) / 1000.0;
    }

    for (int step = 0; step < 120000; step++) {
        for (int i = 0; i < N; i++) {
            x[i] = x[i] * 0.99 + y[i] * 0.01;
            y[i] = y[i] * 0.99 + x[i] * 0.01;
        }
    }

    for (int step = 0; step < 70000; step++) {
        for (int i = 0; i < N; i++) {
            double v = x[i];
            x[i] = ((v * 0.5 + 0.25) * v + 0.1) * 0.5 + y[i] * 0.5;
        }
    }

    for (int i = 0; i < 128; i++) {
        for (int j = 0; j < 128; j++) {
            a[i][j] = (double)((i * 7 + j) % 100) / 100.0;
            b[i][j] = (double)((i * 5 + j * 3) % 100) / 100.0;
        }
    }
    double trace = 0.0;
    for (int rep = 0; rep < 200; rep++) {
        static double c[128][128];
        for (int i = 0; i < 128; i++)
            for (int j = 0; j < 128; j++) c[i][j] = 0.0;
        for (int i = 0; i < 128; i++) {
            for (int k = 0; k < 128; k++) {
                double aik = a[i][k];
                for (int j = 0; j < 128; j++) c[i][j] = c[i][j] + aik * b[k][j];
            }
        }
        for (int i = 0; i < 128; i++) trace = trace + c[i][i];
    }

    double s = 0.0;
    for (int i = 0; i < N; i++) s = s + x[i] + y[i];
    s = s + trace;
    long long q = (long long)(s * 1000000.0);
    printf("%lld\n", q % 1000000000000LL);
    return 0;
}
