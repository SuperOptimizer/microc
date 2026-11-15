/* isnan, isinf, isfinite and signbit as glibc's <math.h> makes them (mcc defines __SUPPORT_SNAN__): calls of __isnan(f),
   __isinf(f), __finite(f), __signbit(f), which x86-64 mcc does in line. Every kind of float and double (zeros,
   denormals, extremes, infinities, NaNs of both signs) and a loop guard; the values are the library's (__isinf: -1 for
   -inf), the expected output is clang + glibc's. Declared here, so the test needs no C library with them. */
#include <stdio.h>
int __isnan(double), __isnanf(float), __isinf(double), __isinff(float), __finite(double), __finitef(float);
int __signbit(double), __signbitf(float);
static volatile double big = 1e300, zero = 0.0;
static float fv[12];
static double dv[12];
__attribute__((noinline)) int cls_f(float x)
{
    return __isnanf(x) * 1000 + (__isinff(x) + 2) * 100 + __finitef(x) * 10 + (__signbitf(x) != 0);
}
__attribute__((noinline)) int cls_d(double x)
{
    return __isnan(x) * 1000 + (__isinf(x) + 2) * 100 + __finite(x) * 10 + (__signbit(x) != 0);
}
int main(void)
{
    unsigned i, n = 0;
    double s = 0, inf = big * big, nan = zero / zero;
    double d0[12];
    d0[0] = 0.0, d0[1] = -0.0, d0[2] = 1.0, d0[3] = -1.5, d0[4] = 1e-310, d0[5] = -1e-310, d0[6] = 1.7e308, d0[7] = -1.7e308;
    d0[8] = inf, d0[9] = -inf, d0[10] = nan, d0[11] = -nan;
    for (i = 0; i < 12; i++) {
        dv[i] = d0[i];
        fv[i] = i == 4 ? 1e-40f : i == 5 ? -1e-40f : i == 6 ? 3.4e38f : i == 7 ? -3.4e38f : (float)d0[i];
    }
    for (i = 0; i < 12; i++)
        printf("f %u %d %d\n", i, cls_f(fv[i]), cls_d(dv[i]));
    for (i = 0; i < 1000; i++) {
        double v = (i % 7 == 0) ? nan : (i % 11 == 0) ? (i & 1 ? inf : -inf) : (double)i - 500;
        if (!__finite(v)) {
            n++;
            continue;
        }
        if (__isnan(v))
            n += 100;
        s += v;
    }
    printf("%u %g\n", n, s);
    return 0;
}
