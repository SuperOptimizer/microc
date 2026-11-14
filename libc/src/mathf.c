/* The math library, part 4: single precision.
   A float function evaluated in double arithmetic is as good as correctly rounded, so the functions here
   are either short double-precision evaluations (the common ones, where speed matters: no sums of two
   doubles as in mathd.c) or simply the double function. */
#include "mathimpl.h"
#include "mathtab.h"

#define ROUNDER 0x1.8p52
#define F_INF 0x7f800000U
#define F_ABS 0x7fffffffU

/* ---- exponentials ---- */

/* exp(r) 2^(k/128) for |r| < 0.003, relative error 2^-50 */
static double exp_tail(double kd, double r)
{
    long k = (long)kd;
    double p = 1.0 + r * (1.0 + r * (0.5 + r * (0x1.5555555555555p-3 + r * (0x1.5555555555555p-5 + r * 0x1.1111111111111p-7))));
    return __math_exp_tab[k & 127][0] * p * u2d((u64)(1023 + (k >> 7)) << 52);
}

/* exp(d) for |d| < 700 */
static double exp_fast(double d)
{
    double kd = (d * EXP_INVL + ROUNDER) - ROUNDER;
    return exp_tail(kd, (d - kd * EXP_L1) - kd * EXP_L2);
}

float expf(float x)
{
    u32 ax = f2u(x) & F_ABS;
    if (ax >= 0x42b00000) { /* 88 */
        if (ax > F_INF)
            return x + x;
        if (x > 88.75f)
            return x * 0x1p127f;
        if (x < -104.0f)
            return 0.0f;
    }
    return (float)exp_fast((double)x);
}

float exp2f(float x)
{
    u32 ax = f2u(x) & F_ABS;
    double d = (double)x, kd;
    if (ax >= 0x42fe0000) { /* 127 */
        if (ax > F_INF)
            return x + x;
        if (x >= 128.0f)
            return x * 0x1p127f;
        if (x < -151.0f)
            return 0.0f;
    }
    kd = (d * 128.0 + ROUNDER) - ROUNDER;
    return (float)exp_tail(kd, (d - kd * 0.0078125) * EXP2_C);
}

float exp10f(float x)
{
    u32 ax = f2u(x) & F_ABS;
    double d = (double)x, kd;
    if (ax >= 0x42180000) { /* 38 */
        if (ax > F_INF)
            return x + x;
        if (x > 38.6f)
            return x * 0x1p127f;
        if (x < -46.0f)
            return 0.0f;
    }
    kd = (d * EXP10_INVL + ROUNDER) - ROUNDER;
    return (float)exp_tail(kd, ((d - kd * EXP10_A1) - kd * EXP10_A2) * LN10_H);
}
float expm1f(float x) { return (float)expm1((double)x); }

/* ---- logarithms ---- */

/* log(x) for finite x > 0, relative error 2^-50; with kp the binary exponent is left out and stored there */
static double log_fast(double x, int *kp)
{
    u64 ix = d2u(x), tmp = ix - LOG_OFF;
    int k = (int)((i64)tmp >> 52);
    const double *t = __math_log_tab[(tmp >> 45) & 127];
    double r = u2d(ix - (tmp & 0xfff0000000000000UL)) * t[0] - 1.0;
    double p = r * r * (-0.5 + r * (0x1.5555555555555p-2 + r * (-0.25 + r * (0x1.999999999999ap-3 + r * -0x1.5555555555555p-3))));
    if (kp) {
        *kp = k;
        return (t[1] + t[2]) + (r + p);
    }
    return ((double)k * LN2_H + (t[1] + t[2])) + (r + p);
}

/* 0 for an ordinary positive argument */
static int logf_special(float x, float *r)
{
    u32 ix = f2u(x);
    if (ix - 1 < F_INF - 1) /* positive, finite; subnormals are normal as doubles */
        return 0;
    if ((ix << 1) == 0)
        *r = -1.0f / (x * x);
    else if (ix == F_INF)
        *r = x;
    else
        *r = (x - x) / (x - x);
    return 1;
}

float logf(float x)
{
    float s;
    if (logf_special(x, &s))
        return s;
    return (float)log_fast((double)x, 0);
}

float log2f(float x)
{
    float s;
    int k;
    double l;
    if (logf_special(x, &s))
        return s;
    l = log_fast((double)x, &k);
    return (float)((double)k + l * INVLN2_H);
}

float log10f(float x)
{
    float s;
    int k;
    double l;
    if (logf_special(x, &s))
        return s;
    l = log_fast((double)x, &k);
    return (float)((double)k * (LOG10_2_H42 + LOG10_2_L42) + l * INVLN10_H);
}
float log1pf(float x) { return (float)log1p((double)x); }

float powf(float x, float y)
{
    u32 ix = f2u(x), ay = f2u(y) & F_ABS;
    double d;
    if (ix - 0x00800000 >= F_INF - 0x00800000 || ay - 1 >= F_INF - 1 || ix == 0x3f800000)
        return (float)pow((double)x, (double)y); /* the special cases are in one place */
    d = (double)y * log_fast((double)x, 0);
    if (d > 89.0)
        return 0x1p127f * 0x1p127f;
    if (d < -104.0)
        return 0x1p-100f * 0x1p-100f;
    return (float)exp_fast(d);
}

/* ---- trigonometric ---- */

static double sin_fast(double r)
{
    double z = r * r;
    return r + r * z * (-0x1.5555555555555p-3 + z * (0x1.1111111111111p-7 + z * (-0x1.a01a01a01a01ap-13
                   + z * (0x1.71de3a556c734p-19 + z * (-0x1.ae64567f544e4p-26 + z * 0x1.6124613a86d09p-33)))));
}

static double cos_fast(double r)
{
    double z = r * r;
    return 1.0 + z * (-0.5 + z * (0x1.5555555555555p-5 + z * (-0x1.6c16c16c16c17p-10 + z * (0x1.a01a01a01a01ap-16
                   + z * (-0x1.27e4fb7789f5cp-22 + z * (0x1.1eed8eff8d898p-29 + z * -0x1.93974a8c07c9dp-37))))));
}

/* x = n pi/2 + r; returns n mod 4 */
static int rem_fast(float x, double *r)
{
    double d = (double)x, kd, l;
    if ((f2u(x) & F_ABS) < 0x49800000) { /* 2^20 */
        kd = (d * INVPIO2 + ROUNDER) - ROUNDER;
        *r = (d - kd * PIO2_1) - kd * (PIO2_2 + PIO2_3);
        return (int)kd & 3;
    }
    {
        int n = __math_rem_pio2(d, r, &l);
        *r += l;
        return n;
    }
}

float sinf(float x)
{
    u32 ax = f2u(x) & F_ABS;
    double r;
    if (ax <= 0x3f490fdb) /* pi/4 */
        return ax < 0x39800000 ? x : (float)sin_fast((double)x);
    if (ax >= F_INF)
        return x - x;
    switch (rem_fast(x, &r)) {
    case 0: return (float)sin_fast(r);
    case 1: return (float)cos_fast(r);
    case 2: return (float)-sin_fast(r);
    default: return (float)-cos_fast(r);
    }
}

float cosf(float x)
{
    u32 ax = f2u(x) & F_ABS;
    double r;
    if (ax <= 0x3f490fdb)
        return (float)cos_fast((double)x);
    if (ax >= F_INF)
        return x - x;
    switch (rem_fast(x, &r)) {
    case 0: return (float)cos_fast(r);
    case 1: return (float)-sin_fast(r);
    case 2: return (float)-cos_fast(r);
    default: return (float)sin_fast(r);
    }
}

void sincosf(float x, float *sp, float *cp)
{
    u32 ax = f2u(x) & F_ABS;
    double r = (double)x, s, c;
    int n = 0;
    if (ax > 0x3f490fdb) {
        if (ax >= F_INF) {
            *sp = *cp = x - x;
            return;
        }
        n = rem_fast(x, &r);
    }
    s = sin_fast(r);
    c = cos_fast(r);
    if (ax < 0x39800000)
        s = (double)x; /* keeps -0 */
    switch (n) {
    case 0: *sp = (float)s; *cp = (float)c; break;
    case 1: *sp = (float)c; *cp = (float)-s; break;
    case 2: *sp = (float)-s; *cp = (float)-c; break;
    default: *sp = (float)-c; *cp = (float)s; break;
    }
}

float tanf(float x)
{
    u32 ax = f2u(x) & F_ABS;
    double r = (double)x, s, c;
    int n = 0;
    if (ax > 0x3f490fdb) {
        if (ax >= F_INF)
            return x - x;
        n = rem_fast(x, &r);
    } else if (ax < 0x39800000) {
        return x;
    }
    s = sin_fast(r);
    c = cos_fast(r);
    return (float)((n & 1) ? -c / s : s / c);
}

/* atan(t) for 0 <= t <= 1 */
static double atan_fast(double t)
{
    int i = (int)(t * 64.0 + 0.5);
    double c = (double)i * 0.015625, u = (t - c) / (1.0 + t * c), z = u * u;
    return __math_atan_tab[i][0] + (u + u * z * (-0x1.5555555555555p-2 + z * (0x1.999999999999ap-3 + z * -0x1.2492492492492p-3)));
}

float atanf(float x)
{
    u32 ax = f2u(x) & F_ABS;
    double a = (double)u2f(ax), r;
    if (ax > F_INF)
        return x + x;
    if (ax < 0x39800000)
        return x;
    r = a <= 1.0 ? atan_fast(a) : PIO2_H - atan_fast(1.0 / a);
    return (float)(x < 0 ? -r : r);
}

float atan2f(float y, float x)
{
    u32 ix = f2u(x), iy = f2u(y), ax = ix & F_ABS, ay = iy & F_ABS;
    double a, b, r;
    if (ax - 1 >= F_INF - 1 || ay - 1 >= F_INF - 1) /* a zero, an infinity or a NaN */
        return (float)atan2((double)y, (double)x);
    a = (double)u2f(ax);
    b = (double)u2f(ay);
    if (b <= a) {
        r = atan_fast(b / a);
        if (ix >> 31)
            r = PI_H - r;
    } else {
        r = atan_fast(a / b);
        r = (ix >> 31) ? PIO2_H + r : PIO2_H - r;
    }
    return (float)((iy >> 31) ? -r : r);
}

float asinf(float x) { return (float)asin((double)x); }
float acosf(float x) { return (float)acos((double)x); }

/* ---- hyperbolic ---- */

float tanhf(float x)
{
    u32 ax = f2u(x) & F_ABS;
    double a = (double)u2f(ax), r;
    if (ax > F_INF)
        return x + x;
    if (ax < 0x3c000000) { /* 2^-7: x - x^3/3 + 2 x^5/15 */
        r = a * a;
        r = a - a * r * (0x1.5555555555555p-2 - r * 0x1.1111111111111p-3);
    } else if (ax >= 0x41200000) { /* 10 */
        r = 1.0;
    } else {
        r = 1.0 - 2.0 / (exp_fast(a + a) + 1.0);
    }
    return (float)((f2u(x) >> 31) ? -r : r);
}

float sinhf(float x) { return (float)sinh((double)x); }
float coshf(float x) { return (float)cosh((double)x); }
float asinhf(float x) { return (float)asinh((double)x); }
float acoshf(float x) { return (float)acosh((double)x); }
float atanhf(float x) { return (float)atanh((double)x); }

/* ---- the rest ---- */

float cbrtf(float x) { return (float)cbrt((double)x); }

float hypotf(float x, float y)
{
    u32 ax = f2u(x) & F_ABS, ay = f2u(y) & F_ABS;
    double a = (double)x, b = (double)y;
    if (ax == F_INF || ay == F_INF)
        return u2f(F_INF);
    return (float)sqrt(a * a + b * b);
}

float erff(float x) { return (float)erf((double)x); }
float erfcf(float x) { return (float)erfc((double)x); }
float tgammaf(float x) { return (float)tgamma((double)x); }
float lgammaf_r(float x, int *sign) { return (float)lgamma_r((double)x, sign); }
float lgammaf(float x) { return (float)lgamma_r((double)x, &signgam); }
