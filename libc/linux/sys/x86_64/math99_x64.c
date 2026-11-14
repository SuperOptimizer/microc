/* x86-64: what the math library has as single instructions on aarch64, for the functions that C99
   added, and the floating-point environment (MXCSR, shown to math99.c as the two registers of
   aarch64, whose bits <fenv.h> names). See math_x64.c. */
#include "../../../src/mathimpl.h"

#define BIG 0x1p52
#define BIGF 0x1p23f

__asm__(".text\n.balign 16\n"
    ".globl sqrtf\n.type sqrtf,%function\nsqrtf:\n sqrtss %xmm0, %xmm0\n ret\n"
    ".globl lrint\n.type lrint,%function\n.globl llrint\n.type llrint,%function\n"
    ".globl lrintl\n.type lrintl,%function\n.globl llrintl\n.type llrintl,%function\n"
    "lrint:\nllrint:\nlrintl:\nllrintl:\n cvtsd2si %xmm0, %rax\n ret\n"
    ".globl lrintf\n.type lrintf,%function\n.globl llrintf\n.type llrintf,%function\n"
    "lrintf:\nllrintf:\n cvtss2si %xmm0, %rax\n ret\n"
    ".type __math_getmxcsr,%function\n__math_getmxcsr:\n stmxcsr -4(%rsp)\n movl -4(%rsp), %eax\n ret\n"
    ".type __math_setmxcsr,%function\n__math_setmxcsr:\n movl %edi, -4(%rsp)\n ldmxcsr -4(%rsp)\n ret\n");

unsigned __math_getmxcsr(void);
void __math_setmxcsr(unsigned);

/* rounding: nearest 0, down 1, up 2, zero 3 in MXCSR; nearest 0, up 1, down 2, zero 3 in FPCR */
static u64 swap_rm(u64 r)
{
    return r == 1 || r == 2 ? 3 - r : r;
}

u64 __math_getfpcr(void)
{
    return swap_rm(__math_getmxcsr() >> 13 & 3) << 22;
}

void __math_setfpcr(u64 v)
{
    __math_setmxcsr((__math_getmxcsr() & ~0x6000U) | (unsigned)(swap_rm(v >> 22 & 3) << 13));
}

/* flags: invalid 1, denormal 2, divide 4, overflow 8, underflow 16, inexact 32 in MXCSR;
   invalid 1, divide 2, overflow 4, underflow 8, inexact 16, denormal 128 in FPSR */
u64 __math_getfpsr(void)
{
    unsigned m = __math_getmxcsr();
    return (m & 1) | (m >> 1 & 0x1e) | (m & 2 ? 0x80 : 0);
}

void __math_setfpsr(u64 v)
{
    __math_setmxcsr((__math_getmxcsr() & ~0x3fU) | (unsigned)((v & 1) | (v & 0x1e) << 1 | (v & 0x80 ? 2 : 0)));
}

double fma(double x, double y, double z) { return __math_fma(x, y, z); }
double fmal(double x, double y, double z) { return __math_fma(x, y, z); }

/* The product is exact in double; the sum is rounded twice, to double and then to float, which is wrong
   only when the double lands exactly between two floats without being the exact sum. */
float fmaf(float x, float y, float z)
{
    double p = (double)x * y, s, e;
    u64 b;
    TWOSUM(s, e, p, (double)z);
    b = d2u(s);
    if (e != 0 && (b & 0x1fffffff) == 0x10000000 && (b >> 52 & 0x7ff) != 0x7ff)
        s = u2d((e < 0) == (s < 0) ? b + 1 : b - 1);
    return (float)s;
}

float fabsf(float x) { return u2f(f2u(x) & 0x7fffffff); }
double floorl(double x) { return floor(x); }
float floorf(float x) { return (float)floor(x); }
double ceill(double x) { return ceil(x); }
float ceilf(float x) { return (float)ceil(x); }

double trunc(double x)
{
    double t;
    if (!(fabs(x) < BIG))
        return x;
    t = (double)(long)x;
    return t == 0 ? x * 0.0 : t;
}
double truncl(double x) { return trunc(x); }
float truncf(float x) { return (float)trunc(x); }

double round(double x)
{
    double t, d;
    if (!(fabs(x) < BIG))
        return x;
    t = (double)(long)x;
    d = x - t;
    if (d >= 0.5)
        t += 1;
    else if (d <= -0.5)
        t -= 1;
    return t == 0 ? x * 0.0 : t;
}
double roundl(double x) { return round(x); }
float roundf(float x) { return (float)round(x); }

/* in the rounding mode of the moment */
double rint(double x)
{
    volatile double t;
    if (!(fabs(x) < BIG))
        return x;
    if (d2u(x) >> 63) {
        t = x - BIG;
        t = t + BIG;
    } else {
        t = x + BIG;
        t = t - BIG;
    }
    return t == 0 ? x * 0.0 : t;
}
double rintl(double x) { return rint(x); }
float rintf(float x) { return (float)rint(x); }

double nearbyint(double x)
{
    unsigned m = __math_getmxcsr();
    x = rint(x);
    __math_setmxcsr((__math_getmxcsr() & ~0x20U) | (m & 0x20));
    return x;
}
double nearbyintl(double x) { return nearbyint(x); }
float nearbyintf(float x) { return (float)nearbyint(x); }

long lround(double x) { return (long)round(x); }
long llround(double x) { return (long)round(x); }
long lroundl(double x) { return (long)round(x); }
long llroundl(double x) { return (long)round(x); }
long lroundf(float x) { return (long)round(x); }
long llroundf(float x) { return (long)round(x); }

/* a NaN loses against a number; -0 is less than +0 */
double fmin(double x, double y)
{
    if (x != x)
        return y;
    if (y != y)
        return x;
    if (x == y)
        return d2u(x) >> 63 ? x : y;
    return x < y ? x : y;
}
double fmax(double x, double y)
{
    if (x != x)
        return y;
    if (y != y)
        return x;
    if (x == y)
        return d2u(x) >> 63 ? y : x;
    return x > y ? x : y;
}
double fminl(double x, double y) { return fmin(x, y); }
double fmaxl(double x, double y) { return fmax(x, y); }
float fminf(float x, float y) { return (float)fmin(x, y); }
float fmaxf(float x, float y) { return (float)fmax(x, y); }
