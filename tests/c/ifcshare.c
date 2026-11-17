/* If-conversion where one way of a branch goes through an empty block shared with another branch (the 0 of
   "i < 256 ? 0 : u[i - 256]" and of the "a == 0 ? 0 : ..." that follows), and the frequency guesses that lay
   the blocks out (loops that count to a constant, up or down, by steps; branches out of the middle of loops;
   ifs with and without bodies that return). What a branch guards (a load, a division) must stay guarded. */
#include <stdio.h>

__attribute__((noinline)) static unsigned ctx(const unsigned char *u, unsigned i)
{
    unsigned a = i < 256u ? 0u : u[i - 256u];
    return 5u + (a == 0u ? 0u : (a <= 2u ? 1u : 2u));
}

/* the same value from two tests, the second way straight to the join */
__attribute__((noinline)) static int pick(int p, int q, int a, int b)
{
    int x = a;
    if (p)
        goto five;
    if (q) {
    five:
        x = 5;
    }
    return x + b;
}

__attribute__((noinline)) static int pick2(int p, int q, int a, int b)
{
    int x;
    if (p > 3)
        x = 7;
    else if (q > 3)
        x = 7;
    else
        x = a;
    return x * b;
}

/* guarded: the pointer is read only when it is not null, the division done only by a nonzero value */
__attribute__((noinline)) static int guarded(const int *p, int i, int d)
{
    int a = i < 4 ? 0 : (p ? *p : 0);
    int b = i < 4 ? 0 : (d ? 1000 / d : 0);
    return a + b;
}

/* float values through the shared arm */
__attribute__((noinline)) static double fpick(int p, int q, double a)
{
    double x = p ? 1.5 : (q ? 1.5 : a);
    return x + x;
}

/* loops for the frequency guesses: constant counts up, down and by steps, a count that depends on a parameter,
   a way out of the middle, a return from the middle, two counters, an inner loop behind a rare test */
__attribute__((noinline)) static unsigned loops(const unsigned char *u, unsigned n, unsigned stop)
{
    unsigned s = 0, i, j, k;
    for (i = 0; i < 4096; i++)
        s += ctx(u, i);
    for (i = 4096; i > 0; i--)
        s ^= u[i - 1] + i;
    for (i = 3; i < 4000; i += 7)
        s += u[i] * 3u;
    for (i = 4095; i != 5; i -= 2)
        s += u[i] >> 1;
    for (i = 0; i < n; i++) {
        if (u[i] == stop)
            break;
        s += u[i];
    }
    for (i = 0; i < 64; i++) {
        if (u[i] > 250 && stop > 300)
            return s + 1000000u;
        if (u[i] > 200)
            for (j = 0; j < 4; j++)
                s += u[i + j] & 3u;
        else if (u[i] > 100)
            s += 2;
    }
    for (i = 0, k = 100; i < 50 && k > 3; i += 3, k -= 5)
        s += u[i] * k;
    i = 0;
    do {
        s += u[i] ^ 0x55u;
        i += 5;
    } while (i < 1000);
    return s;
}

int main(void)
{
    static unsigned char u[4200];
    unsigned i, s = 0;
    static const int seven = 7;
    for (i = 0; i < 4200; i++)
        u[i] = (unsigned char)((i * 2654435761u) >> 23);
    u[300] = 0;
    u[301] = 1;
    u[302] = 2;
    u[303] = 3;
    for (i = 0; i < 4096; i++)
        s = s * 31u + ctx(u, i);
    printf("%u %u %u %u %u %u\n", s, ctx(u, 0), ctx(u, 556), ctx(u, 557), ctx(u, 558), ctx(u, 559));
    printf("%d %d %d %d\n", pick(0, 0, 3, 10), pick(1, 0, 3, 10), pick(0, 1, 3, 10), pick(1, 1, 3, 10));
    printf("%d %d %d %d\n", pick2(0, 0, 3, 10), pick2(4, 0, 3, 10), pick2(0, 4, 3, 10), pick2(4, 4, 3, 10));
    printf("%d %d %d %d %d\n", guarded(0, 0, 0), guarded(0, 9, 0), guarded(&seven, 9, 0), guarded(&seven, 9, 8), guarded(&seven, 1, 8));
    printf("%g %g %g\n", fpick(1, 0, 4.25), fpick(0, 1, 4.25), fpick(0, 0, 4.25));
    printf("%u %u %u %u\n", loops(u, 4000, 999), loops(u, 100, 17), loops(u, 0, 301), loops(u, 10, u[5]));
    return 0;
}
