#ifndef _MATH_H
#define _MATH_H

typedef float float_t;
typedef double double_t;

#define HUGE_VAL (1e300 * 1e300)
#define HUGE_VALF ((float)(1e300 * 1e300))
#define HUGE_VALL ((long double)(1e300 * 1e300))
#define INFINITY ((float)(1e300 * 1e300))
#define NAN (0.0f / 0.0f)

#define FP_NAN 0
#define FP_INFINITE 1
#define FP_ZERO 2
#define FP_SUBNORMAL 3
#define FP_NORMAL 4
#define FP_ILOGB0 (-2147483647 - 1)
#define FP_ILOGBNAN (-2147483647 - 1)

#define MATH_ERRNO 1
#define MATH_ERREXCEPT 2
#define math_errhandling 0

#define M_E 2.7182818284590452354
#define M_LOG2E 1.4426950408889634074
#define M_LOG10E 0.43429448190325182765
#define M_LN2 0.69314718055994530942
#define M_LN10 2.30258509299404568402
#define M_PI 3.14159265358979323846
#define M_PI_2 1.57079632679489661923
#define M_PI_4 0.78539816339744830962
#define M_1_PI 0.31830988618379067154
#define M_2_PI 0.63661977236758134308
#define M_2_SQRTPI 1.12837916709551257390
#define M_SQRT2 1.41421356237309504880
#define M_SQRT1_2 0.70710678118654752440

int __fpclassify(double), __fpclassifyf(float), __signbit(double), __signbitf(float);
int __isnan(double), __isnanf(float), __isinf(double), __isinff(float), __finite(double), __finitef(float);

/* these take float, double or long double (which is double here) */
#define fpclassify(x) (sizeof(x) == sizeof(float) ? __fpclassifyf(x) : __fpclassify(x))
#define signbit(x) (sizeof(x) == sizeof(float) ? __signbitf(x) : __signbit(x))
#define isnormal(x) (fpclassify(x) == FP_NORMAL)
#define isnan(x) __extension__({ __typeof__(x) __x = (x); __x != __x; })
#define isfinite(x) __extension__({ __typeof__(x) __x = (x); __x - __x == 0; })
#define isinf(x) __extension__({ __typeof__(x) __x = (x); __x == __x && __x - __x != 0; })
#define isunordered(x, y) __extension__({ __typeof__(x) __x = (x); __typeof__(y) __y = (y); __x != __x || __y != __y; })
#define isgreater(x, y) ((x) > (y))
#define isgreaterequal(x, y) ((x) >= (y))
#define isless(x, y) ((x) < (y))
#define islessequal(x, y) ((x) <= (y))
#define islessgreater(x, y) __extension__({ __typeof__(x) __x = (x); __typeof__(y) __y = (y); __x < __y || __x > __y; })

double sin(double), cos(double), tan(double), asin(double), acos(double), atan(double), sinh(double);
double cosh(double), tanh(double), asinh(double), acosh(double), atanh(double), exp(double), exp2(double);
double expm1(double), exp10(double), pow10(double), log(double), log2(double), log10(double), log1p(double);
double sqrt(double), cbrt(double), fabs(double), floor(double), ceil(double), trunc(double), round(double);
double rint(double), nearbyint(double), logb(double), erf(double), erfc(double), tgamma(double);
double lgamma(double), gamma(double);
double atan2(double, double), pow(double, double), hypot(double, double), fmod(double, double);
double remainder(double, double), drem(double, double), fmin(double, double), fmax(double, double);
double fdim(double, double), copysign(double, double), nextafter(double, double);
double fma(double, double, double), remquo(double, double, int *), frexp(double, int *), ldexp(double, int), scalbn(double, int);
double scalbln(double, long), modf(double, double *), nan(const char *), nexttoward(double, long double);
double lgamma_r(double, int *);
long lrint(double), lround(double);
long long llrint(double), llround(double);
int ilogb(double);
void sincos(double, double *, double *);

float sinf(float), cosf(float), tanf(float), asinf(float), acosf(float), atanf(float), sinhf(float);
float coshf(float), tanhf(float), asinhf(float), acoshf(float), atanhf(float), expf(float), exp2f(float);
float expm1f(float), exp10f(float), pow10f(float), logf(float), log2f(float), log10f(float), log1pf(float);
float sqrtf(float), cbrtf(float), fabsf(float), floorf(float), ceilf(float), truncf(float), roundf(float);
float rintf(float), nearbyintf(float), logbf(float), erff(float), erfcf(float), tgammaf(float);
float lgammaf(float), gammaf(float);
float atan2f(float, float), powf(float, float), hypotf(float, float), fmodf(float, float);
float remainderf(float, float), dremf(float, float), fminf(float, float), fmaxf(float, float);
float fdimf(float, float), copysignf(float, float), nextafterf(float, float);
float fmaf(float, float, float), remquof(float, float, int *), frexpf(float, int *), ldexpf(float, int), scalbnf(float, int);
float scalblnf(float, long), modff(float, float *), nanf(const char *), nexttowardf(float, long double);
float lgammaf_r(float, int *);
long lrintf(float), lroundf(float);
long long llrintf(float), llroundf(float);
int ilogbf(float);
void sincosf(float, float *, float *);

long double sinl(long double), cosl(long double), tanl(long double), asinl(long double), acosl(long double);
long double atanl(long double), sinhl(long double), coshl(long double), tanhl(long double), asinhl(long double);
long double acoshl(long double), atanhl(long double), expl(long double), exp2l(long double), expm1l(long double);
long double exp10l(long double), pow10l(long double), logl(long double), log2l(long double), log10l(long double);
long double log1pl(long double), sqrtl(long double), cbrtl(long double), fabsl(long double), floorl(long double);
long double ceill(long double), truncl(long double), roundl(long double), rintl(long double);
long double nearbyintl(long double), logbl(long double), erfl(long double), erfcl(long double);
long double tgammal(long double), lgammal(long double), gammal(long double);
long double atan2l(long double, long double), powl(long double, long double), hypotl(long double, long double);
long double fmodl(long double, long double), remainderl(long double, long double);
long double dreml(long double, long double), fminl(long double, long double), fmaxl(long double, long double);
long double fdiml(long double, long double), copysignl(long double, long double);
long double nextafterl(long double, long double);
long double fmal(long double, long double, long double), remquol(long double, long double, int *), frexpl(long double, int *), ldexpl(long double, int), scalbnl(long double, int);
long double scalblnl(long double, long), modfl(long double, long double *), nanl(const char *), nexttowardl(long double, long double);
long double lgammal_r(long double, int *);
long lrintl(long double), lroundl(long double);
long long llrintl(long double), llroundl(long double);
int ilogbl(long double);
void sincosl(long double, long double *, long double *);

int finite(double), finitef(float), isnanf(float), isinff(float);
extern int signgam;

#endif
