/* sin and cos of one value fused into one sincos call (opt.c fuse_sincos: -mglibc, no math errno, MCC_NOSINCOS): a pair
   in one block, in a loop, the float pair, a sin only on one way (the cos before it dominates: fused) */
#include <stdio.h>
#include <math.h>

static double pair(double x) { return sin(x) * 2 + cos(x); }

static double loop(const double *a, int n)
{
    double s = 0;
    int i;
    for (i = 0; i < n; i++) {
        double c = cos(a[i]), si = sin(a[i]);
        s += c * 3 + si * si;
    }
    return s;
}

static float pairf(float x) { return sinf(x) - cosf(x) * 0.5f; }

static double branch(double x, int m)
{
    double r = cos(x);
    if (m)
        r += sin(x) * 4;
    return r;
}

int main(void)
{
    double a[37];
    int i;
    for (i = 0; i < 37; i++)
        a[i] = i * 0.37 - 5;
    printf("pair %.6f %.6f\n", pair(0.3), pair(-2.5));
    printf("loop %.6f\n", loop(a, 37));
    printf("pairf %.5f\n", (double)pairf(1.25f));
    printf("branch %.6f %.6f\n", branch(0.7, 0), branch(0.7, 1));
    return 0;
}
