/* a flag tested twice: the second test is decided by the way the first went (x86-64 threads the jump: MCC_NOJTI) */
#include <stdio.h>

__attribute__((noinline)) static float fill(float lim) { return lim * .5f; }

__attribute__((noinline)) static void warp(unsigned char *T, unsigned char *M, const float *v, float lim, float *out, int n,
                                           const unsigned char *src)
{
    int k, d;
    for (k = 0; k < n; k++) {
        int inside = 1;
        float o = 0;
        for (d = 0; d < 3; d++)
            if (v[3 * k + d] < 0 || v[3 * k + d] > lim)
                inside = 0;
        T[k] = 0;
        M[k] = 0;
        if (!inside)
            o = fill(lim);
        if (inside) {
            o = src[(int)v[3 * k]] * 2.f;
            M[k] = 1;
        }
        out[k] = o;
    }
}

/* not a truth value: nonzero is all the first test says */
__attribute__((noinline)) static int count(int x, int y)
{
    int r = 0;
    if (x)
        r += y;
    if (x == 2)
        r += 100;
    if (x != 0)
        r += 1000;
    if (!x)
        r -= 7;
    return r;
}

/* the value changes between the tests */
__attribute__((noinline)) static int again(int x, int *p)
{
    int r = 0, f = x > 3;
    if (f)
        r = *p;
    f = f && r > 10;
    if (f)
        r += 5;
    if (!f)
        r -= 1;
    return r;
}

/* the second test in a loop the first is outside of */
__attribute__((noinline)) static int loop(int n, int f)
{
    int k, r = 0;
    if (f)
        r = 3;
    for (k = 0; k < n; k++) {
        if (f)
            r += k;
        else
            r -= 1;
        if (k == 2)
            f = !f;
    }
    return r;
}

int main(void)
{
    static float v[12] = {1, 2, 3, -1, 2, 3, 1, 9, 3, 0, 0, 8};
    static unsigned char src[16] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
    unsigned char T[4], M[4];
    float out[4];
    volatile int n = 4, z = 0;
    int k, p = 20;
    warp(T, M, v, 8, out, n, src);
    for (k = 0; k < 4; k++)
        printf("%g %d %d\n", out[k], T[k], M[k]);
    printf("%d %d %d %d\n", count(z, 5), count(z + 1, 5), count(z + 2, 5), count(z - 3, 5));
    printf("%d %d", again(z + 4, &p), again(z, &p));
    p = 3;
    printf(" %d\n", again(z + 9, &p));
    printf("%d %d %d\n", loop(n + 2, z), loop(n + 2, z + 1), loop(z, z + 1));
    return 0;
}
