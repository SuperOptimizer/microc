/* Branches decided by where control came from (jump threading, opt.c): an inlined function returning a truth value
   whose test is a phi of constants, with the reader's state live into both outcomes, in loops, nested, with
   several returns, the result also stored, and a phi whose value is used after the branch on either side. */
#include <stdio.h>
typedef unsigned long long u64;
typedef struct {
    const unsigned char *b;
    unsigned long pos, n;
    u64 acc;
    unsigned nb;
} Rd;
static int get(Rd *r, unsigned n, unsigned *out)
{
    while (r->nb < n) {
        if (r->pos >= r->n)
            return 0;
        r->acc |= (u64)r->b[r->pos++] << r->nb;
        r->nb += 8;
    }
    *out = (unsigned)(r->acc & ((1ull << n) - 1));
    r->acc >>= n;
    r->nb -= n;
    return 1;
}
static int classify(int x, int *k)
{
    if (x < 0) {
        *k = -x;
        return 0;
    }
    if (x == 0) {
        *k = 7;
        return 2;
    }
    if (x > 1000)
        return 3;
    *k = x * 3;
    return 1;
}
static int even(unsigned v) { return (v & 1) == 0; }
__attribute__((noinline)) long decode(const unsigned char *b, unsigned long n, unsigned *o, int cnt, const unsigned char *w)
{
    Rd r = {b, 0, n, 0, 0};
    long s = 0;
    int i;
    for (i = 0; i < cnt; i++) {
        unsigned v;
        if (!get(&r, w[i], &v))
            return -1 - i;
        o[i] = v;
        s += v;
        if (even(v) && !get(&r, 3, &v))
            return -1000 - (long)r.nb;
        s += v * 3 + r.nb;
    }
    return s + (long)r.pos;
}
__attribute__((noinline)) long cls(const int *a, int n)
{
    long s = 0;
    int i, k = 0;
    for (i = 0; i < n; i++) {
        int c = classify(a[i], &k);
        if (c == 1)
            s += k;
        else if (c == 0)
            s -= k * 2;
        else if (c == 2)
            s += k + i;
        else
            s ^= a[i];
        s += c;
    }
    return s + k;
}
__attribute__((noinline)) int pick(int a, int b, int c)
{
    int t, u;
    if (a > b) {
        t = 1;
        u = a;
    } else if (b > c) {
        t = 0;
        u = b;
    } else {
        t = 1;
        u = c;
    }
    if (t)
        return u * 2 + t;
    return u - 5;
}
int main(void)
{
    unsigned char buf[300], w[100];
    unsigned o[100];
    int a[200], i;
    u64 x = 0x9e3779b97f4a7c15ull;
    long t = 0;
    for (i = 0; i < 300; i++)
        buf[i] = (unsigned char)(x * (u64)(i + 1) >> 33);
    for (i = 0; i < 100; i++)
        w[i] = (unsigned char)(1 + (x * (u64)(i + 7) >> 40) % 17);
    for (i = 0; i < 200; i++)
        a[i] = (int)((x * (u64)(i + 3) >> 44) % 2500) - 700;
    a[5] = 0;
    a[9] = 0;
    for (i = 1; i <= 300; i += 37)
        printf("%ld ", decode(buf, (unsigned long)i, o, 100, w));
    printf("\n%ld %ld\n", cls(a, 200), cls(a + 3, 50));
    for (i = 0; i < 27; i++)
        t = t * 7 + pick(i % 3, (i / 3) % 3, i / 9);
    printf("%ld\n", t);
    return 0;
}
