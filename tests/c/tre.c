#include <stdio.h>
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static long gcd(long a, long b) { if (b == 0) return a; return gcd(b, a % b); }
static unsigned sum(const unsigned *p, int n) { if (n == 0) return 7; return p[0] * 3 + sum(p + 1, n - 1); }
static long tri(long n, long k) { if (n <= 0) return k; if (n & 1) return tri(n - 1, k + 1) + n; return n * 2 + tri(n - 2, k); }
static int depth;
static void walk(int n) { depth += n; if (n > 0) walk(n - 1); }
static int addr(int n) { int x = n; int *p = &x; if (n <= 0) return 0; return *p + addr(n - 1); }
static int after(int n) { int r; if (n <= 0) return 1; r = after(n - 1); return r + r; }
static int g;
static int side(int n) { if (n <= 0) return g; g++; return side(n - 1) + n; }
static int ack(int m, int n) { if (m == 0) return n + 1; if (n == 0) return ack(m - 1, 1); return ack(m - 1, ack(m, n - 1)); }
static int loopy(int v, int k)
{
    for (;;) {
        if (v > 1000)
            return k;
        if (v & 1) {
            v = v * 3 + 1;
            continue;
        }
        if (k > 50)
            return v;
        return loopy(v + 7, k + 1) + v;
    }
}
static int loopt(int v)
{
    for (;;) {
        if (v > 5000)
            return v;
        if (v % 3 == 0) {
            v += 5;
            continue;
        }
        return loopt(v * 2 + 1);
    }
}
int main(void)
{
    unsigned a[100];
    int i;
    long t = 0;
    for (i = 0; i < 100; i++)
        a[i] = i * 2654435761u;
    for (i = 0; i < 25; i++)
        t += fib(i);
    t += gcd(1071, 462) + gcd(1L << 40, 3L << 20) + sum(a, 100) + sum(a, 0);
    for (i = -2; i < 30; i++)
        t += tri(i, i * 3);
    walk(1000);
    t += depth + addr(50) + after(10) + side(100) + g + ack(2, 3);
    for (i = 0; i < 40; i++)
        t += loopy(i, i) + loopt(i);
    printf("%ld\n", t);
    return 0;
}
