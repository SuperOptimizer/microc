/* Private to the math library (math.c, mathd.c, mathf.c). The library declares everything itself
   instead of including <math.h>, because it is built against three different sets of headers. */
#ifndef MATHIMPL_H
#define MATHIMPL_H

typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;

typedef union { double f; u64 i; } math_du;
typedef union { float f; u32 i; } math_fu;
static inline u64 d2u(double x) { math_du u; u.f = x; return u.i; }
static inline double u2d(u64 i) { math_du u; u.i = i; return u.f; }
static inline u32 f2u(float x) { math_fu u; u.f = x; return u.i; }
static inline float u2f(u32 i) { math_fu u; u.i = i; return u.f; }

#define SIGN64 0x8000000000000000UL
#define INF64 0x7ff0000000000000UL
#define D_INF u2d(INF64)

/* error-free transformations: s + e = a + b exactly (FASTSUM needs |a| >= |b| or a == 0) */
#define TWOSUM(s, e, a, b) do { double a_ = (a), b_ = (b), s_ = a_ + b_, t_ = s_ - a_; \
        (e) = (a_ - (s_ - t_)) + (b_ - t_); (s) = s_; } while (0)
#define FASTSUM(s, e, a, b) do { double a_ = (a), b_ = (b), s_ = a_ + b_; (e) = (a_ - s_) + b_; (s) = s_; } while (0)
/* p + e = a * b exactly */
#define TWOPROD(p, e, a, b) do { double a_ = (a), b_ = (b), p_ = a_ * b_; (e) = fma(a_, b_, -p_); (p) = p_; } while (0)

/* Names. A program that uses only C89 may define functions called round or gamma itself, and the linker
   takes whole objects from the library: so the objects with the C89 functions (math.c, mathd.c and
   libc/sys/math_arm64.s) define nothing else but reserved names (__math_...), and call nothing else. */
double __math_fma(double, double, double); /* one instruction */
double __math_scalbn(double, int);
int __math_rem_pio2(double x, double *rh, double *rl);
double __math_scale(double hi, double lo, long e);
extern const double __math_exp_tab[128][2];
extern const double __math_log_tab[128][3];
extern const double __math_atan_tab[65][2];

#ifdef MATH_KERNELS /* mathd.c and mathx.c */
#define fma __math_fma
#define poly __math_poly
#define div_dd __math_div_dd
#define round_sub __math_round_sub
#define scale_any __math_scale_any
#define exp_tail __math_exp_tail
#define exp_dd __math_exp_dd
#define log_dd __math_log_dd
#define log_special __math_log_special
#define sin_k __math_sin_k
#define cos_k __math_cos_k
double poly(double z, const double *c, int n);
double div_dd(double nh, double nl, double dh, double dl);
double round_sub(double h, double l);
double scale_any(double h, double l, long e);
double exp_tail(long k, double r, double *lo, long *pe);
double exp_dd(double xh, double xl, double *lo, long *pe);
double log_dd(double x, double *lo, int *kp);
int log_special(double x, double *r);
double sin_k(double h, double l, double *lo);
double cos_k(double h, double l, double *lo);

#define ONE64 0x3ff0000000000000UL
#define MANT64 0x000fffffffffffffUL
#define ROUNDER 0x1.8p52 /* (x + ROUNDER) - ROUNDER rounds x to an integer */
#define PIO4_BITS 0x3fe921fb54442d18UL
#define TINY_BITS 0x3e40000000000000UL /* 2^-27 */

/* polynomials written out, the even and the odd terms separately to halve the dependency chain */
#define POLY6(z, z2, c) ((c[0] + z2 * (c[2] + z2 * c[4])) + z * (c[1] + z2 * (c[3] + z2 * c[5])))
#define POLY7(z, z2, c) ((c[0] + z2 * (c[2] + z2 * (c[4] + z2 * c[6]))) + z * (c[1] + z2 * (c[3] + z2 * c[5])))
#define POLY13(z, z2, c) ((c[0] + z2 * (c[2] + z2 * (c[4] + z2 * (c[6] + z2 * (c[8] + z2 * (c[10] + z2 * c[12])))))) \
        + z * (c[1] + z2 * (c[3] + z2 * (c[5] + z2 * (c[7] + z2 * (c[9] + z2 * c[11]))))))
#define COUNT_IS(a, n) typedef char a##_count[sizeof a == (n) * sizeof a[0] ? 1 : -1]
#endif

/* the public functions that the library itself calls */
double sqrt(double), fabs(double), floor(double), ceil(double);
double fma(double, double, double), trunc(double);
float truncf(float);
double scalbn(double, int), remquo(double, double, int *), remainder(double, double), nextafter(double, double);
double copysign(double, double), scalbln(double, long), frexp(double, int *), modf(double, double *), fmod(double, double);
double logb(double), fdim(double, double), nan(const char *);
float remainderf(float, float), exp10f(float), copysignf(float, float), scalbnf(float, int);
int ilogb(double);
double exp(double), exp2(double), exp10(double), expm1(double), log(double), log2(double), log10(double), log1p(double);
double pow(double, double), cbrt(double), hypot(double, double);
double sin(double), cos(double), tan(double), asin(double), acos(double), atan(double), atan2(double, double);
double sinh(double), cosh(double), tanh(double), asinh(double), acosh(double), atanh(double);
double erf(double), erfc(double), tgamma(double), lgamma(double), lgamma_r(double, int *);
void sincos(double, double *, double *);
extern int signgam;

#endif
