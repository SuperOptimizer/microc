/* a value kept in memory across a call and read on both sides of a two-compare branch: the register it gets for the
   blocks after the call is loaded before the first of the branches, not after it (x86-64: mcc's own sel_term read
   a register that one of the two ways had not loaded) */
#include <stdio.h>

static int tab[64];
__attribute__((noinline)) static int emit(int op) { static int n; tab[n++ & 63] = op; return n; }

__attribute__((noinline)) static int pick(int t, int e, int next, int cc, int x)
{
    int r = emit(cc), a, b;
    if (t == next || (e != next && tab[e & 63] > tab[t & 63] && x)) {
        a = e;
        b = t + r;
        if (t != next)
            a += x;
    } else {
        a = t;
        b = e - r;
    }
    r = emit(a) + emit(b);
    return r * 100 + a * 7 + b + t + e;
}

int main(void)
{
    volatile int v = 3;
    printf("%d\n", pick(v, v + 1, v, 1, 2));
    printf("%d\n", pick(v, v + 1, v + 1, 1, 2));
    printf("%d\n", pick(v + 9, v + 1, v, 1, 0));
    printf("%d\n", pick(v + 2, v + 30, v, 1, 5));
    return 0;
}
