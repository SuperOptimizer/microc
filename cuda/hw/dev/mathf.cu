/* The device library's single-precision functions as device code: see mathf.h. Each public function is
   written out once, with everything it uses inlined, under the name mcdw_NAME; mklib takes the PTX of this
   unit apart and makes of each the piece __mcuda_NAME, which is what compiled device code calls. */
#include <math.h>
#define MCD __device__ static __forceinline__
/* (the bits of a float in a register, and back: one instruction, where the header's functions go through memory) */
__device__ static __forceinline__ unsigned mcd_f2u(float a) { unsigned r; asm("mov.b32 %0, %1;" : "=r"(r) : "f"(a)); return r; }
__device__ static __forceinline__ float mcd_u2f(unsigned a) { float r; asm("mov.b32 %0, %1;" : "=f"(r) : "r"(a)); return r; }
#define F2U(x) mcd_f2u(x)
#define U2F(x) mcd_u2f(x)
__device__ static __forceinline__ float mcd_ex2(float a) { float r; asm("ex2.approx.ftz.f32 %0, %1;" : "=f"(r) : "f"(a)); return r; }
__device__ static __forceinline__ float mcd_ex2n(float a) { float r; asm("ex2.approx.f32 %0, %1;" : "=f"(r) : "f"(a)); return r; }
__device__ static __forceinline__ float mcd_rcp(float a) { float r; asm("rcp.approx.ftz.f32 %0, %1;" : "=f"(r) : "f"(a)); return r; }
#define RCP(x) mcd_rcp(x)
#define EX2(x) mcd_ex2(x)
#define EX2N(x) mcd_ex2n(x)
#define FMIN(a, b) fminf(a, b)
#define FMAX(a, b) fmaxf(a, b)
#define FMA(a, b, c) fmaf(a, b, c)
#define RINT(x) rintf(x)
#define SQRT(x) sqrtf(x)
#define TRUNC(x) truncf(x)
#define FABS(x) fabsf(x)
#include "mathf.h"

#define OUT __device__ __attribute__((noinline))
/* float to float; two floats to float */
#define F1(X) X(nearbyintf) X(roundf) X(rsqrtf) X(logbf) X(expf) X(exp2f) X(exp10f) X(expm1f) X(logf) X(log2f) X(log10f) X(log1pf) \
    X(sinf) X(cosf) X(tanf) X(sinpif) X(cospif) X(asinf) X(acosf) X(atanf) X(sinhf) X(coshf) X(tanhf) X(asinhf) X(acoshf) \
    X(atanhf) X(erff) X(erfcf) X(cbrtf)
#define F2(X) X(powf) X(atan2f) X(hypotf) X(fmodf) X(remainderf) X(fdimf)
#define X(n) OUT float mcdw_##n(float x) { return mcd_##n(x); }
F1(X)
#undef X
#define X(n) OUT float mcdw_##n(float x, float y) { return mcd_##n(x, y); }
F2(X)
#undef X
OUT float mcdw_scalbnf(float x, int n) { return mcd_scalbnf(x, n); }
OUT float mcdw_ldexpf(float x, int n) { return mcd_ldexpf(x, n); }
OUT float mcdw_frexpf(float x, int *e) { return mcd_frexpf(x, e); }
OUT float mcdw_modff(float x, float *ip) { return mcd_modff(x, ip); }
OUT int mcdw_ilogbf(float x) { return mcd_ilogbf(x); }
OUT long mcdw_lroundf(float x) { return mcd_lroundf(x); }
OUT long long mcdw_llroundf(float x) { return mcd_llroundf(x); }
OUT long mcdw_lrintf(float x) { return mcd_lrintf(x); }
OUT long long mcdw_llrintf(float x) { return mcd_llrintf(x); }
OUT float mcdw_nanf(const char *s) { return mcd_nanf(s); }
OUT void mcdw_sincosf(float x, float *s, float *c) { mcd_sincosf(x, s, c); }
OUT void mcdw_sincospif(float x, float *s, float *c) { mcd_sincospif(x, s, c); }

/* Every function is called once here, so that the compiler writes it out; and a launch, so that mklib is
   handed this unit's module. Neither is ever run. */
__global__ void mcd_all_mathf(float *out)
{
    float f = out[0], g = out[1];
    int i = (int)out[2];
#define X(n) f += mcdw_##n(f);
    F1(X)
#undef X
#define X(n) f += mcdw_##n(f, g);
    F2(X)
#undef X
    f += mcdw_scalbnf(f, i) + mcdw_ldexpf(f, i) + mcdw_frexpf(f, &i) + mcdw_modff(f, &g) + (float)mcdw_ilogbf(f);
    f += (float)(mcdw_lroundf(f) + mcdw_llroundf(f) + mcdw_lrintf(f) + mcdw_llrintf(f)) + mcdw_nanf((const char *)0);
    mcdw_sincosf(f, &f, &g);
    mcdw_sincospif(f, &f, &g);
    out[0] = f + g + (float)i;
}
extern "C" void mcd_unit_mathf(void);
void mcd_unit_mathf(void) { mcd_all_mathf<<<1, 1>>>((float *)0); }
