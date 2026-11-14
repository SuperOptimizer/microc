/* The math library, part 1: the functions of C89 that are exact (the rest of these is in math99.c).
   The transcendental functions are in mathd.c and mathx.c (double) and mathf.c (float).
   One-instruction functions (sqrt, floor, fma, lrint, fmin ...) are assembly: libc/sys/math*_arm64.s
   for Darwin, the blocks in this file and in math99.c for Linux; all come from tests/ext/libm/mkasm.py. */
#include "mathimpl.h"

double __huge_val = 1e300 * 1e300;

#if defined(__linux__) && !defined(__x86_64__) /* x86-64: libc/linux/sys/x86_64 */
/* BEGIN generated stubs */
__asm__(".text\n.balign 4\n"
    ".globl __math_fma\n.type __math_fma,%function\n__math_fma:\n fmadd d0, d0, d1, d2\n ret\n"
    ".globl floor\n.type floor,%function\nfloor:\n frintm d0, d0\n ret\n"
    ".globl ceil\n.type ceil,%function\nceil:\n frintp d0, d0\n ret\n");
/* END generated stubs */
#endif

double frexp(double x, int *exp)
{
    u64 b = d2u(x);
    int e = (int)((b >> 52) & 0x7ff);
    *exp = 0;
    if (e == 0x7ff || (b << 1) == 0)
        return x + x;
    if (e == 0) {
        b = d2u(x * 0x1p64);
        e = (int)((b >> 52) & 0x7ff) - 64;
    }
    *exp = e - 1022;
    return u2d((b & ~INF64) | (1022UL << 52));
}

/* x * 2^n, rounded once */
double __math_scalbn(double x, int n)
{
    if (n > 1023) {
        x *= 0x1p1023;
        n -= 1023;
        if (n > 1023) {
            x *= 0x1p1023;
            n -= 1023;
            if (n > 1023)
                n = 1023;
        }
    } else if (n < -1022) {
        /* keep the intermediate results normal, or so small that the outcome is settled */
        x *= 0x1p-969;
        n += 969;
        if (n < -1022) {
            x *= 0x1p-969;
            n += 969;
            if (n < -1022)
                n = -1022;
        }
    }
    return x * u2d((u64)(n + 1023) << 52);
}
double ldexp(double x, int n) { return __math_scalbn(x, n); }

double modf(double x, double *ip)
{
    double i = x < 0 ? ceil(x) : floor(x);
    *ip = i;
    if (x - x != 0) /* infinity or NaN */
        return x != x ? x : u2d(d2u(x) & SIGN64);
    return u2d((d2u(x - i) & ~SIGN64) | (d2u(x) & SIGN64));
}

/* ---- fmod: long division on the significands, 11 quotient bits at a time ---- */

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

double fmod(double x, double y)
{
    u64 ux = d2u(x), uy = d2u(y), mx, my;
    int ex, ey, n;
    if ((uy << 1) == 0 || (ux << 1) >= (INF64 << 1) || (uy << 1) > (INF64 << 1))
        return (x * y) / (x * y);
    if ((ux << 1) <= (uy << 1))
        return (ux << 1) == (uy << 1) ? 0.0 * x : x;
    ex = decompose(ux, &mx);
    ey = decompose(uy, &my);
    mx %= my;
    for (n = ex - ey; n > 0 && mx; n -= 11) {
        int s = n < 11 ? n : 11;
        mx = (mx << s) % my;
    }
    x = __math_scalbn((double)mx, ey - 1075);
    return (ux >> 63) ? -x : x;
}
