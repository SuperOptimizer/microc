/* Math library test.
     check eval           reads "name arguments" on stdin and prints each line with the results appended
     check FILE           reads such lines with outputs and compares them with what this library computes;
                          prints "libm: N functions ok", or the functions that are off by more than their allowance
     check -v FILE        the same with the largest difference per function
   A line is a name and hexadecimal bit patterns: the arguments, then the results.
   Built with the host compiler (-DHOSTGEN) "eval" makes the expected values from the host's library. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef HOSTGEN
#define exp10l __exp10
#define exp10 __exp10
#define exp10f __exp10f
#define sincos __sincos
#define sincosf __sincosf
static int finite(double x) { return isfinite(x); }
static int finitef(float x) { return isfinite(x); }
static double pow10(double x) { return __exp10(x); }
static double gamma(double x) { return lgamma(x); }
static double drem(double x, double y) { return remainder(x, y); }
double lgamma_r(double, int *);
float lgammaf_r(float, int *);
#endif

typedef unsigned long u64;
typedef unsigned int u32;
static double D(u64 u) { double x; memcpy(&x, &u, 8); return x; }
static u64 UD(double x) { u64 u; memcpy(&u, &x, 8); return u; }
static float F(u64 u) { float x; u32 v = (u32)u; memcpy(&x, &v, 4); return x; }
static u64 UF(float x) { u32 u; memcpy(&u, &x, 4); return u; }

/* the macros, as functions */
static long fpclass(int c) { return c == FP_NAN ? 0 : c == FP_INFINITE ? 1 : c == FP_ZERO ? 2 : c == FP_SUBNORMAL ? 3 : c == FP_NORMAL ? 4 : 5; }
static long m_fpclassify(double x) { return fpclass(fpclassify(x)); }
static long m_fpclassifyf(float x) { return fpclass(fpclassify(x)); }
static long m_isnan(double x) { return isnan(x) != 0; }
static long m_isnanf(float x) { return isnan(x) != 0; }
static long m_isinf(double x) { return isinf(x) != 0; }
static long m_isinff(float x) { return isinf(x) != 0; }
static long m_isfinite(double x) { return isfinite(x) != 0; }
static long m_isfinitef(float x) { return isfinite(x) != 0; }
static long m_isnormal(double x) { return isnormal(x) != 0; }
static long m_isnormalf(float x) { return isnormal(x) != 0; }
static long m_signbit(double x) { return signbit(x) != 0; }
static long m_signbitf(float x) { return signbit(x) != 0; }
static long m_finite(double x) { return finite(x) != 0; }
static long m_finitef(float x) { return finitef(x) != 0; }
static long m_ilogb(double x) { return ilogb(x); }
static long m_ilogbf(float x) { return ilogbf(x); }
static long m_isless(double x, double y) { return isless(x, y) + 2 * isgreater(x, y) + 4 * islessequal(x, y) + 8 * isgreaterequal(x, y) + 16 * islessgreater(x, y) + 32 * isunordered(x, y); }
static long m_islessf(float x, float y) { return isless(x, y) + 2 * isgreater(x, y) + 4 * islessequal(x, y) + 8 * isgreaterequal(x, y) + 16 * islessgreater(x, y) + 32 * isunordered(x, y); }
static long m_llrint(double x) { return llrint(x); }
static long m_llround(double x) { return llround(x); }
static long m_llrintf(float x) { return llrintf(x); }
static long m_llroundf(float x) { return llroundf(x); }
static double m_lgamma_sign(double x, int *s) { double r = lgamma(x); *s = signgam; return r; }

enum { D_D, D_DD, D_DDD, F_F, F_FF, F_FFF, L_D, L_F, L_DD, L_FF, D_DI, F_FI, D_DL, F_FL,
       D_D_PI, F_F_PI, D_D_PD, F_F_PF, D_DD_PI, F_FF_PI, SC_D, SC_F };

typedef struct {
    const char *name;
    int kind;
    void *fn;
    int tol; /* allowed difference from the host in units in the last place */
    long n;
    long bad;
    u64 maxd;
    u64 worst[3];
} Fn;

#define E(k, f, t) {#f, k, (void *)f, t, 0, 0, 0, {0, 0, 0}}
#define M(k, f) {#f, k, (void *)m_##f, 0, 0, 0, 0, {0, 0, 0}}
static Fn fns[] = {
#include "funcs.h"
    {0, 0, 0, 0, 0, 0, 0, {0, 0, 0}}
};

static void eval(Fn *f, u64 *in, u64 *out)
{
    int i = 0;
    double d = 0, d2 = 0;
    float s = 0, s2 = 0;
    out[0] = out[1] = 0;
    switch (f->kind) {
    case D_D: out[0] = UD(((double (*)(double))f->fn)(D(in[0]))); break;
    case D_DD: out[0] = UD(((double (*)(double, double))f->fn)(D(in[0]), D(in[1]))); break;
    case D_DDD: out[0] = UD(((double (*)(double, double, double))f->fn)(D(in[0]), D(in[1]), D(in[2]))); break;
    case F_F: out[0] = UF(((float (*)(float))f->fn)(F(in[0]))); break;
    case F_FF: out[0] = UF(((float (*)(float, float))f->fn)(F(in[0]), F(in[1]))); break;
    case F_FFF: out[0] = UF(((float (*)(float, float, float))f->fn)(F(in[0]), F(in[1]), F(in[2]))); break;
    case L_D: out[0] = (u64)((long (*)(double))f->fn)(D(in[0])); break;
    case L_F: out[0] = (u64)((long (*)(float))f->fn)(F(in[0])); break;
    case L_DD: out[0] = (u64)((long (*)(double, double))f->fn)(D(in[0]), D(in[1])); break;
    case L_FF: out[0] = (u64)((long (*)(float, float))f->fn)(F(in[0]), F(in[1])); break;
    case D_DI: out[0] = UD(((double (*)(double, int))f->fn)(D(in[0]), (int)in[1])); break;
    case F_FI: out[0] = UF(((float (*)(float, int))f->fn)(F(in[0]), (int)in[1])); break;
    case D_DL: out[0] = UD(((double (*)(double, long))f->fn)(D(in[0]), (long)in[1])); break;
    case F_FL: out[0] = UF(((float (*)(float, long))f->fn)(F(in[0]), (long)in[1])); break;
    case D_D_PI: /* the integer says nothing when the result is not finite */
        d = ((double (*)(double, int *))f->fn)(D(in[0]), &i);
        out[0] = UD(d);
        out[1] = d - d == 0 ? (u64)(long)i : 0;
        break;
    case F_F_PI:
        s = ((float (*)(float, int *))f->fn)(F(in[0]), &i);
        out[0] = UF(s);
        out[1] = s - s == 0 ? (u64)(long)i : 0;
        break;
    case D_D_PD: out[0] = UD(((double (*)(double, double *))f->fn)(D(in[0]), &d)); out[1] = UD(d); break;
    case F_F_PF: out[0] = UF(((float (*)(float, float *))f->fn)(F(in[0]), &s)); out[1] = UF(s); break;
    case D_DD_PI: out[0] = UD(((double (*)(double, double, int *))f->fn)(D(in[0]), D(in[1]), &i)); out[1] = (u64)(long)((i & 7) | (i < 0 && (i & 7) ? 8 : 0)); break;
    case F_FF_PI: out[0] = UF(((float (*)(float, float, int *))f->fn)(F(in[0]), F(in[1]), &i)); out[1] = (u64)(long)((i & 7) | (i < 0 && (i & 7) ? 8 : 0)); break;
    case SC_D: ((void (*)(double, double *, double *))f->fn)(D(in[0]), &d, &d2); out[0] = UD(d); out[1] = UD(d2); break;
    case SC_F: ((void (*)(float, float *, float *))f->fn)(F(in[0]), &s, &s2); out[0] = UF(s); out[1] = UF(s2); break;
    }
}

/* distance between two results in units in the last place; NaNs are all alike */
static u64 dist(u64 a, u64 b, int isfloat, int isint)
{
    long x, y;
    if (a == b)
        return 0;
    if (isint)
        return 1000000;
    if (isfloat) {
        if ((a & 0x7fffffff) > 0x7f800000 && (b & 0x7fffffff) > 0x7f800000)
            return 0;
        if ((a & 0x7fffffff) > 0x7f800000 || (b & 0x7fffffff) > 0x7f800000 || ((a ^ b) & 0x80000000))
            return 1000000;
        x = (long)(a & 0x7fffffff);
        y = (long)(b & 0x7fffffff);
    } else {
        if ((a << 1) > 0xffe0000000000000UL && (b << 1) > 0xffe0000000000000UL)
            return 0;
        if ((a << 1) > 0xffe0000000000000UL || (b << 1) > 0xffe0000000000000UL || ((a ^ b) >> 63))
            return 1000000;
        x = (long)(a & 0x7fffffffffffffffUL);
        y = (long)(b & 0x7fffffffffffffffUL);
    }
    return (u64)(x > y ? x - y : y - x);
}

static int out_float(int k, int which)
{
    switch (k) {
    case F_F: case F_FF: case F_FFF: case F_FI: case F_FL: case F_F_PI: case F_FF_PI: return which == 0;
    case F_F_PF: case SC_F: return 1;
    }
    return 0;
}
static int out_int(int k, int which)
{
    switch (k) {
    case L_D: case L_F: case L_DD: case L_FF: return 1;
    case D_D_PI: case F_F_PI: case D_DD_PI: case F_FF_PI: return which == 1;
    }
    return 0;
}

static int nin(int k)
{
    switch (k) {
    case D_DDD: case F_FFF: return 3;
    case D_DD: case F_FF: case L_DD: case L_FF: case D_DI: case F_FI: case D_DL: case F_FL: case D_DD_PI: case F_FF_PI: return 2;
    }
    return 1;
}
static int nout(int k)
{
    switch (k) {
    case D_D_PI: case F_F_PI: case D_D_PD: case F_F_PF: case D_DD_PI: case F_FF_PI: case SC_D: case SC_F: return 2;
    }
    return 1;
}

/* "name hex hex ...": finds the function, fills v[], returns the number of values or -1 */
static int parse(char *line, Fn **fp, u64 *v)
{
    char *p = line, *q;
    Fn *f = *fp;
    int n = 0;
    while (*p && *p != ' ' && *p != '\n')
        p++;
    if (p == line)
        return -1;
    if (*p)
        *p++ = 0;
    if (!f || !f->name || strcmp(f->name, line) != 0)
        for (f = fns; f->name && strcmp(f->name, line) != 0; f++)
            ;
    *fp = f;
    if (!f->name)
        return -1;
    while (n < 5) {
        u64 x = strtoul(p, &q, 16);
        if (q == p)
            break;
        v[n++] = x;
        p = q;
    }
    return n;
}

int main(int argc, char **argv)
{
    char line[256];
    u64 v[5], in[3], want[2], got[2], d0, d1;
    int i, n;
    Fn *f = 0;
    FILE *fp;
    int verbose = 0, nfn = 0, nbad = 0;
    long lines = 0;
    if (argc > 1 && strcmp(argv[1], "eval") == 0) {
        while (fgets(line, sizeof line, stdin)) {
            n = parse(line, &f, v);
            if (n < 0 || n < nin(f->kind))
                continue;
            in[0] = in[1] = in[2] = 0;
            printf("%s", f->name);
            for (i = 0; i < nin(f->kind); i++) {
                in[i] = v[i];
                printf(" %lx", in[i]);
            }
            eval(f, in, got);
            for (i = 0; i < nout(f->kind); i++)
                printf(" %lx", got[i]);
            printf("\n");
        }
        return 0;
    }
    if (argc > 2 && strcmp(argv[1], "-v") == 0) {
        verbose = 1;
        argv++;
        argc--;
    }
    if (argc < 2 || !(fp = fopen(argv[1], "r"))) {
        fprintf(stderr, "usage: check [-v] FILE | check eval\n");
        return 2;
    }
    while (fgets(line, sizeof line, fp)) {
        n = parse(line, &f, v);
        if (n < 0)
            continue;
        if (n != nin(f->kind) + nout(f->kind)) {
            fprintf(stderr, "bad line for %s\n", f->name);
            return 1;
        }
        in[0] = in[1] = in[2] = want[0] = want[1] = 0;
        for (i = 0; i < nin(f->kind); i++)
            in[i] = v[i];
        for (i = 0; i < nout(f->kind); i++)
            want[i] = v[nin(f->kind) + i];
        eval(f, in, got);
        d0 = dist(got[0], want[0], out_float(f->kind, 0), out_int(f->kind, 0));
        d1 = dist(got[1], want[1], out_float(f->kind, 1), out_int(f->kind, 1));
        if (d1 > d0)
            d0 = d1;
        f->n++;
        lines++;
        if (d0 > (u64)f->tol) {
            if (f->bad++ < 3 && verbose)
                printf("  %s %lx %lx %lx: got %lx %lx, expected %lx %lx\n", f->name, in[0], in[1], in[2], got[0], got[1], want[0], want[1]);
        }
        if (d0 > f->maxd) {
            f->maxd = d0;
            memcpy(f->worst, in, sizeof in);
        }
    }
    for (f = fns; f->name; f++) {
        if (!f->n)
            continue;
        nfn++;
        if (verbose)
            printf("%-12s %6ld cases, max difference %lu ulp (at %lx %lx %lx)%s\n", f->name, f->n, f->maxd, f->worst[0], f->worst[1], f->worst[2], f->bad ? "  FAIL" : "");
        if (f->bad) {
            nbad++;
            printf("libm: %s: %ld of %ld results differ by more than %d ulp (largest difference %lu)\n", f->name, f->bad, f->n, f->tol, f->maxd);
        }
    }
    if (nbad || !nfn)
        return 1;
    printf("libm: %d functions ok\n", nfn);
    return 0;
}
