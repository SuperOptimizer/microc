/* Floating-point instructions that must give the same results on the processor and in mvm's interpreter:
   fused multiply-add (one rounding) and the minimum and maximum that prefer a number to a NaN. */
#include <stdio.h>
#include <string.h>
__asm__(".text\n"
        ".globl t_fmadd\nt_fmadd: fmadd d0, d0, d1, d2\n ret\n"
        ".globl t_fmsub\nt_fmsub: fmsub d0, d0, d1, d2\n ret\n"
        ".globl t_fnmadd\nt_fnmadd: fnmadd d0, d0, d1, d2\n ret\n"
        ".globl t_fnmsub\nt_fnmsub: fnmsub d0, d0, d1, d2\n ret\n"
        ".globl t_fmaddf\nt_fmaddf: fmadd s0, s0, s1, s2\n ret\n"
        ".globl t_fminnm\nt_fminnm: fminnm d0, d0, d1\n ret\n"
        ".globl t_fmaxnm\nt_fmaxnm: fmaxnm d0, d0, d1\n ret\n"
        ".globl t_fmin\nt_fmin: fmin d0, d0, d1\n ret\n"
        ".globl t_fmax\nt_fmax: fmax d0, d0, d1\n ret\n");
double t_fmadd(double, double, double), t_fmsub(double, double, double), t_fnmadd(double, double, double),
    t_fnmsub(double, double, double), t_fminnm(double, double), t_fmaxnm(double, double), t_fmin(double, double),
    t_fmax(double, double);
float t_fmaddf(float, float, float);
static unsigned long long bits(double d) { unsigned long long u; memcpy(&u, &d, 8); return u; }
static unsigned fbits(float f) { unsigned u; memcpy(&u, &f, 4); return u; }
int main(void)
{
    volatile double a = 1.0 + 0x1p-30, b = 1.0 - 0x1p-30, x = 0.1, y = 10.0, zero = 0.0;
    volatile float fa = 1.0f + 0x1p-12f, fb = 1.0f - 0x1p-12f, fx = 0.01f, fy = 0.16f;
    double n = zero / zero;
    printf("%llx %llx %llx %llx\n", bits(t_fmadd(a, b, -1.0)), bits(t_fmadd(x, y, -1.0)), bits(t_fmsub(a, b, 1.0)),
           bits(t_fmadd(1e200, 1e200, -1e300)));
    printf("%llx %llx %llx\n", bits(t_fnmadd(a, b, -1.0)), bits(t_fnmsub(x, y, 1.0)), bits(t_fmadd(x, x, x)));
    printf("%x %x %x\n", fbits(t_fmaddf(fa, fb, -1.0f)), fbits(t_fmaddf(fx, fy, 0.0f)), fbits(t_fmaddf(0.1f, 10.0f, -1.0f)));
    printf("%g %g %g %g\n", t_fminnm(n, 1.0), t_fmaxnm(1.0, n), t_fminnm(zero, -zero), t_fmaxnm(-zero, zero));
    printf("%d %d %g %g\n", t_fmin(n, 1.0) != t_fmin(n, 1.0), t_fmax(1.0, n) != t_fmax(1.0, n), t_fmin(zero, -zero), t_fmax(-zero, zero));
    return 0;
}
