/* 32-bit loop counters read widened (an index, a long sum): on x86-64 the counter becomes a 64-bit one where it
   cannot wrap. Counters at the ends of int's range, counting down, steps other than 1, bounds that are not
   constants, unsigned reads, the counter read after the loop, loops left from the middle, two counters. */
#include <limits.h>
#include <stdio.h>

static int tab[64];

__attribute__((noinline)) static long up(int lo, int hi)
{
    long s = 0;
    int i;
    for (i = lo; i < hi; i++)
        s += (long)i - lo + tab[(long)i - lo & 63];
    return s * 2 + i;
}

__attribute__((noinline)) static long down(int hi, int lim)
{
    long s = 0;
    int i;
    for (i = hi; i > lim; i--)
        s += (long)i - lim + tab[(long)i - lim & 63];
    return s * 2 + i;
}

__attribute__((noinline)) static long downge(int n)
{
    long s = 0;
    int i;
    for (i = n - 1; i >= 0; i--)
        s += tab[i] * (long)i;
    return s;
}

__attribute__((noinline)) static long step3(int lo)
{
    long s = 0;
    int i;
    for (i = lo; i < 61; i += 3)
        s += tab[i] + (long)i;
    return s + i;
}

__attribute__((noinline)) static long stepn(int lo, int hi, int st)
{
    long s = 0;
    int i;
    for (i = lo; i < hi; i += st)
        s += tab[i & 63] + (long)i;
    return s + i;
}

__attribute__((noinline)) static long uns(int n)
{
    long s = 0;
    int i;
    for (i = 0; i < n; i++)
        s += tab[(unsigned)i] + (long)(unsigned)i;
    return s;
}

__attribute__((noinline)) static long wrapping(unsigned lo, int n)
{
    /* an unsigned counter compared as an int: it may pass INT_MAX, and then the loop ends */
    long s = 0;
    unsigned i;
    for (i = lo; (int)i < n; i++)
        s += (long)(int)i - (long)(int)lo + 1;
    return s + (long)(int)i;
}

__attribute__((noinline)) static long leave(const unsigned char *a, int n)
{
    long s = 0;
    int i = 0;
    while (i < n) {
        s += a[i];
        if (a[i] == 255)
            break;
        i++;
    }
    return s * 100 + i;
}

__attribute__((noinline)) static long two(int n, int m)
{
    long s = 0;
    int i, j;
    for (i = 0, j = m; i < n; i++, j--)
        s += tab[i] * 3 + tab[(long)j & 63] + (long)j;
    return s + i + j;
}

__attribute__((noinline)) static long nest(int n)
{
    long s = 0;
    int i, j;
    for (i = 0; i < n; i++)
        for (j = i; j < n; j++)
            s += tab[i] ^ tab[j] ^ (long)(i - j);
    return s;
}

__attribute__((noinline)) static long stencil(int n)
{
    long s = 0;
    int i;
    for (i = 1; i < n; i++)
        s += tab[i - 1] * 2 + tab[i] + (long)(i - 2);
    return s;
}

__attribute__((noinline)) static long before(unsigned lo, int n)
{
    /* (int)(u - 1) of an unsigned counter that starts at INT_MIN is INT_MAX: not the counter less one */
    long s = 0;
    unsigned u;
    for (u = lo; (int)u < n; u++)
        s += (long)(int)(u - 1) / 1000 + (long)(int)(u - 3) % 7;
    return s;
}

__attribute__((noinline)) static long before_k(int n)
{
    long s = 0;
    unsigned u;
    for (u = 0x80000000u; (int)u < n; u++)
        s += (long)(int)(u - 1) / 1000 + (long)(int)(u - 3) % 7;
    return s;
}

int main(void)
{
    unsigned char u[20];
    int k;
    for (k = 0; k < 64; k++)
        tab[k] = k * 37 % 101 - 50;
    for (k = 0; k < 20; k++)
        u[k] = (unsigned char)(k == 17 ? 255 : k * 13);
    printf("%ld %ld %ld %ld\n", up(0, 50), up(-7, 9), up(5, 5), up(9, 3));
    printf("%ld %ld\n", up(INT_MAX - 40, INT_MAX), up(INT_MIN, INT_MIN + 33));
    printf("%ld %ld %ld\n", down(40, -3), down(INT_MIN + 30, INT_MIN), down(INT_MAX, INT_MAX - 21));
    printf("%ld %ld %ld\n", downge(64), downge(1), downge(0));
    printf("%ld %ld %ld\n", step3(0), step3(-2 + 2), step3(58));
    printf("%ld %ld %ld\n", stepn(0, 60, 7), stepn(-20, 20, 5), stepn(INT_MAX - 60, INT_MAX - 13, 11));
    printf("%ld %ld\n", uns(64), uns(0));
    printf("%ld %ld %ld\n", wrapping(3, 40), wrapping((unsigned)INT_MAX - 5, INT_MAX), wrapping(10, 10));
    printf("%ld %ld %ld\n", leave(u, 20), leave(u, 9), leave(u, 0));
    printf("%ld %ld %ld\n", two(64, 70), two(10, -5), nest(40));
    printf("%ld %ld %ld\n", stencil(64), stencil(2), stencil(1));
    printf("%ld %ld %ld\n", before(0x80000000u, INT_MIN + 5), before(7, 30), before_k(INT_MIN + 4));
    return 0;
}
