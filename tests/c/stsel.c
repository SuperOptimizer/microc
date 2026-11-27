#include <stdio.h>
struct P { short life; int x; unsigned char f; double d; };
static void step(struct P *p, int n, int *other)
{
    int i;
    for (i = 0; i < n; i++) {
        p[i].life = (short)(p[i].life - 1);
        if (p[i].life < 0)
            p[i].life = 99;
        p[i].x = p[i].x * 3 + 1;
        if (p[i].x > 1000)
            ;
        else
            p[i].x = p[i].x - 7;
        p[i].d = p[i].d * 0.5;
        if (p[i].d < 1.0)
            p[i].d = p[i].d + 100.0 + i;
        p[i].f = (unsigned char)(p[i].f + 3);
        if (p[i].f & 1) {
            *other += 1;          /* another store first: not converted */
            p[i].f = 0;
        }
        p[i].x += 1;
        if (*other > 50)          /* the arm reads memory: not converted */
            p[i].x = *other;
    }
}
int main(void)
{
    struct P a[40];
    int i, o = 0, r;
    long s = 0;
    for (i = 0; i < 40; i++) {
        a[i].life = (short)(i % 7);
        a[i].x = i * 37 - 300;
        a[i].f = (unsigned char)i;
        a[i].d = i * 3.25;
    }
    for (r = 0; r < 50; r++)
        step(a, 40, &o);
    for (i = 0; i < 40; i++)
        s = s * 31 + a[i].life + a[i].x + a[i].f + (long)a[i].d;
    printf("%ld %d\n", s, o);
    return 0;
}
