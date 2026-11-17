/* What fp_linux.s and fp_darwin.s are on arm64, for x86-64 Linux, in C over the C library: the fused
   multiply-add, square root, rounding to an integral value and the rounding mode. The control word is the
   one the C library shows (its __math_getfpcr: the rounding mode where arm64's FPCR has it, whatever the
   machine's own register looks like), which is what fpconv.c writes into it. The sums of mma are made by
   the plain C of ptxops.c here (its mma_scalar says so), so the matrix routines are never entered. */
#if defined(__x86_64__)
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "ptx.h"

u64 __math_getfpcr(void);
void __math_setfpcr(u64 v);

float mc_fmaf(float a, float b, float c);

float mc_fmaf(float a, float b, float c) { return fmaf(a, b, c); }
double mc_fma(double a, double b, double c) { return fma(a, b, c); }
float mc_sqrtf(float x) { return sqrtf(x); }
double mc_sqrt(double x) { return sqrt(x); }
u64 mc_getfpcr(void) { return __math_getfpcr(); }
void mc_setfpcr(u64 v) { __math_setfpcr(v); }

/* to the nearest integral value, ties to even, whatever the rounding mode of the moment */
double mc_rintn(double x)
{
    u64 old = __math_getfpcr();
    if (old & 3UL << 22) {
        __math_setfpcr(old & ~(3UL << 22));
        x = rint(x);
        __math_setfpcr(old);
        return x;
    }
    return rint(x);
}
double mc_rintp(double x) { return ceil(x); }
double mc_rintm(double x) { return floor(x); }
double mc_rintz(double x) { return trunc(x); }
double mc_rinta(double x) { return round(x); }
/* (every float is a double, and so is its integral value) */
float mc_rintnf(float x) { return (float)mc_rintn((double)x); }
float mc_rintpf(float x) { return (float)ceil((double)x); }
float mc_rintmf(float x) { return (float)floor((double)x); }
float mc_rintzf(float x) { return (float)trunc((double)x); }

u64 mc_ticks(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (u64)ts.tv_sec * 1000000000UL + (u64)ts.tv_nsec;
}
u64 mc_tickhz(void) { return 1000000000UL; }

static void not_here(const char *what)
{
    fprintf(stderr, "mcuda: %s is not made for this machine\n", what);
    abort();
}

void mc_mma_rows(const double *A, const double *B, double *S, long K)
{
    (void)A, (void)B, (void)S, (void)K;
    not_here("mc_mma_rows");
}
int mc_mma_f16(const void *A, const void *B, void *C, double *W)
{
    (void)A, (void)B, (void)C, (void)W;
    not_here("mc_mma_f16");
    return 1;
}
int mc_mma_f16hw(const void *A, const void *B, void *C, double *W)
{
    (void)A, (void)B, (void)C, (void)W;
    not_here("mc_mma_f16hw");
    return 1;
}
void mc_mma_s8(const void *A, const void *B, void *C, void *W, long flags)
{
    (void)A, (void)B, (void)C, (void)W, (void)flags;
    not_here("mc_mma_s8");
}
void mc_mma_get16(Slot **R, const s32 *xs, void *A, void *B, void *C)
{
    (void)R, (void)xs, (void)A, (void)B, (void)C;
    not_here("mc_mma_get16");
}
void mc_mma_get8(Slot **R, const s32 *xs, void *A, void *B, void *C)
{
    (void)R, (void)xs, (void)A, (void)B, (void)C;
    not_here("mc_mma_get8");
}
void mc_mma_put(Slot **R, const s32 *xd, const void *C)
{
    (void)R, (void)xd, (void)C;
    not_here("mc_mma_put");
}
#else
extern int mc_fp_c_nothing_here;
#endif
