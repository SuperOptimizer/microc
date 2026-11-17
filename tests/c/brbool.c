/* a flag that several tests may clear, then one branch on it: a branch per test (x86-64: br_bools, FUSE_BR2 with
   constants at later leaves and stores between the tests and the branch). The values must not change: NaN
   compares false, the stores between are done on every way, an operand loaded once is read where it stood. */
#include <stdio.h>

static unsigned char dst[64], src[64];
static float cs[8];

static int inside3(const float *v, int P, unsigned char *out)
{
    int inside = 1, d;
    for (d = 0; d < 3; d++)
        if (v[d] < 0 || v[d] > P - 1)
            inside = 0;
    *out = 255; /* written on both ways */
    if (!inside)
        return -1;
    *out = (unsigned char)((int)v[0] + (int)v[1] + (int)v[2]);
    return *out;
}

static float corners(const float *f, const unsigned char *p, int lim)
{
    float s = 0;
    int c;
    for (c = 0; c < 8; c++) {
        float w = ((c & 4) ? f[0] : 1 - f[0]) * ((c & 2) ? f[1] : 1 - f[1]) * ((c & 1) ? f[2] : 1 - f[2]);
        int ph = p[c];
        if (w <= 1e-7f || ph >= lim)
            continue;
        s += w * cs[ph & 7];
    }
    return s;
}

/* the load is used by the test alone and a store that may write it stands between: the old value is tested */
static int stale(int *p, int *q, int a)
{
    int ok = 1;
    if (*p > 3 || a > 100)
        ok = 0;
    *q = 50; /* q may be p */
    if (!ok)
        return 1;
    return *p + 2;
}

static int ands(double x, double y, int k)
{
    int r = 0;
    if (x >= 0.5 && y < 2.5 && k != 7)
        r = 1;
    if (!(x > -1e300) || y != y)
        r += 10;
    return r;
}

int main(void)
{
    float v[5][3], f[3];
    volatile float zero = 0;
    int k, a, b;
    unsigned char o;
    v[0][0] = 1.5f, v[0][1] = 2.25f, v[0][2] = 30.0f;
    v[1][0] = -0.5f, v[1][1] = 2, v[1][2] = 3;
    v[2][0] = 1, v[2][1] = 31.5f, v[2][2] = 3;
    v[3][0] = 1, v[3][1] = 2, v[3][2] = zero / zero; /* NaN: neither < 0 nor > P - 1 */
    v[4][0] = 0, v[4][1] = 31, v[4][2] = 0;
    for (k = 0; k < 5; k++) {
        int r = inside3(v[k], 32, &o);
        printf("%d %d\n", r == -1 ? -1 : r > 0, o == 255 ? 255 : o);
    }
    for (k = 0; k < 8; k++)
        cs[k] = (float)(k + 1) * 0.5f, src[k] = (unsigned char)(k * 9);
    f[0] = 0.25f, f[1] = 0.5f, f[2] = 1.0f;
    printf("%g %g\n", corners(f, src, 40), corners(f, src, 1));
    f[2] = zero / zero;
    printf("%g\n", corners(f, src, 40) == corners(f, src, 40) ? 1.0 : 0.0);
    a = 2, b = 2;
    printf("%d %d %d\n", stale(&a, &a, 5), a, stale(&b, &a, 500));
    a = 9;
    printf("%d %d\n", stale(&a, &a, 5), a);
    printf("%d %d %d %d %d\n", ands(1, 2, 3), ands(0.25, 2, 3), ands(1, 2, 7), ands(zero / zero, 1, 1), ands(1, zero / zero, 1));
    return 0;
}
