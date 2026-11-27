/* sinpi / cospi over special and pseudo-random arguments, as hex: built by mcc against its own C library, and with -DREF by the host cc
   as a reference in long double (the exact reduction to [-1/4, 1/4], then sinl / cosl of pi f: 64-bit significands, so the double
   result is the correctly rounded one but in rare ties); compared in ulps by tests/sinpi.sh. (glibc 2.43's own sinpi is not a
   reference: sinpi(0.25) = 0x1.6a09e667f3bccp-1, 1 ulp below sqrt(1/2).) */
#include <stdio.h>
#ifdef REF
#include <math.h>
static void ref(double x, double *sp, double *cp)
{
    long double a = fabsl((long double)x), r = a - 2.0L * floorl(a * 0.5L), f, s, c, t;
    int n = (int)floorl(2.0L * r + 0.5L);
    f = r - 0.5L * n;
    s = sinl(3.14159265358979323846264338327950288L * f); c = cosl(3.14159265358979323846264338327950288L * f);
    if (f == 0) s = 0;
    switch (n & 3) { case 1: t = s; s = c; c = f == 0 ? 0 : -t; break; case 2: s = f == 0 ? 0 : -s; c = -c; break;
                     case 3: t = s; s = -c; c = f == 0 ? 0 : t; break; }
    *sp = signbit(x) ? -(double)s : (double)s; *cp = (double)c;
}
#define sinpi ref_sinpi
#define cospi ref_cospi
static double sinpi(double x) { double s, c; ref(x, &s, &c); return s; }
static double cospi(double x) { double s, c; ref(x, &s, &c); return c; }
#else
double sinpi(double); double cospi(double);
#endif
static unsigned long long st = 88172645463325252ULL;
static unsigned long long rnd(void) { st ^= st << 13; st ^= st >> 7; st ^= st << 17; return st; }
static void one(double x) { printf("%a %a %a\n", x, sinpi(x), cospi(x)); }
int main(void)
{
    static const double sp[] = {0.0, -0.0, 0.25, 0.5, 0.75, 1.0, 1.5, 2.0, 2.5, -0.5, -1.0, -1.5, -2.0, 1e-300, -1e-300, 0x1p-1074, 1e-10,
        0.1, 1.0 / 3.0, 0.4999999999999999, 0.5000000000000001, 123456.75, 0x1p51 + 0.5, 0x1p52 + 1.0, 0x1p53, 0x1p60, -0x1p52 - 1.0, 1e300};
    int i;
    for (i = 0; i < (int)(sizeof sp / sizeof sp[0]); i++) one(sp[i]);
    for (i = 0; i < 20000; i++) {
        unsigned long long r = rnd();
        double u = (double)(r >> 11) * 0x1p-53;               /* [0, 1) */
        one((r & 1 ? -1.0 : 1.0) * u * 4.0);                 /* (-4, 4) */
        one((r & 2 ? -1.0 : 1.0) * (double)(rnd() >> 11) * 0x1p-30);   /* up to 2^23, fractions in 2^-30 steps */
        one(u * 0x1p-20 * (r & 4 ? -1.0 : 1.0));             /* small */
    }
    return 0;
}
