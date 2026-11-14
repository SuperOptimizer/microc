/* The math library: the exact functions that C99 added (rounding to integers, remainders, scaling,
   neighbours, classification) and the floating-point environment. See math.c. */
#include "mathimpl.h"

#if defined(__linux__) && !defined(__x86_64__) /* x86-64: libc/linux/sys/x86_64 */
/* BEGIN generated stubs */
__asm__(".text\n.balign 4\n"
    ".globl fma\n.type fma,%function\n.globl fmal\n.type fmal,%function\nfma:\nfmal:\n fmadd d0, d0, d1, d2\n ret\n"
    ".globl fmaf\n.type fmaf,%function\nfmaf:\n fmadd s0, s0, s1, s2\n ret\n"
    ".globl sqrtf\n.type sqrtf,%function\nsqrtf:\n fsqrt s0, s0\n ret\n"
    ".globl fabsf\n.type fabsf,%function\nfabsf:\n fabs s0, s0\n ret\n"
    ".globl floorl\n.type floorl,%function\nfloorl:\n frintm d0, d0\n ret\n"
    ".globl floorf\n.type floorf,%function\nfloorf:\n frintm s0, s0\n ret\n"
    ".globl ceill\n.type ceill,%function\nceill:\n frintp d0, d0\n ret\n"
    ".globl ceilf\n.type ceilf,%function\nceilf:\n frintp s0, s0\n ret\n"
    ".globl trunc\n.type trunc,%function\n.globl truncl\n.type truncl,%function\ntrunc:\ntruncl:\n frintz d0, d0\n ret\n"
    ".globl truncf\n.type truncf,%function\ntruncf:\n frintz s0, s0\n ret\n"
    ".globl round\n.type round,%function\n.globl roundl\n.type roundl,%function\nround:\nroundl:\n frinta d0, d0\n ret\n"
    ".globl roundf\n.type roundf,%function\nroundf:\n frinta s0, s0\n ret\n"
    ".globl rint\n.type rint,%function\n.globl rintl\n.type rintl,%function\nrint:\nrintl:\n frintx d0, d0\n ret\n"
    ".globl rintf\n.type rintf,%function\nrintf:\n frintx s0, s0\n ret\n"
    ".globl nearbyint\n.type nearbyint,%function\n.globl nearbyintl\n.type nearbyintl,%function\nnearbyint:\nnearbyintl:\n frinti d0, d0\n ret\n"
    ".globl nearbyintf\n.type nearbyintf,%function\nnearbyintf:\n frinti s0, s0\n ret\n"
    ".globl lrint\n.type lrint,%function\n.globl llrint\n.type llrint,%function\n.globl lrintl\n.type lrintl,%function\n.globl llrintl\n.type llrintl,%function\nlrint:\nllrint:\nlrintl:\nllrintl:\n frintx d0, d0\n fcvtzs x0, d0\n ret\n"
    ".globl lrintf\n.type lrintf,%function\n.globl llrintf\n.type llrintf,%function\nlrintf:\nllrintf:\n frintx s0, s0\n fcvtzs x0, s0\n ret\n"
    ".globl lround\n.type lround,%function\n.globl llround\n.type llround,%function\n.globl lroundl\n.type lroundl,%function\n.globl llroundl\n.type llroundl,%function\nlround:\nllround:\nlroundl:\nllroundl:\n fcvtas x0, d0\n ret\n"
    ".globl lroundf\n.type lroundf,%function\n.globl llroundf\n.type llroundf,%function\nlroundf:\nllroundf:\n fcvtas x0, s0\n ret\n"
    ".globl fmin\n.type fmin,%function\n.globl fminl\n.type fminl,%function\nfmin:\nfminl:\n fminnm d0, d0, d1\n ret\n"
    ".globl fminf\n.type fminf,%function\nfminf:\n fminnm s0, s0, s1\n ret\n"
    ".globl fmax\n.type fmax,%function\n.globl fmaxl\n.type fmaxl,%function\nfmax:\nfmaxl:\n fmaxnm d0, d0, d1\n ret\n"
    ".globl fmaxf\n.type fmaxf,%function\nfmaxf:\n fmaxnm s0, s0, s1\n ret\n"
    ".globl __math_getfpcr\n.type __math_getfpcr,%function\n__math_getfpcr:\n mrs x0, fpcr\n ret\n"
    ".globl __math_setfpcr\n.type __math_setfpcr,%function\n__math_setfpcr:\n msr fpcr, x0\n ret\n"
    ".globl __math_getfpsr\n.type __math_getfpsr,%function\n__math_getfpsr:\n mrs x0, fpsr\n ret\n"
    ".globl __math_setfpsr\n.type __math_setfpsr,%function\n__math_setfpsr:\n msr fpsr, x0\n ret\n");
/* END generated stubs */
#endif

/* ---- floating-point environment (fenv.h) ---- */
u64 __math_getfpcr(void), __math_getfpsr(void);
void __math_setfpcr(u64), __math_setfpsr(u64);

int fegetround(void) { return (int)(__math_getfpcr() & 0xc00000); }
int fesetround(int r)
{
    if (r & ~0xc00000)
        return 1;
    __math_setfpcr((__math_getfpcr() & ~0xc00000UL) | (u64)r);
    return 0;
}
int feclearexcept(int e) { __math_setfpsr(__math_getfpsr() & ~(u64)(e & 0x9f)); return 0; }
int feraiseexcept(int e) { __math_setfpsr(__math_getfpsr() | (u64)(e & 0x9f)); return 0; }
int fetestexcept(int e) { return (int)(__math_getfpsr() & (u64)(e & 0x9f)); }
int fegetexceptflag(unsigned int *f, int e) { *f = (unsigned int)fetestexcept(e); return 0; }
int fesetexceptflag(const unsigned int *f, int e)
{
    __math_setfpsr((__math_getfpsr() & ~(u64)(e & 0x9f)) | (*f & e & 0x9f));
    return 0;
}
int fegetenv(u64 *env) { env[0] = __math_getfpcr(); env[1] = __math_getfpsr(); return 0; }
int fesetenv(const u64 *env)
{
    if (env == (const u64 *)-1) {
        __math_setfpcr(0);
        __math_setfpsr(0);
    } else {
        __math_setfpcr(env[0]);
        __math_setfpsr(env[1]);
    }
    return 0;
}
int feholdexcept(u64 *env) { fegetenv(env); __math_setfpsr(0); return 0; }
int feupdateenv(const u64 *env) { u64 f = __math_getfpsr() & 0x9f; fesetenv(env); feraiseexcept((int)f); return 0; }

/* ---- classification ---- */
int __fpclassify(double x)
{
    u64 u = d2u(x) & ~SIGN64;
    if (u == 0) return 2;                   /* FP_ZERO */
    if (u < 0x0010000000000000UL) return 3; /* FP_SUBNORMAL */
    if (u < INF64) return 4;                /* FP_NORMAL */
    return u == INF64;                      /* FP_INFINITE 1, FP_NAN 0 */
}
int __fpclassifyf(float x)
{
    u32 u = f2u(x) & 0x7fffffff;
    if (u == 0) return 2;
    if (u < 0x00800000) return 3;
    if (u < 0x7f800000) return 4;
    return u == 0x7f800000;
}
int __signbit(double x) { return (int)(d2u(x) >> 63); }
int __signbitf(float x) { return (int)(f2u(x) >> 31); }
int __isnan(double x) { return x != x; }
int __isnanf(float x) { return x != x; }
int __isinf(double x) { return (d2u(x) & ~SIGN64) == INF64; }
int __isinff(float x) { return (f2u(x) & 0x7fffffff) == 0x7f800000; }
int __finite(double x) { return (d2u(x) & ~SIGN64) < INF64; }
int __finitef(float x) { return (f2u(x) & 0x7fffffff) < 0x7f800000; }

/* ---- signs, differences ---- */
double copysign(double x, double y) { return u2d((d2u(x) & ~SIGN64) | (d2u(y) & SIGN64)); }
float copysignf(float x, float y) { return u2f((f2u(x) & 0x7fffffff) | (f2u(y) & 0x80000000)); }
double fdim(double x, double y) { return x != x || y != y ? x + y : x > y ? x - y : 0.0; }
float fdimf(float x, float y) { return x != x || y != y ? x + y : x > y ? x - y : 0.0f; }
double nan(const char *s) { (void)s; return u2d(0x7ff8000000000000UL); }
float nanf(const char *s) { (void)s; return u2f(0x7fc00000); }

/* ---- exponent manipulation ---- */
static int clz64(u64 x)
{
    int n = 0;
    if (!(x >> 32)) { n += 32; x <<= 32; }
    if (!(x >> 48)) { n += 16; x <<= 16; }
    if (!(x >> 56)) { n += 8; x <<= 8; }
    if (!(x >> 60)) { n += 4; x <<= 4; }
    if (!(x >> 62)) { n += 2; x <<= 2; }
    if (!(x >> 63)) n += 1;
    return n;
}

float frexpf(float x, int *exp) { return (float)frexp((double)x, exp); }

double scalbn(double x, int n) { return __math_scalbn(x, n); }
double scalbln(double x, long n) { return scalbn(x, n > 5000 ? 5000 : n < -5000 ? -5000 : (int)n); }
float scalbnf(float x, int n)
{
    if (n > 400) n = 400;
    if (n < -400) n = -400;
    return (float)scalbn((double)x, n); /* a float scaled in double is exact until the one final rounding */
}
float ldexpf(float x, int n) { return scalbnf(x, n); }
float scalblnf(float x, long n) { return scalbnf(x, n > 400 ? 400 : n < -400 ? -400 : (int)n); }

int ilogb(double x)
{
    u64 b = d2u(x) & ~SIGN64;
    int e = (int)(b >> 52);
    if (b == 0 || b > INF64)
        return -2147483647 - 1; /* FP_ILOGB0, FP_ILOGBNAN */
    if (e == 0x7ff)
        return 2147483647;
    if (e == 0)
        return -1011 - clz64(b);
    return e - 1023;
}
int ilogbf(float x) { return ilogb((double)x); }
double logb(double x)
{
    u64 b = d2u(x) & ~SIGN64;
    if (b == 0)
        return -1.0 / (x * x);
    if (b >= INF64)
        return x * x;
    return (double)ilogb(x);
}
float logbf(float x) { return (float)logb((double)x); }

float modff(float x, float *ip)
{
    float i = truncf(x);
    *ip = i;
    if (x - x != 0)
        return x != x ? x : copysignf(0.0f, x);
    return copysignf(x - i, x);
}

/* ---- remainders (see fmod in math.c) ---- */

/* splits |x| into mx * 2^(ex - 1075); returns ex */
static int decompose(u64 ux, u64 *mx)
{
    int e = (int)((ux >> 52) & 0x7ff);
    u64 m = ux & 0x000fffffffffffffUL;
    if (e == 0)
        e = 1;
    else
        m |= 1UL << 52;
    *mx = m;
    return e;
}

float fmodf(float x, float y) { return (float)fmod((double)x, (double)y); } /* the result is a float: exact */

double remquo(double x, double y, int *quo)
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
        /* |x| < |y|: the quotient is 0 or 1 */
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
    r = scalbn((double)mx, ey - 1075);
    if (neg)
        r = -r;
    if ((ux ^ uy) >> 63)
        *quo = -(int)(q & 0x7fffffff);
    else
        *quo = (int)(q & 0x7fffffff);
    if (r == 0)
        return copysign(0.0, x);
    return (ux >> 63) ? -r : r;
}
double remainder(double x, double y) { int q; return remquo(x, y, &q); }
float remquof(float x, float y, int *quo) { return (float)remquo((double)x, (double)y, quo); }
float remainderf(float x, float y)
{
    int q;
    return (float)remquo((double)x, (double)y, &q);
}

/* ---- neighbours ---- */
double nextafter(double x, double y)
{
    u64 ux = d2u(x), uy = d2u(y);
    if (x != x || y != y)
        return x + y;
    if (x == y)
        return y;
    if ((ux << 1) == 0)
        return u2d((uy & SIGN64) | 1);
    if ((x < y) == (x > 0))
        ux++;
    else
        ux--;
    return u2d(ux);
}
float nextafterf(float x, float y)
{
    u32 ux = f2u(x), uy = f2u(y);
    if (x != x || y != y)
        return x + y;
    if (x == y)
        return y;
    if ((ux << 1) == 0)
        return u2f((uy & 0x80000000) | 1);
    if ((x < y) == (x > 0))
        ux++;
    else
        ux--;
    return u2f(ux);
}
double nexttoward(double x, double y) { return nextafter(x, y); } /* long double is double */
float nexttowardf(float x, double y)
{
    u32 ux = f2u(x);
    if (x != x || y != y)
        return (float)(x + y);
    if ((double)x == y)
        return (float)y;
    if ((ux << 1) == 0)
        return u2f(((u32)(d2u(y) >> 63) << 31) | 1);
    if (((double)x < y) == (x > 0))
        ux++;
    else
        ux--;
    return u2f(ux);
}

/* ---- long double is double ---- */
double sqrtl(double x) { return sqrt(x); }
double fabsl(double x) { return fabs(x); }
double copysignl(double x, double y) { return copysign(x, y); }
double fdiml(double x, double y) { return fdim(x, y); }
double nanl(const char *s) { return nan(s); }
double frexpl(double x, int *e) { return frexp(x, e); }
double ldexpl(double x, int n) { return scalbn(x, n); }
double scalbnl(double x, int n) { return scalbn(x, n); }
double scalblnl(double x, long n) { return scalbln(x, n); }
int ilogbl(double x) { return ilogb(x); }
double logbl(double x) { return logb(x); }
double modfl(double x, double *ip) { return modf(x, ip); }
double fmodl(double x, double y) { return fmod(x, y); }
double remquol(double x, double y, int *q) { return remquo(x, y, q); }
double remainderl(double x, double y) { return remainder(x, y); }
double nextafterl(double x, double y) { return nextafter(x, y); }
double nexttowardl(double x, double y) { return nextafter(x, y); }
