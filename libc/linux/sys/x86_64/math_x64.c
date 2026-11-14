/* x86-64: what the math library has as single instructions on aarch64, for the functions of C89
   (so this object defines reserved names and C89 names only; see libc/src/mathimpl.h). Plain SSE2 has
   neither a fused multiply-add nor rounding to an integer: fma uses the instruction when the processor
   and the system have it (FMA3) and integer arithmetic otherwise. */
#include "../../../src/mathimpl.h"

int __math_hasfma; /* 0 not known yet, 1 yes, 2 no */
double __math_fma_soft(double x, double y, double z);

__asm__(".text\n.balign 16\n"
    ".globl __math_fma\n.type __math_fma,%function\n__math_fma:\n"
    " movl __math_hasfma(%rip), %eax\n cmpl $1, %eax\n jne 1f\n"
    " .byte 0xc4, 0xe2, 0xf1, 0xa9, 0xc2\n ret\n" /* vfmadd213sd %xmm2, %xmm1, %xmm0 */
    "1: testl %eax, %eax\n jnz __math_fma_soft\n"
    " pushq %rbx\n movl $1, %eax\n cpuid\n movl $2, %esi\n"
    " andl $0x18001000, %ecx\n cmpl $0x18001000, %ecx\n jne 2f\n" /* FMA, OSXSAVE, AVX */
    " xorl %ecx, %ecx\n xgetbv\n andl $6, %eax\n cmpl $6, %eax\n jne 2f\n movl $1, %esi\n"
    "2: movl %esi, __math_hasfma(%rip)\n popq %rbx\n jmp __math_fma\n");

typedef struct { u64 m; int e, sign; } Num;

static Num split(double x)
{
    Num n;
    u64 b = d2u(x);
    int e = (int)(b >> 52);
    n.sign = e & 0x800;
    e &= 0x7ff;
    if (!e) {
        b = d2u(x * 0x1p63);
        e = (int)(b >> 52) & 0x7ff;
        e = e ? e - 63 : 0x800;
    }
    b &= ((u64)1 << 52) - 1;
    b |= (u64)1 << 52;
    n.m = b << 1;
    n.e = e - (0x3ff + 52 + 1);
    return n;
}

static void mul64(u64 *hi, u64 *lo, u64 x, u64 y)
{
    u64 xl = (u32)x, xh = x >> 32, yl = (u32)y, yh = y >> 32;
    u64 t1 = xl * yl, t2 = xl * yh + xh * yl, t3 = xh * yh;
    *lo = t1 + (t2 << 32);
    *hi = t3 + (t2 >> 32) + (t1 > *lo);
}

#define SPECIAL (0x7ff - 0x3ff - 52 - 1)

double __math_fma_soft(double x, double y, double z)
{
    Num nx = split(x), ny = split(y), nz = split(z);
    u64 rhi, rlo, zhi, zlo, t;
    int e, d, sign, nonzero;
    i64 i;
    double r;

    if (nx.e >= SPECIAL || ny.e >= SPECIAL)
        return x * y + z;
    if (nz.e >= SPECIAL) {
        if (nz.e > SPECIAL) /* z is 0 */
            return x * y + z;
        return z;
    }
    /* the product, 106 to 108 bits, then z lined up with it; a lost low part leaves a sticky bit */
    mul64(&rhi, &rlo, nx.m, ny.m);
    e = nx.e + ny.e;
    d = nz.e - e;
    if (d > 0) {
        if (d < 64) {
            zlo = nz.m << d;
            zhi = nz.m >> (64 - d);
        } else {
            zlo = 0;
            zhi = nz.m;
            e = nz.e - 64;
            d -= 64;
            if (d == 0) {
                ;
            } else if (d < 64) {
                rlo = rhi << (64 - d) | rlo >> d | ((rlo << (64 - d)) != 0);
                rhi = rhi >> d;
            } else {
                rlo = 1;
                rhi = 0;
            }
        }
    } else {
        zhi = 0;
        d = -d;
        if (d == 0)
            zlo = nz.m;
        else if (d < 64)
            zlo = nz.m >> d | ((nz.m << (64 - d)) != 0);
        else
            zlo = 1;
    }
    sign = nx.sign ^ ny.sign;
    nonzero = 1;
    if (sign == nz.sign) {
        rlo += zlo;
        rhi += zhi + (rlo < zlo);
    } else {
        t = rlo;
        rlo -= zlo;
        rhi = rhi - zhi - (t < rlo);
        if (rhi >> 63) {
            rlo = -rlo;
            rhi = -rhi - (rlo != 0);
            sign = !sign;
        }
        nonzero = rhi != 0;
    }
    /* the top 63 bits of the result into rhi, its last bit sticky */
    if (nonzero) {
        e += 64;
        d = __builtin_clzl(rhi) - 1;
        rhi = rhi << d | rlo >> (64 - d) | ((rlo << d) != 0);
    } else if (rlo) {
        d = __builtin_clzl(rlo) - 1;
        if (d < 0)
            rhi = rlo >> 1 | (rlo & 1);
        else
            rhi = rlo << d;
    } else {
        return x * y + z; /* an exact zero: this gets its sign right */
    }
    e -= d;
    i = (i64)rhi;
    if (sign)
        i = -i;
    r = (double)i; /* rounds; |r| is in [2^62, 2^63] */
    if (e < -1022 - 62) {
        /* subnormal: the scaling must not round a second time */
        if (e == -1022 - 63) {
            double c = 0x1p63;
            if (sign)
                c = -c;
            if (r == c)
                return __math_scalbn(r, e); /* rounds up to the smallest normal number */
            if (rhi << 53) {
                /* one more bit goes when it is scaled: put a bit on top so that the conversion
                   rounds at the right place, then take it away again */
                i = (i64)(rhi >> 1 | (rhi & 1) | (u64)1 << 62);
                if (sign)
                    i = -i;
                r = (double)i;
                r = 2 * r - c;
            }
        } else {
            d = 10;
            i = (i64)((rhi >> d | ((rhi << (64 - d)) != 0)) << d);
            if (sign)
                i = -i;
            r = (double)i;
        }
    }
    return __math_scalbn(r, e);
}

#define BIG 0x1p52

double floor(double x)
{
    double t;
    if (!(fabs(x) < BIG))
        return x;
    t = (double)(long)x;
    if (t > x)
        t -= 1;
    return t == 0 ? x * 0.0 : t;
}

double ceil(double x)
{
    double t;
    if (!(fabs(x) < BIG))
        return x;
    t = (double)(long)x;
    if (t < x)
        t += 1;
    return t == 0 ? x * 0.0 : t;
}
