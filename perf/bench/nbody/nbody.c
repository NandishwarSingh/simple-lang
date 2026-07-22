/* IDIOMATIC-CLASS: written the way C is normally written — structs are
 * passed and mutated through pointers. Because a and b may alias, the
 * compiler must reload b's fields after each write through a. */
#include <stdio.h>

typedef struct {
    long long x, y, vx, vy;
} P;

static void interact(P *a, const P *b) {
    long long dx = b->x - a->x;
    long long dy = b->y - a->y;
    a->vx += dx / 16;
    a->vy += dy / 16;
}

int main(void) {
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
                    interact(&ps[i], &ps[j]);
        for (int i = 0; i < 8; i++) {
            ps[i].x += ps[i].vx;
            ps[i].y += ps[i].vy;
        }
    }
    long long h = 0;
    for (int i = 0; i < 8; i++)
        h += ps[i].x + ps[i].y + ps[i].vx + ps[i].vy;
    printf("%lld\n", h);
    return 0;
}
