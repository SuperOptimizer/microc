#include "abi.h"
typedef __builtin_va_list va_list;
#define va_start(a, b) __builtin_va_start(a, b)
#define va_arg(a, t) __builtin_va_arg(a, t)
#define va_end(a) __builtin_va_end(a)

#define DEF(T) \
    T make_##T(long seed) { T s; fill(&s, (int)sizeof s, seed); return s; } \
    long sum_##T(T s) { return hash(&s, (int)sizeof s); } \
    long take0_##T(T s) { return sum_##T(s); } \
    long take5_##T(int a, int b, int c, int d, int e, T s, long z) { return a + 2 * b + 3 * c + 4 * d + 5 * e + sum_##T(s) + 7 * z; } \
    long take7d_##T(double a, double b, double c, double d, double e, double f, double g, T s, double z) \
    { return (long)(a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g + 9 * z) + sum_##T(s); } \
    long takemix_##T(T s, int a, double b, T t, long c, T u, float d, T v, T w) \
    { return sum_##T(s) + 3 * sum_##T(t) + 5 * sum_##T(u) + 7 * sum_##T(v) + 11 * sum_##T(w) + a + (long)b + c + (long)d; } \
    long takestk_##T(long a, long b, long c, long d, long e, long f, double g, double h, double i, double j, double k, double l, double m, double n, T s, char y, T t) \
    { return a + b + c + d + e + f + (long)(g + h + i + j + k + l + m + n) + sum_##T(s) + y + 3 * sum_##T(t); } \
    T ret_##T(long seed) { return make_##T(seed); } \
    T pass_##T(int a, T s, double b) { T t = s; (void)a; (void)b; return t; } \
    long var_##T(int n, ...) \
    { \
        va_list ap; \
        long r = 0; \
        int k; \
        va_start(ap, n); \
        for (k = 0; k < n; k++) { \
            T s = va_arg(ap, T); \
            r = r * 3 + sum_##T(s) + va_arg(ap, int) + (long)va_arg(ap, double); \
        } \
        va_end(ap); \
        return r; \
    }
TYPES(DEF)

long var_mixed(const char *fmt, ...)
{
    va_list ap;
    long r = 0;
    va_start(ap, fmt);
    for (; *fmt; fmt++) {
        r *= 3;
        if (*fmt == 'i')
            r += va_arg(ap, int);
        else if (*fmt == 'l')
            r += va_arg(ap, long);
        else if (*fmt == 'd')
            r += (long)(va_arg(ap, double) * 2);
        else if (*fmt == 'p')
            r += *va_arg(ap, int *);
        else if (*fmt == 'q')
            r += (long)(va_arg(ap, __int128) >> 64);
    }
    va_end(ap);
    return r;
}

long many(char a, short b, int c, long d, unsigned char e, unsigned short f, unsigned g, unsigned long h, float i, double j,
          signed char k, long l, long m, float n, double o, double p, double q, double r, double s, double t, int u, float v)
{
    return a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * (long)g + 8 * (long)h + (long)(9 * i) + (long)(10 * j) + 11 * k + 12 * l +
           13 * m + (long)(14 * n) + (long)(15 * o + 16 * p + 17 * q + 18 * r + 19 * s + 20 * t) + 21 * u + (long)(22 * v);
}

/* (not returning __int128: mcc returns only the low half of one, on every target) */
long add128(__int128 a, long b, __int128 c, int d, int e, int f, __int128 g)
{
    __int128 r = a + b + c * 3 + d + e + f + g * 5;
    return (long)(r >> 64) * 1000 + (long)r;
}
