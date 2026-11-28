/* 32-bit values widened to 64 after a phi or a select (x64.c: the widening is no instruction when every value that
   meets there was made by a 32-bit operation). The ones that were not: halves of 64-bit values, parameters and
   results of calls with rubbish above, values that go round a loop, each mixed with ones that are clean. */
#include <stdio.h>

__attribute__((noinline)) static unsigned long wide(unsigned long x) { return x * 0x9e3779b97f4a7c15ul + 0xfedcba9800000000ul; }
__attribute__((noinline)) static unsigned low(unsigned long x) { return (unsigned)x; }
__attribute__((noinline)) static unsigned long idx(const unsigned long *t, unsigned i) { return t[i] + i; }

__attribute__((noinline)) static unsigned long pick(unsigned long big, int c)
{
    unsigned x = c ? (unsigned)big : 5u; /* a half of big, or a constant */
    return (unsigned long)x * 3 + (unsigned long)x;
}

__attribute__((noinline)) static unsigned long join(unsigned long big, unsigned small, int c)
{
    unsigned x;
    if (c & 1)
        x = (unsigned)(big >> 3);
    else if (c & 2)
        x = small + 1;
    else
        x = (unsigned)big;
    return (unsigned long)x << 4 | (c & 4 ? x : 0);
}

__attribute__((noinline)) static unsigned long loop(unsigned long big, int n)
{
    unsigned x = (unsigned)big, k;
    unsigned long s = 0;
    for (k = 0; k < (unsigned)n; k++) {
        s += (unsigned long)x + k; /* x starts as a half and goes round; k is clean all the way */
        x = k & 1 ? x * 3 : (unsigned)(s >> 7);
    }
    return s ^ x;
}

__attribute__((noinline)) static unsigned long calls(unsigned long big, int c)
{
    unsigned x = c ? low(big) : 7u, y = c > 1 ? (unsigned)wide(big) : x;
    return (unsigned long)x + ((unsigned long)y << 1);
}

__attribute__((noinline)) static unsigned long table(const unsigned long *t, unsigned long big, int n)
{
    unsigned i = (unsigned)big & 7, k;
    unsigned long s = 0;
    for (k = 0; k < (unsigned)n; k++) {
        s += t[i] + idx(t, i);
        i = (i * 5 + (unsigned)(s >> 60) + 1) & 7;
        if (s & 1)
            i = (unsigned)(big >> k) & 7;
    }
    return s;
}

__attribute__((noinline)) static long neg(long big, int c)
{
    int x = c ? (int)big : -9; /* signed: the widening is another instruction, and has to stay one */
    unsigned u = c ? (unsigned)big : 0xfffffff7u;
    return (long)x * 2 + (long)(unsigned long)u;
}

int main(void)
{
    unsigned long t[8], big = 0xdeadbeefcafef00dul, r = 0;
    int k;
    for (k = 0; k < 8; k++)
        t[k] = wide((unsigned long)k);
    printf("%lu %lu\n", pick(big, 0), pick(big, 1));
    for (k = 0; k < 8; k++)
        r = r * 31 + join(big + (unsigned long)k, (unsigned)k * 0x40000001u, k);
    printf("%lu\n", r);
    printf("%lu %lu %lu\n", loop(big, 1), loop(big, 10), loop(~big, 33));
    printf("%lu %lu %lu\n", calls(big, 0), calls(big, 1), calls(big, 2));
    printf("%lu %lu\n", table(t, big, 5), table(t, ~big, 40));
    printf("%ld %ld\n", neg((long)big, 0), neg((long)big, 1));
    return 0;
}
