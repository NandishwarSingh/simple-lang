/* IDIOMATIC-CLASS: normal C++ — references, mutation in place. */
#include <cstdio>

struct P {
    long long x, y, vx, vy;
};

static void interact(P &a, const P &b) {
    long long dx = b.x - a.x;
    long long dy = b.y - a.y;
    a.vx += dx / 16;
    a.vy += dy / 16;
}

int main() {
    P ps[8];
    for (int i = 0; i < 8; i++) {
        ps[i].x = (long long)i * 100;
        ps[i].y = (long long)i * 37;
        ps[i].vx = 1;
        ps[i].vy = 2;
    }
    for (int s = 0; s < 2000000; s++) {
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 8; j++)
                if (i != j)
                    interact(ps[i], ps[j]);
        for (int i = 0; i < 8; i++) {
            ps[i].x += ps[i].vx;
            ps[i].y += ps[i].vy;
        }
    }
    long long h = 0;
    for (int i = 0; i < 8; i++)
        h += ps[i].x + ps[i].y + ps[i].vx + ps[i].vy;
    std::printf("%lld\n", h);
    return 0;
}
