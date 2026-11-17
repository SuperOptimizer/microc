/* a pure call read on one side of a choice only: called on that side (x86-64 host with the GNU C library and
   -fno-math-errno); the values must be the same either way */
#include <math.h>
#include <stdio.h>

static double wind(double z, double y, double x)
{
    double r = exp((y - 3.0) * 0.125) + cos(x - z), th = atan2(y - 3.0, x - z);
    return r / 24.0 - th / 6.283185307179586 + .002 * z;
}

static unsigned char ph[16 * 16 * 16];

static void gen(int n, int cut)
{
    int z, y, x;
    for (z = 0; z < n; z++)
        for (y = 0; y < n; y++)
            for (x = 0; x < n; x++) {
                double w = wind(z, y, x), f = w - floor(w);
                int cc = (int)(f * 252);
                if (cc >= 252)
                    cc = 251;
                ph[(z * n + y) * n + x] = z < cut ? (unsigned char)cc : 255;
            }
}

static double both(double a, double b, int c)
{
    double s = sin(a) * 2, t = cos(b) + 1; /* one on each side */
    return c ? s + a : t - b;
}

static double shared(double a, int c)
{
    double s = sqrt(a + 1) + atan2(a, 2.0); /* read on one side and after: stays */
    double r = c ? s : 1.0;
    return r + s;
}

static int branchy(double a, int c, int d)
{
    double e = exp(a);
    int r = 0;
    if (c) {
        if (d)
            r = (int)(e * 10);
        else
            r = 3;
    }
    return r;
}

int main(void)
{
    unsigned long sum = 0;
    int k;
    volatile int one = 1, zero = 0;
    gen(16, 5);
    for (k = 0; k < 16 * 16 * 16; k++)
        sum = sum * 31 + ph[k];
    printf("%lu %d %d\n", sum, ph[17], ph[16 * 16 * 7]);
    printf("%.6f %.6f\n", both(0.5, 0.25, one), both(0.5, 0.25, zero));
    printf("%.6f %.6f\n", shared(2.0, one), shared(2.0, zero));
    printf("%d %d %d\n", branchy(1.0, one, one), branchy(1.0, one, zero), branchy(1.0, zero, one));
    return 0;
}
