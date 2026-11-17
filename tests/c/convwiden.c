/* (long)(a + k) made (long)a + k where the int sum cannot have wrapped; unsigned sums, and sums of another
   function with the same numbers, must keep wrapping */
#include <stdio.h>
#include <stddef.h>

static int f1(int a) { return a + 1; }
static long g1(unsigned a) { return (int)(a + 1u); }
static long g2(unsigned a, unsigned b) { return (int)(a - b); }
static long h1(int a) { long x = a + 3; long y = a - 5; return x * 1000 + y; }
static long h2(int a, int b) { return (long)(a + b) * 7 - (long)(a - b + 2); }
static size_t idx(int b0, int b1, int b2, int z, int y, int x, int P)
{
    return ((size_t)(b0 + z) * P + b1 + y) * P + b2 + x;
}
static long mixed(int a)
{
    unsigned u = (unsigned)a + 1u; /* wraps */
    int s = (int)u;
    long r = s;                    /* must be the wrapped value */
    return r;
}
static long both(int a, int c)
{
    long r = 0;
    if (c) {
        int s = a + 1; /* signed: executed only here */
        r = s;
    } else {
        unsigned u = (unsigned)a + 1u;
        r = (int)u;
    }
    return r;
}
static float tab[64];
static float get(const float *p, int i, int n)
{
    float s = 0;
    int k;
    for (k = 0; k < n; k++)
        s += p[i + k] + p[i - 1 + k];
    return s;
}

int main(void)
{
    int k;
    volatile int big = 0x7fffffff;
    volatile unsigned ub = 0x7fffffffu;
    for (k = 0; k < 64; k++)
        tab[k] = (float)k;
    printf("%d %ld %ld\n", f1(41), g1(ub), g1(0xffffffffu));
    printf("%ld %ld\n", g2(0u, 1u), g2(0x80000000u, 1u));
    printf("%ld %ld\n", h1(-1), h1(100));
    printf("%ld %ld\n", h2(3, 4), h2(-100000, 7));
    printf("%lu %lu\n", (unsigned long)idx(1, 2, 3, 1, 0, 1, 16), (unsigned long)idx(0, 0, 0, 0, 0, 0, 16));
    printf("%ld %ld %ld\n", mixed(big), both(big, 0), both(5, 1));
    printf("%g %g\n", get(tab, 1, 8), get(tab, 20, 3));
    return 0;
}
