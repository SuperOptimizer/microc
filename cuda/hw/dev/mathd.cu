/* The device library's double-precision functions: the C library's own (libc/src/math.c, mathd.c, mathx.c;
   mklib.sh makes the copies included here, in which every function is device code), under the names
   mcdd_NAME, with their tables; and written out once each as
   mcdw_NAME, of which mklib makes the piece __mcuda_NAME (see mathf.cu). */
#define MCDD(X) X(frexp) X(ldexp) X(modf) X(fmod) X(exp) X(exp2) X(exp10) X(expm1) X(log) X(log2) X(log10) X(log1p) \
    X(pow) X(cbrt) X(hypot) X(sin) X(cos) X(tan) X(asin) X(acos) X(atan) X(atan2) X(sinh) X(cosh) X(tanh) X(asinh) \
    X(acosh) X(atanh) X(erf) X(erfc) X(tgamma) X(lgamma) X(lgamma_r) X(sincos)
#define frexp mcdd_frexp
#define ldexp mcdd_ldexp
#define modf mcdd_modf
#define fmod mcdd_fmod
#define exp mcdd_exp
#define exp2 mcdd_exp2
#define exp10 mcdd_exp10
#define expm1 mcdd_expm1
#define log mcdd_log
#define log2 mcdd_log2
#define log10 mcdd_log10
#define log1p mcdd_log1p
#define pow mcdd_pow
#define cbrt mcdd_cbrt
#define hypot mcdd_hypot
#define sin mcdd_sin
#define cos mcdd_cos
#define tan mcdd_tan
#define asin mcdd_asin
#define acos mcdd_acos
#define atan mcdd_atan
#define atan2 mcdd_atan2
#define sinh mcdd_sinh
#define cosh mcdd_cosh
#define tanh mcdd_tanh
#define asinh mcdd_asinh
#define acosh mcdd_acosh
#define atanh mcdd_atanh
#define erf mcdd_erf
#define erfc mcdd_erfc
#define tgamma mcdd_tgamma
#define lgamma mcdd_lgamma
#define lgamma_r mcdd_lgamma_r
#define sincos mcdd_sincos
#define scalbn mcdd_scalbn
#define copysign mcdd_copysign
#define signgam mcdd_signgam
#define __huge_val mcdd_huge_val
#define __math_exp_tab mcdd_exp_tab
#define __math_log_tab mcdd_log_tab
#define __math_atan_tab mcdd_atan_tab
#define __math_fma mcdd_fma
#undef __linux__ /* (math.c: no assembly) */
#define MATH_KERNELS
#include "mathimpl.h"
__device__ static __forceinline__ double mcdd_fma_(double a, double b, double c) { return __fma_rn(a, b, c); }
#undef fma
#define fma mcdd_fma_
#include "math.c"
__host__ __device__ double mcdd_scalbn(double x, int n) { return __math_scalbn(x, n); }
__host__ __device__ double mcdd_copysign(double x, double y) { return u2d((d2u(x) & ~SIGN64) | (d2u(y) & SIGN64)); }
#include "mathd.c"
#undef MATHTAB_CORE
#include "mathx.c"

#define OUT __device__ __attribute__((noinline))
#define D1(X) X(exp) X(exp2) X(exp10) X(expm1) X(log) X(log2) X(log10) X(log1p) X(cbrt) X(sin) X(cos) X(tan) X(asin) \
    X(acos) X(atan) X(sinh) X(cosh) X(tanh) X(asinh) X(acosh) X(atanh) X(erf) X(erfc) X(tgamma)
#define D2(X) X(pow) X(hypot) X(atan2) X(fmod)
#define X(n) OUT double mcdw_##n(double x) { return mcdd_##n(x); }
D1(X)
#undef X
#define X(n) OUT double mcdw_##n(double x, double y) { return mcdd_##n(x, y); }
D2(X)
#undef X
OUT double mcdw_frexp(double x, int *e) { return mcdd_frexp(x, e); }
OUT double mcdw_ldexp(double x, int n) { return __math_scalbn(x, n); }
OUT double mcdw_scalbn(double x, int n) { return __math_scalbn(x, n); }
OUT double mcdw_scalbln(double x, long n) { return __math_scalbn(x, n > 5000 ? 5000 : n < -5000 ? -5000 : (int)n); }
OUT double mcdw_modf(double x, double *ip) { return mcdd_modf(x, ip); }
OUT void mcdw_sincos(double x, double *s, double *c) { mcdd_sincos(x, s, c); }
/* (the sign of gamma goes nowhere on a device: there is no signgam that threads could share) */
OUT double mcdw_lgamma(double x) { int s; return mcdd_lgamma_r(x, &s); }
OUT double mcdw_lgamma_r(double x, int *s) { return mcdd_lgamma_r(x, s); }
OUT double mcdw_copysign(double x, double y) { return mcdd_copysign(x, y); }
OUT double mcdw_fdim(double x, double y) { return x != x || y != y ? x + y : x > y ? x - y : 0.0; }
OUT double mcdw_nan(const char *s) { (void)s; return u2d(0x7ff8000000000000UL); }
/* half-way cases away from zero: |x| + 0.5 is exact below 2^52 except just under 0.5, which the cut settles */
__device__ static __forceinline__ double mcdd_round(double x)
{
    u64 b = d2u(x), a = b & ~SIGN64;
    double r;
    if (a >= 0x4330000000000000UL) /* an integer already, infinite or NaN */
        return x + 0.0;
    r = trunc(u2d(a));
    if (u2d(a) - r >= 0.5)
        r += 1.0;
    return u2d(d2u(r) | (b & SIGN64));
}
OUT double mcdw_round(double x) { return mcdd_round(x); }
__device__ static __forceinline__ long mcdd_tolong(double r)
{
    /* as the processors of the host do it: what does not fit is the least long */
    if (!(r >= -0x1p63 && r < 0x1p63))
        return (long)(1UL << 63);
    return (long)r;
}
OUT long mcdw_lround(double x) { return mcdd_tolong(mcdd_round(x)); }
OUT long long mcdw_llround(double x) { return mcdd_tolong(mcdd_round(x)); }
OUT long mcdw_lrint(double x) { return mcdd_tolong(rint(x)); }
OUT long long mcdw_llrint(double x) { return mcdd_tolong(rint(x)); }
OUT double mcdw_nearbyint(double x) { return rint(x); }
OUT int mcdw_ilogb(double x)
{
    u64 b = d2u(x) & ~SIGN64;
    int e = (int)(b >> 52);
    if (b == 0 || b > INF64)
        return -2147483647 - 1;
    if (e == 0x7ff)
        return 2147483647;
    if (e == 0)
        return -1011 - clz64(b);
    return e - 1023;
}
OUT double mcdw_logb(double x)
{
    u64 b = d2u(x) & ~SIGN64;
    if (b == 0)
        return -1.0 / (x * x);
    if (b >= INF64)
        return x * x;
    return (double)mcdw_ilogb(x);
}
OUT double mcdw_remquo(double x, double y, int *quo)
{
    u64 ux = d2u(x), uy = d2u(y), mx, my, q = 0;
    int ex, ey, n, neg = 0;
    double r;
    *quo = 0;
    if ((uy << 1) == 0 || (ux << 1) >= (INF64 << 1) || (uy << 1) > (INF64 << 1))
        return (x * y) / (x * y);
    if ((ux << 1) == 0 || (uy << 1) == (INF64 << 1))
        return x;
    ex = decompose(ux, &mx);
    ey = decompose(uy, &my);
    n = ex - ey;
    if (n < 0) {
        if (n < -1 || mx <= my)
            return x;
        mx = 2 * my - mx;
        q = 1;
        neg = 1;
        ey = ex;
    } else {
        q = mx / my;
        mx %= my;
        for (; n > 0; n -= 11) {
            int s = n < 11 ? n : 11;
            u64 t = mx << s;
            q = (q << s) | (t / my);
            mx = t % my;
        }
        if (2 * mx > my || (2 * mx == my && (q & 1))) {
            mx = my - mx;
            q++;
            neg = 1;
        }
    }
    r = __math_scalbn((double)mx, ey - 1075);
    if (neg)
        r = -r;
    if ((ux ^ uy) >> 63)
        *quo = -(int)(q & 0x7fffffff);
    else
        *quo = (int)(q & 0x7fffffff);
    if (r == 0)
        return mcdd_copysign(0.0, x);
    return (ux >> 63) ? -r : r;
}
OUT double mcdw_remainder(double x, double y) { int q; return mcdw_remquo(x, y, &q); }

__global__ void mcd_all_mathd(double *out, int *ip, long *lp)
{
    double x = out[0], y = out[1], s, c;
#define X(n) x += mcdw_##n(x);
    D1(X)
#undef X
#define X(n) x += mcdw_##n(x, y);
    D2(X)
#undef X
    x += mcdw_frexp(x, ip) + mcdw_ldexp(x, *ip) + mcdw_scalbn(x, *ip) + mcdw_scalbln(x, *lp) + mcdw_modf(x, &y);
    mcdw_sincos(x, &s, &c);
    x += s + c + mcdw_lgamma(x) + mcdw_lgamma_r(x, ip) + mcdw_copysign(x, y) + mcdw_fdim(x, y) + mcdw_nan((const char *)0) + mcdw_round(x);
    lp[0] = mcdw_lround(x) + mcdw_llround(x) + mcdw_lrint(x) + mcdw_llrint(x) + mcdw_ilogb(x);
    out[0] = x + mcdw_nearbyint(x) + mcdw_logb(x) + mcdw_remquo(x, y, ip) + mcdw_remainder(x, y);
}
extern "C" void mcd_unit_mathd(void);
void mcd_unit_mathd(void) { mcd_all_mathd<<<1, 1>>>((double *)0, (int *)0, (long *)0); }
