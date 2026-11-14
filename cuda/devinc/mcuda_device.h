/* mcc CUDA: what nvcc makes known to every .cu file without an #include. */
#ifndef MCUDA_DEVICE_H
#define MCUDA_DEVICE_H

/* nvcc's own headers bring these in */
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

struct uint3 { unsigned x, y, z; };
struct dim3 {
    unsigned x, y, z;
    dim3(unsigned vx = 1, unsigned vy = 1, unsigned vz = 1) : x(vx), y(vy), z(vz) {}
};
extern const dim3 threadIdx, blockIdx, blockDim, gridDim;
static const int warpSize = 32;

struct float2 { float x, y; };
struct float3 { float x, y, z; };
struct float4 { float x, y, z, w; };
struct int2 { int x, y; };
struct int3 { int x, y, z; };
struct int4 { int x, y, z, w; };
struct uint2 { unsigned x, y; };
struct uint4 { unsigned x, y, z, w; };
__host__ __device__ static __forceinline__ float2 make_float2(float x, float y) { float2 r = {x, y}; return r; }
__host__ __device__ static __forceinline__ float3 make_float3(float x, float y, float z) { float3 r = {x, y, z}; return r; }
__host__ __device__ static __forceinline__ float4 make_float4(float x, float y, float z, float w) { float4 r = {x, y, z, w}; return r; }
__host__ __device__ static __forceinline__ int2 make_int2(int x, int y) { int2 r = {x, y}; return r; }
__host__ __device__ static __forceinline__ int3 make_int3(int x, int y, int z) { int3 r = {x, y, z}; return r; }
__host__ __device__ static __forceinline__ int4 make_int4(int x, int y, int z, int w) { int4 r = {x, y, z, w}; return r; }
__host__ __device__ static __forceinline__ uint2 make_uint2(unsigned x, unsigned y) { uint2 r = {x, y}; return r; }
__host__ __device__ static __forceinline__ uint4 make_uint4(unsigned x, unsigned y, unsigned z, unsigned w) { uint4 r = {x, y, z, w}; return r; }

/* barriers */
__device__ static __forceinline__ void __syncthreads(void) { asm volatile("bar.sync 0;"); }
__device__ static __forceinline__ void __syncwarp(unsigned mask = 0xffffffffu) { asm volatile("bar.warp.sync %0;" : : "r"(mask)); }
__device__ static __forceinline__ int __syncthreads_count(int p) { int r; asm volatile("{ .reg .pred q; setp.ne.b32 q, %1, 0; bar.red.popc.u32 %0, 0, q; }" : "=r"(r) : "r"(p)); return r; }
__device__ static __forceinline__ int __syncthreads_or(int p) { int r; asm volatile("{ .reg .pred q; setp.ne.b32 q, %1, 0; bar.red.or.pred q, 0, q; selp.b32 %0, 1, 0, q; }" : "=r"(r) : "r"(p)); return r; }
__device__ static __forceinline__ int __syncthreads_and(int p) { int r; asm volatile("{ .reg .pred q; setp.ne.b32 q, %1, 0; bar.red.and.pred q, 0, q; selp.b32 %0, 1, 0, q; }" : "=r"(r) : "r"(p)); return r; }
__device__ static __forceinline__ void __trap(void) { asm volatile("trap;"); }

/* bits */
__host__ __device__ static __forceinline__ unsigned __float_as_uint(float f) { union { float f; unsigned u; } v; v.f = f; return v.u; }
__host__ __device__ static __forceinline__ int __float_as_int(float f) { union { float f; int u; } v; v.f = f; return v.u; }
__host__ __device__ static __forceinline__ float __uint_as_float(unsigned u) { union { float f; unsigned u; } v; v.u = u; return v.f; }
__host__ __device__ static __forceinline__ float __int_as_float(int u) { union { float f; int u; } v; v.u = u; return v.f; }
__host__ __device__ static __forceinline__ long long __double_as_longlong(double f) { union { double f; long long u; } v; v.f = f; return v.u; }
__host__ __device__ static __forceinline__ double __longlong_as_double(long long u) { union { double f; long long u; } v; v.u = u; return v.f; }
__device__ static __forceinline__ int __popc(unsigned x) { int r; asm("popc.b32 %0, %1;" : "=r"(r) : "r"(x)); return r; }
__device__ static __forceinline__ int __popcll(unsigned long long x) { int r; asm("popc.b64 %0, %1;" : "=r"(r) : "l"(x)); return r; }
__device__ static __forceinline__ int __clz(int x) { int r; asm("clz.b32 %0, %1;" : "=r"(r) : "r"(x)); return r; }
__device__ static __forceinline__ int __clzll(long long x) { int r; asm("clz.b64 %0, %1;" : "=r"(r) : "l"(x)); return r; }
__device__ static __forceinline__ int __ffs(int x) { int r; asm("{ .reg .b32 t; brev.b32 t, %1; bfind.shiftamt.u32 t, t; add.s32 %0, t, 1; }" : "=r"(r) : "r"(x)); return r; }
__host__ __device__ static __forceinline__ int __mulhi(int a, int b) { return (int)(((long long)a * b) >> 32); }
__host__ __device__ static __forceinline__ unsigned __umulhi(unsigned a, unsigned b) { return (unsigned)(((unsigned long long)a * b) >> 32); }
__host__ __device__ static __forceinline__ long long __mul64hi(long long a, long long b) { return (long long)(((__int128)a * b) >> 64); }
__host__ __device__ static __forceinline__ unsigned long long __umul64hi(unsigned long long a, unsigned long long b) { return (unsigned long long)(((unsigned __int128)a * b) >> 64); }
__device__ static __forceinline__ unsigned __brev(unsigned x) { unsigned r; asm("brev.b32 %0, %1;" : "=r"(r) : "r"(x)); return r; }
__device__ static __forceinline__ unsigned __funnelshift_r(unsigned lo, unsigned hi, unsigned s) { unsigned r; asm("shf.r.wrap.b32 %0, %1, %2, %3;" : "=r"(r) : "r"(lo), "r"(hi), "r"(s)); return r; }
__device__ static __forceinline__ unsigned __funnelshift_l(unsigned lo, unsigned hi, unsigned s) { unsigned r; asm("shf.l.wrap.b32 %0, %1, %2, %3;" : "=r"(r) : "r"(lo), "r"(hi), "r"(s)); return r; }
__device__ static __forceinline__ unsigned __byte_perm(unsigned x, unsigned y, unsigned s) { unsigned r; asm("prmt.b32 %0, %1, %2, %3;" : "=r"(r) : "r"(x), "r"(y), "r"(s & 0x7777)); return r; }   /* the fourth bit of a selector would ask prmt for a sign */

/* arithmetic with a stated rounding, never contracted */
__device__ static __forceinline__ float __fmul_rn(float a, float b) { float r; asm("mul.rn.f32 %0, %1, %2;" : "=f"(r) : "f"(a), "f"(b)); return r; }
__device__ static __forceinline__ float __fadd_rn(float a, float b) { float r; asm("add.rn.f32 %0, %1, %2;" : "=f"(r) : "f"(a), "f"(b)); return r; }
__device__ static __forceinline__ float __fsub_rn(float a, float b) { float r; asm("sub.rn.f32 %0, %1, %2;" : "=f"(r) : "f"(a), "f"(b)); return r; }
__device__ static __forceinline__ float __fdiv_rn(float a, float b) { float r; asm("div.rn.f32 %0, %1, %2;" : "=f"(r) : "f"(a), "f"(b)); return r; }
__device__ static __forceinline__ float __fsqrt_rn(float a) { float r; asm("sqrt.rn.f32 %0, %1;" : "=f"(r) : "f"(a)); return r; }
__device__ static __forceinline__ float __fmaf_rn(float a, float b, float c) { float r; asm("fma.rn.f32 %0, %1, %2, %3;" : "=f"(r) : "f"(a), "f"(b), "f"(c)); return r; }
__device__ static __forceinline__ double __dmul_rn(double a, double b) { double r; asm("mul.rn.f64 %0, %1, %2;" : "=d"(r) : "d"(a), "d"(b)); return r; }
__device__ static __forceinline__ double __dadd_rn(double a, double b) { double r; asm("add.rn.f64 %0, %1, %2;" : "=d"(r) : "d"(a), "d"(b)); return r; }
__device__ static __forceinline__ double __dsub_rn(double a, double b) { double r; asm("sub.rn.f64 %0, %1, %2;" : "=d"(r) : "d"(a), "d"(b)); return r; }
__device__ static __forceinline__ double __ddiv_rn(double a, double b) { double r; asm("div.rn.f64 %0, %1, %2;" : "=d"(r) : "d"(a), "d"(b)); return r; }
__device__ static __forceinline__ double __fma_rn(double a, double b, double c) { double r; asm("fma.rn.f64 %0, %1, %2, %3;" : "=d"(r) : "d"(a), "d"(b), "d"(c)); return r; }
__device__ static __forceinline__ int __float2int_rn(float a) { int r; asm("cvt.rni.s32.f32 %0, %1;" : "=r"(r) : "f"(a)); return r; }
__device__ static __forceinline__ int __float2int_rz(float a) { int r; asm("cvt.rzi.s32.f32 %0, %1;" : "=r"(r) : "f"(a)); return r; }
__device__ static __forceinline__ int __float2int_rd(float a) { int r; asm("cvt.rmi.s32.f32 %0, %1;" : "=r"(r) : "f"(a)); return r; }
__device__ static __forceinline__ int __float2int_ru(float a) { int r; asm("cvt.rpi.s32.f32 %0, %1;" : "=r"(r) : "f"(a)); return r; }
__device__ static __forceinline__ unsigned __float2uint_rn(float a) { unsigned r; asm("cvt.rni.u32.f32 %0, %1;" : "=r"(r) : "f"(a)); return r; }
__device__ static __forceinline__ unsigned __float2uint_rz(float a) { unsigned r; asm("cvt.rzi.u32.f32 %0, %1;" : "=r"(r) : "f"(a)); return r; }
__device__ static __forceinline__ float __int2float_rn(int a) { float r; asm("cvt.rn.f32.s32 %0, %1;" : "=f"(r) : "r"(a)); return r; }
__device__ static __forceinline__ float __uint2float_rn(unsigned a) { float r; asm("cvt.rn.f32.u32 %0, %1;" : "=f"(r) : "r"(a)); return r; }
__device__ static __forceinline__ float __saturatef(float a) { float r; asm("cvt.sat.f32.f32 %0, %1;" : "=f"(r) : "f"(a)); return r; }
__device__ static __forceinline__ float rsqrtf(float a) { float r; asm("rsqrt.approx.f32 %0, %1;" : "=f"(r) : "f"(a)); return r; }
__device__ static __forceinline__ double rsqrt(double a) { double r; asm("rsqrt.approx.f64 %0, %1;" : "=d"(r) : "d"(a)); return r; }

extern "C" {
float expf(float); float logf(float); float sinf(float); float cosf(float); float tanhf(float); float powf(float, float);
float sqrtf(float); float fabsf(float); float floorf(float); float ceilf(float); float fminf(float, float); float fmaxf(float, float);
float truncf(float); float rintf(float); float roundf(float); float fmaf(float, float, float); float copysignf(float, float);
float log2f(float); float exp2f(float); float erff(float); float atan2f(float, float); float fmodf(float, float);
float log1pf(float); float expm1f(float); float tanf(float); float atanf(float); float acosf(float); float asinf(float);
float cbrtf(float); float log10f(float); float sinhf(float); float coshf(float); float nearbyintf(float);
double exp(double); double log(double); double sqrt(double); double fabs(double); double floor(double); double ceil(double);
double pow(double, double); double sin(double); double cos(double); double tanh(double); double fmin(double, double);
double fmax(double, double); double round(double); double rint(double); double trunc(double); double fma(double, double, double);
double log2(double); double exp2(double); double atan2(double, double); double fmod(double, double); double erf(double);
int abs(int); long labs(long); long long llabs(long long);
}
#define MCUDA_DEV(f) __device__ static __forceinline__ float __##f(float a) { return f(a); }
MCUDA_DEV(expf) MCUDA_DEV(logf) MCUDA_DEV(sinf) MCUDA_DEV(cosf) MCUDA_DEV(tanf) MCUDA_DEV(log2f) MCUDA_DEV(exp2f) MCUDA_DEV(log10f)
#undef MCUDA_DEV
__device__ static __forceinline__ float __powf(float a, float b) { return powf(a, b); }

/* the C++ overloads of the C library's names */
__host__ __device__ static __forceinline__ float sqrt(float a) { return sqrtf(a); }
__host__ __device__ static __forceinline__ float fabs(float a) { return fabsf(a); }
__host__ __device__ static __forceinline__ float abs(float a) { return fabsf(a); }
__host__ __device__ static __forceinline__ double abs(double a) { return fabs(a); }
__host__ __device__ static __forceinline__ float exp(float a) { return expf(a); }
__host__ __device__ static __forceinline__ float log(float a) { return logf(a); }
__host__ __device__ static __forceinline__ float floor(float a) { return floorf(a); }
__host__ __device__ static __forceinline__ float ceil(float a) { return ceilf(a); }
__host__ __device__ static __forceinline__ float pow(float a, float b) { return powf(a, b); }
__host__ __device__ static __forceinline__ float sin(float a) { return sinf(a); }
__host__ __device__ static __forceinline__ float cos(float a) { return cosf(a); }
__host__ __device__ static __forceinline__ float tanh(float a) { return tanhf(a); }
__host__ __device__ static __forceinline__ float round(float a) { return roundf(a); }
__host__ __device__ static __forceinline__ float fmin(float a, float b) { return fminf(a, b); }
__host__ __device__ static __forceinline__ float fmax(float a, float b) { return fmaxf(a, b); }
__host__ __device__ static __forceinline__ int min(int a, int b) { return a < b ? a : b; }
__host__ __device__ static __forceinline__ int max(int a, int b) { return a > b ? a : b; }
__host__ __device__ static __forceinline__ unsigned min(unsigned a, unsigned b) { return a < b ? a : b; }
__host__ __device__ static __forceinline__ unsigned max(unsigned a, unsigned b) { return a > b ? a : b; }
__host__ __device__ static __forceinline__ unsigned min(int a, unsigned b) { return (unsigned)a < b ? (unsigned)a : b; }
__host__ __device__ static __forceinline__ unsigned min(unsigned a, int b) { return a < (unsigned)b ? a : (unsigned)b; }
__host__ __device__ static __forceinline__ unsigned max(int a, unsigned b) { return (unsigned)a > b ? (unsigned)a : b; }
__host__ __device__ static __forceinline__ unsigned max(unsigned a, int b) { return a > (unsigned)b ? a : (unsigned)b; }
__host__ __device__ static __forceinline__ long min(long a, long b) { return a < b ? a : b; }
__host__ __device__ static __forceinline__ long max(long a, long b) { return a > b ? a : b; }
__host__ __device__ static __forceinline__ unsigned long min(unsigned long a, unsigned long b) { return a < b ? a : b; }
__host__ __device__ static __forceinline__ unsigned long max(unsigned long a, unsigned long b) { return a > b ? a : b; }
__host__ __device__ static __forceinline__ long long min(long long a, long long b) { return a < b ? a : b; }
__host__ __device__ static __forceinline__ long long max(long long a, long long b) { return a > b ? a : b; }
__host__ __device__ static __forceinline__ unsigned long long min(unsigned long long a, unsigned long long b) { return a < b ? a : b; }
__host__ __device__ static __forceinline__ unsigned long long max(unsigned long long a, unsigned long long b) { return a > b ? a : b; }
__host__ __device__ static __forceinline__ float min(float a, float b) { return fminf(a, b); }
__host__ __device__ static __forceinline__ float max(float a, float b) { return fmaxf(a, b); }
__host__ __device__ static __forceinline__ float min(float a, int b) { return fminf(a, (float)b); }
__host__ __device__ static __forceinline__ float max(float a, int b) { return fmaxf(a, (float)b); }
__host__ __device__ static __forceinline__ float min(int a, float b) { return fminf((float)a, b); }
__host__ __device__ static __forceinline__ float max(int a, float b) { return fmaxf((float)a, b); }
__host__ __device__ static __forceinline__ double min(double a, double b) { return fmin(a, b); }
__host__ __device__ static __forceinline__ double max(double a, double b) { return fmax(a, b); }
__host__ __device__ static __forceinline__ double min(double a, float b) { return fmin(a, (double)b); }
__host__ __device__ static __forceinline__ double max(double a, float b) { return fmax(a, (double)b); }
__host__ __device__ static __forceinline__ double min(float a, double b) { return fmin((double)a, b); }
__host__ __device__ static __forceinline__ double max(float a, double b) { return fmax((double)a, b); }

/* more of the same, as CUDA names them */
__device__ static __forceinline__ float __fadd_rz(float a, float b) { float r; asm("add.rz.f32 %0, %1, %2;" : "=f"(r) : "f"(a), "f"(b)); return r; }
__device__ static __forceinline__ float __fadd_ru(float a, float b) { float r; asm("add.rp.f32 %0, %1, %2;" : "=f"(r) : "f"(a), "f"(b)); return r; }
__device__ static __forceinline__ float __fadd_rd(float a, float b) { float r; asm("add.rm.f32 %0, %1, %2;" : "=f"(r) : "f"(a), "f"(b)); return r; }
__device__ static __forceinline__ float __fmul_rz(float a, float b) { float r; asm("mul.rz.f32 %0, %1, %2;" : "=f"(r) : "f"(a), "f"(b)); return r; }
__device__ static __forceinline__ float __fmul_ru(float a, float b) { float r; asm("mul.rp.f32 %0, %1, %2;" : "=f"(r) : "f"(a), "f"(b)); return r; }
__device__ static __forceinline__ float __fmul_rd(float a, float b) { float r; asm("mul.rm.f32 %0, %1, %2;" : "=f"(r) : "f"(a), "f"(b)); return r; }
__device__ static __forceinline__ double __dadd_rz(double a, double b) { double r; asm("add.rz.f64 %0, %1, %2;" : "=d"(r) : "d"(a), "d"(b)); return r; }
__device__ static __forceinline__ double __dmul_rz(double a, double b) { double r; asm("mul.rz.f64 %0, %1, %2;" : "=d"(r) : "d"(a), "d"(b)); return r; }
__device__ static __forceinline__ double __dsqrt_rn(double a) { double r; asm("sqrt.rn.f64 %0, %1;" : "=d"(r) : "d"(a)); return r; }
__device__ static __forceinline__ int __double2int_rn(double a) { int r; asm("cvt.rni.s32.f64 %0, %1;" : "=r"(r) : "d"(a)); return r; }
__device__ static __forceinline__ int __double2int_rz(double a) { int r; asm("cvt.rzi.s32.f64 %0, %1;" : "=r"(r) : "d"(a)); return r; }
__device__ static __forceinline__ unsigned __double2uint_rn(double a) { unsigned r; asm("cvt.rni.u32.f64 %0, %1;" : "=r"(r) : "d"(a)); return r; }
__device__ static __forceinline__ long long __double2ll_rn(double a) { long long r; asm("cvt.rni.s64.f64 %0, %1;" : "=l"(r) : "d"(a)); return r; }
__device__ static __forceinline__ long long __double2ll_rz(double a) { long long r; asm("cvt.rzi.s64.f64 %0, %1;" : "=l"(r) : "d"(a)); return r; }
__device__ static __forceinline__ long long __double2ll_rd(double a) { long long r; asm("cvt.rmi.s64.f64 %0, %1;" : "=l"(r) : "d"(a)); return r; }
__device__ static __forceinline__ long long __float2ll_rn(float a) { long long r; asm("cvt.rni.s64.f32 %0, %1;" : "=l"(r) : "f"(a)); return r; }
__device__ static __forceinline__ long long __float2ll_rz(float a) { long long r; asm("cvt.rzi.s64.f32 %0, %1;" : "=l"(r) : "f"(a)); return r; }
__device__ static __forceinline__ double __ll2double_rn(long long a) { double r; asm("cvt.rn.f64.s64 %0, %1;" : "=d"(r) : "l"(a)); return r; }
__device__ static __forceinline__ double __int2double_rn(int a) { double r; asm("cvt.rn.f64.s32 %0, %1;" : "=d"(r) : "r"(a)); return r; }
__device__ static __forceinline__ int __ffsll(long long x) { int r; asm("{ .reg .b64 t; .reg .b32 u; brev.b64 t, %1; bfind.shiftamt.u64 u, t; add.s32 %0, u, 1; }" : "=r"(r) : "l"(x)); return r; }
/* each 16-bit half of the result is all ones where the halves of a and b are equal */
__host__ __device__ static __forceinline__ unsigned __vcmpeq2(unsigned a, unsigned b) { unsigned x = a ^ b; return ((x & 0xffffu) ? 0u : 0xffffu) | ((x >> 16) ? 0u : 0xffff0000u); }

/* atomic operations on memory; each gives back what was there before */
/* (in host code, which nvcc would not let call them, they are the plain operation: with __CUDA_ARCH__ defined
   for the whole unit, a function for both sides that takes its device form can come to be called there) */
#define MCUDA_ATOM(name, op, T, cls, hv) \
    __host__ __device__ static __forceinline__ T name(T *p, T v) { \
        T r; \
        if (__builtin_mcuda_device()) asm volatile("atom." op " %0, [%1], %2;" : "=" cls(r) : "l"(p), cls(v)); \
        else { r = *p; *p = (hv); } \
        return r; }
MCUDA_ATOM(atomicAdd, "add.s32", int, "r", r + v) MCUDA_ATOM(atomicAdd, "add.u32", unsigned, "r", r + v) MCUDA_ATOM(atomicAdd, "add.u64", unsigned long long, "l", r + v)
MCUDA_ATOM(atomicAdd, "add.u64", unsigned long, "l", r + v) MCUDA_ATOM(atomicAdd, "add.s64", long long, "l", r + v)
MCUDA_ATOM(atomicAdd, "add.f32", float, "f", r + v) MCUDA_ATOM(atomicAdd, "add.f64", double, "d", r + v)
MCUDA_ATOM(atomicMin, "min.s32", int, "r", v < r ? v : r) MCUDA_ATOM(atomicMin, "min.u32", unsigned, "r", v < r ? v : r) MCUDA_ATOM(atomicMin, "min.u64", unsigned long long, "l", v < r ? v : r)
MCUDA_ATOM(atomicMin, "min.u64", unsigned long, "l", v < r ? v : r) MCUDA_ATOM(atomicMin, "min.s64", long long, "l", v < r ? v : r)
MCUDA_ATOM(atomicMax, "max.s32", int, "r", v > r ? v : r) MCUDA_ATOM(atomicMax, "max.u32", unsigned, "r", v > r ? v : r) MCUDA_ATOM(atomicMax, "max.u64", unsigned long long, "l", v > r ? v : r)
MCUDA_ATOM(atomicMax, "max.u64", unsigned long, "l", v > r ? v : r) MCUDA_ATOM(atomicMax, "max.s64", long long, "l", v > r ? v : r)
MCUDA_ATOM(atomicExch, "exch.b32", int, "r", v) MCUDA_ATOM(atomicExch, "exch.b32", unsigned, "r", v) MCUDA_ATOM(atomicExch, "exch.b64", unsigned long long, "l", v)
MCUDA_ATOM(atomicExch, "exch.b64", unsigned long, "l", v)
MCUDA_ATOM(atomicAnd, "and.b32", int, "r", r & v) MCUDA_ATOM(atomicAnd, "and.b32", unsigned, "r", r & v) MCUDA_ATOM(atomicAnd, "and.b64", unsigned long long, "l", r & v)
MCUDA_ATOM(atomicOr, "or.b32", int, "r", r | v) MCUDA_ATOM(atomicOr, "or.b32", unsigned, "r", r | v) MCUDA_ATOM(atomicOr, "or.b64", unsigned long long, "l", r | v)
MCUDA_ATOM(atomicXor, "xor.b32", int, "r", r ^ v) MCUDA_ATOM(atomicXor, "xor.b32", unsigned, "r", r ^ v) MCUDA_ATOM(atomicXor, "xor.b64", unsigned long long, "l", r ^ v)
#undef MCUDA_ATOM
__host__ __device__ static __forceinline__ float atomicExch(float *p, float v) { return __uint_as_float(atomicExch((unsigned *)p, __float_as_uint(v))); }
__host__ __device__ static __forceinline__ int atomicSub(int *p, int v) { return atomicAdd(p, -v); }
__host__ __device__ static __forceinline__ unsigned atomicSub(unsigned *p, unsigned v) { return atomicAdd(p, 0u - v); }
#define MCUDA_CAS(T, ty, cls) \
    __host__ __device__ static __forceinline__ T atomicCAS(T *p, T cmp, T v) { \
        T r; \
        if (__builtin_mcuda_device()) asm volatile("atom.cas." ty " %0, [%1], %2, %3;" : "=" cls(r) : "l"(p), cls(cmp), cls(v)); \
        else { r = *p; if (r == cmp) *p = v; } \
        return r; }
MCUDA_CAS(int, "b32", "r") MCUDA_CAS(unsigned, "b32", "r") MCUDA_CAS(unsigned long long, "b64", "l") MCUDA_CAS(unsigned long, "b64", "l")
#undef MCUDA_CAS

/* more of the mathematical library */
extern "C" {
float sinpif(float); float cospif(float); double sinpi(double); double cospi(double);
void sincosf(float, float *, float *); void sincos(double, double *, double *);
float hypotf(float, float); double hypot(double, double); float ldexpf(float, int); double ldexp(double, int);
float frexpf(float, int *); double frexp(double, int *); int ilogbf(float); int ilogb(double);
long lrintf(float); long lrint(double); long lroundf(float); long lround(double); long long llround(double); long long llroundf(float);
long long llrint(double); long long llrintf(float); float remainderf(float, float); double remainder(double, double);
float erfcf(float); double erfc(double); double log1p(double); double expm1(double); double log10(double); double cbrt(double);
double tan(double); double atan(double); double asin(double); double acos(double); double sinh(double); double cosh(double);
float nanf(const char *); double nan(const char *);
}
__device__ static __forceinline__ void __sincosf(float a, float *s, float *c) { sincosf(a, s, c); }
__device__ static __forceinline__ void sincospif(float a, float *s, float *c) { *s = sinpif(a); *c = cospif(a); }
__device__ static __forceinline__ void sincospi(double a, double *s, double *c) { *s = sinpi(a); *c = cospi(a); }
/* the inverse of the normal distribution's cumulative function (Wichura's AS 241, PPND16) */
__host__ __device__ static inline double normcdfinv(double p)
{
    double q = p - 0.5, r, v;
    if (!(p > 0.0 && p < 1.0)) return p == 0.0 ? -1.0 / 0.0 : p == 1.0 ? 1.0 / 0.0 : 0.0 / 0.0;
    if (fabs(q) <= 0.425) {
        r = 0.180625 - q * q;
        return q * (((((((2.5090809287301226727e3 * r + 3.3430575583588128105e4) * r + 6.7265770927008700853e4) * r + 4.5921953931549871457e4) * r + 1.3731693765509461125e4) * r + 1.9715909503065514427e3) * r + 1.3314166789178437745e2) * r + 3.3871328727963666080e0) /
               (((((((5.2264952788528545610e3 * r + 2.8729085735721942674e4) * r + 3.9307895800092710610e4) * r + 2.1213794301586595867e4) * r + 5.3941960214247511077e3) * r + 6.8718700749205790830e2) * r + 4.2313330701600911252e1) * r + 1.0);
    }
    r = q < 0 ? p : 1.0 - p;
    r = sqrt(-log(r));
    if (r <= 5.0) {
        r -= 1.6;
        v = (((((((7.74545014278341407640e-4 * r + 2.27238449892691845833e-2) * r + 2.41780725177450611770e-1) * r + 1.27045825245236838258e0) * r + 3.64784832476320460504e0) * r + 5.76949722146069140550e0) * r + 4.63033784615654529590e0) * r + 1.42343711074968357734e0) /
            (((((((1.05075007164441684324e-9 * r + 5.47593808499534494600e-4) * r + 1.51986665636164571966e-2) * r + 1.48103976427480074590e-1) * r + 6.89767334985100004550e-1) * r + 1.67638483018380384940e0) * r + 2.05319162663775882187e0) * r + 1.0);
    } else {
        r -= 5.0;
        v = (((((((2.01033439929228813265e-7 * r + 2.71155556874348757815e-5) * r + 1.24266094738807843860e-3) * r + 2.65321895265761230930e-2) * r + 2.96560571828504891230e-1) * r + 1.78482653991729133580e0) * r + 5.46378491116411436990e0) * r + 6.65790464350110377720e0) /
            (((((((2.04426310338993978564e-15 * r + 1.42151175831644588870e-7) * r + 1.84631831751005468180e-5) * r + 7.86869131145613259100e-4) * r + 1.48753612908506148525e-2) * r + 1.36929880922735805310e-1) * r + 5.99832206555887937690e-1) * r + 1.0);
    }
    return q < 0 ? -v : v;
}
__device__ static __forceinline__ unsigned __activemask(void) { unsigned r; asm volatile("activemask.b32 %0;" : "=r"(r)); return r; }
__device__ static __forceinline__ unsigned __match_any_sync(unsigned mask, unsigned v) { unsigned r; asm volatile("match.any.sync.b32 %0, %1, %2;" : "=r"(r) : "r"(v), "r"(mask)); return r; }
__device__ static __forceinline__ unsigned __match_any_sync(unsigned mask, int v) { return __match_any_sync(mask, (unsigned)v); }
__device__ static __forceinline__ unsigned __match_any_sync(unsigned mask, unsigned long long v) { unsigned r; asm volatile("match.any.sync.b64 %0, %1, %2;" : "=r"(r) : "l"(v), "r"(mask)); return r; }

/* memory */
/* ld.global.nc where T is a number or a pointer (mcc also reads "const T *__restrict__" kernel parameters so); in host
   code a plain read */
template <class T> __host__ __device__ static __forceinline__ T __ldg(const T *p) {
    if (__builtin_mcuda_device()) return __builtin_mcc_ldg(p);
    return *p; }
__device__ static __forceinline__ unsigned long __cvta_generic_to_shared(const void *p) { unsigned long r; asm("cvta.to.shared.u64 %0, %1;" : "=l"(r) : "l"(p)); return r; }

/* warps */
#define MCUDA_SHFL(name, mode, T, cls) \
    __device__ static __forceinline__ T name(unsigned mask, T v, unsigned d, int w = 32) { T r; unsigned c = ((32 - w) << 8) | cbits; \
        asm volatile("shfl.sync." mode ".b32 %0, %1, %2, %3, %4;" : "=" cls(r) : cls(v), "r"(d), "r"(c), "r"(mask)); return r; }
#define cbits 0x1f
MCUDA_SHFL(__shfl_xor_sync, "bfly", int, "r") MCUDA_SHFL(__shfl_xor_sync, "bfly", unsigned, "r") MCUDA_SHFL(__shfl_xor_sync, "bfly", float, "f")
MCUDA_SHFL(__shfl_down_sync, "down", int, "r") MCUDA_SHFL(__shfl_down_sync, "down", unsigned, "r") MCUDA_SHFL(__shfl_down_sync, "down", float, "f")
MCUDA_SHFL(__shfl_sync, "idx", int, "r") MCUDA_SHFL(__shfl_sync, "idx", unsigned, "r") MCUDA_SHFL(__shfl_sync, "idx", float, "f")
#undef cbits
#define cbits 0
MCUDA_SHFL(__shfl_up_sync, "up", int, "r") MCUDA_SHFL(__shfl_up_sync, "up", unsigned, "r") MCUDA_SHFL(__shfl_up_sync, "up", float, "f")
#undef cbits
#undef MCUDA_SHFL
/* 64-bit values: two shuffles, of the low and of the high half (never through float) */
#define MCUDA_SHFL64(name, T) \
    __device__ static __forceinline__ T name(unsigned mask, T v, unsigned d, int w = 32) { unsigned long long u = (unsigned long long)v; \
        unsigned lo = name(mask, (unsigned)u, d, w), hi = name(mask, (unsigned)(u >> 32), d, w); return (T)((unsigned long long)hi << 32 | lo); }
#define MCUDA_SHFLD(name) \
    __device__ static __forceinline__ double name(unsigned mask, double v, unsigned d, int w = 32) { unsigned long long u = (unsigned long long)__double_as_longlong(v); \
        unsigned lo = name(mask, (unsigned)u, d, w), hi = name(mask, (unsigned)(u >> 32), d, w); return __longlong_as_double((long long)((unsigned long long)hi << 32 | lo)); }
#define MCUDA_SHFLALL(name) MCUDA_SHFL64(name, long) MCUDA_SHFL64(name, unsigned long) MCUDA_SHFL64(name, long long) MCUDA_SHFL64(name, unsigned long long) MCUDA_SHFLD(name)
MCUDA_SHFLALL(__shfl_xor_sync) MCUDA_SHFLALL(__shfl_down_sync) MCUDA_SHFLALL(__shfl_sync) MCUDA_SHFLALL(__shfl_up_sync)
#undef MCUDA_SHFL64
#undef MCUDA_SHFLD
#undef MCUDA_SHFLALL
__device__ static __forceinline__ unsigned __ballot_sync(unsigned mask, int p) { unsigned r; asm volatile("{ .reg .pred q; setp.ne.b32 q, %1, 0; vote.sync.ballot.b32 %0, q, %2; }" : "=r"(r) : "r"(p), "r"(mask)); return r; }
__device__ static __forceinline__ int __any_sync(unsigned mask, int p) { int r; asm volatile("{ .reg .pred q; setp.ne.b32 q, %1, 0; vote.sync.any.pred q, q, %2; selp.b32 %0, 1, 0, q; }" : "=r"(r) : "r"(p), "r"(mask)); return r; }
__device__ static __forceinline__ int __all_sync(unsigned mask, int p) { int r; asm volatile("{ .reg .pred q; setp.ne.b32 q, %1, 0; vote.sync.all.pred q, q, %2; selp.b32 %0, 1, 0, q; }" : "=r"(r) : "r"(p), "r"(mask)); return r; }
__device__ static __forceinline__ int __reduce_max_sync(unsigned mask, int v) { int r; asm volatile("redux.sync.max.s32 %0, %1, %2;" : "=r"(r) : "r"(v), "r"(mask)); return r; }
__device__ static __forceinline__ unsigned __reduce_max_sync(unsigned mask, unsigned v) { unsigned r; asm volatile("redux.sync.max.u32 %0, %1, %2;" : "=r"(r) : "r"(v), "r"(mask)); return r; }
__device__ static __forceinline__ int __reduce_min_sync(unsigned mask, int v) { int r; asm volatile("redux.sync.min.s32 %0, %1, %2;" : "=r"(r) : "r"(v), "r"(mask)); return r; }
__device__ static __forceinline__ unsigned __reduce_min_sync(unsigned mask, unsigned v) { unsigned r; asm volatile("redux.sync.min.u32 %0, %1, %2;" : "=r"(r) : "r"(v), "r"(mask)); return r; }
__device__ static __forceinline__ unsigned __reduce_add_sync(unsigned mask, unsigned v) { unsigned r; asm volatile("redux.sync.add.u32 %0, %1, %2;" : "=r"(r) : "r"(v), "r"(mask)); return r; }
__device__ static __forceinline__ unsigned __reduce_or_sync(unsigned mask, unsigned v) { unsigned r; asm volatile("redux.sync.or.b32 %0, %1, %2;" : "=r"(r) : "r"(v), "r"(mask)); return r; }
__device__ static __forceinline__ int __reduce_add_sync(unsigned mask, int v) { int r; asm volatile("redux.sync.add.s32 %0, %1, %2;" : "=r"(r) : "r"(v), "r"(mask)); return r; }
__device__ static __forceinline__ unsigned __reduce_and_sync(unsigned mask, unsigned v) { unsigned r; asm volatile("redux.sync.and.b32 %0, %1, %2;" : "=r"(r) : "r"(v), "r"(mask)); return r; }
__device__ static __forceinline__ unsigned __reduce_xor_sync(unsigned mask, unsigned v) { unsigned r; asm volatile("redux.sync.xor.b32 %0, %1, %2;" : "=r"(r) : "r"(v), "r"(mask)); return r; }

/* A device variable is named in host code by the host's object of that name: the run time copies to and from
   wherever the device has it (in mcuda the object itself). */
extern "C" int __mcuda_to_symbol(const void *symbol, const void *src, unsigned long count, unsigned long offset, int kind);
extern "C" int __mcuda_from_symbol(void *dst, const void *symbol, unsigned long count, unsigned long offset, int kind);
template <class T> __host__ static __forceinline__ int cudaMemcpyToSymbol(const T &symbol, const void *src, unsigned long count, unsigned long offset = 0, int kind = 1) {
    return __mcuda_to_symbol((const void *)&symbol, src, count, offset, kind);
}
template <class T> __host__ static __forceinline__ int cudaMemcpyFromSymbol(void *dst, const T &symbol, unsigned long count, unsigned long offset = 0, int kind = 2) {
    return __mcuda_from_symbol(dst, (const void *)&symbol, count, offset, kind);
}

#endif
