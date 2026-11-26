/* mcc CUDA: half precision. __half is the compiler's _Float16. */
#ifndef MCUDA_FP16_H
#define MCUDA_FP16_H
typedef _Float16 __half;
typedef __half half;
struct __attribute__((aligned(4))) __half2 {
    __half x, y;
    __half2() {}
    __half2(__half a, __half b) : x(a), y(b) {}
};
typedef __half2 half2;
__host__ __device__ static __forceinline__ __half __float2half(float f) { return (__half)f; }
__host__ __device__ static __forceinline__ __half __float2half_rn(float f) { return (__half)f; }
__host__ __device__ static __forceinline__ __half __double2half(double f) { return (__half)f; }
__host__ __device__ static __forceinline__ float __half2float(__half h) { return (float)h; }
__host__ __device__ static __forceinline__ __half __ushort_as_half(unsigned short u) { union { unsigned short u; __half h; } v; v.u = u; return v.h; }
__host__ __device__ static __forceinline__ unsigned short __half_as_ushort(__half h) { union { unsigned short u; __half h; } v; v.h = h; return v.u; }
__host__ __device__ static __forceinline__ short __half_as_short(__half h) { union { short u; __half h; } v; v.h = h; return v.u; }
__host__ __device__ static __forceinline__ __half __short_as_half(short u) { union { short u; __half h; } v; v.u = u; return v.h; }
__host__ __device__ static __forceinline__ __half2 __halves2half2(__half a, __half b) { __half2 r = {a, b}; return r; }
__host__ __device__ static __forceinline__ __half2 __floats2half2_rn(float a, float b) { __half2 r = {(__half)a, (__half)b}; return r; }
__host__ __device__ static __forceinline__ __half2 __float2half2_rn(float a) { __half2 r = {(__half)a, (__half)a}; return r; }
__host__ __device__ static __forceinline__ __half2 __float22half2_rn(float2 a) { __half2 r = {(__half)a.x, (__half)a.y}; return r; }
__host__ __device__ static __forceinline__ float2 __half22float2(__half2 h) { float2 r = {(float)h.x, (float)h.y}; return r; }
__host__ __device__ static __forceinline__ float __low2float(__half2 h) { return (float)h.x; }
__host__ __device__ static __forceinline__ float __high2float(__half2 h) { return (float)h.y; }
__host__ __device__ static __forceinline__ __half __low2half(__half2 h) { return h.x; }
__host__ __device__ static __forceinline__ __half __high2half(__half2 h) { return h.y; }
__host__ __device__ static __forceinline__ __half __hadd(__half a, __half b) { return a + b; }
__host__ __device__ static __forceinline__ __half __hsub(__half a, __half b) { return a - b; }
__host__ __device__ static __forceinline__ __half __hmul(__half a, __half b) { return a * b; }
__host__ __device__ static __forceinline__ __half __hfma(__half a, __half b, __half c) { return (__half)fmaf((float)a, (float)b, (float)c); }
/* both halves at once, as the GPU has it; in host code, one at a time */
#define MCUDA_H2(name, ins, hx, hy) \
    __host__ __device__ static __forceinline__ __half2 name(__half2 a, __half2 b) { \
        union { unsigned u; __half2 h; } p, q, r; \
        if (__builtin_mcuda_device()) { p.h = a; q.h = b; asm(ins ".f16x2 %0, %1, %2;" : "=r"(r.u) : "r"(p.u), "r"(q.u)); } \
        else { r.h.x = (hx); r.h.y = (hy); } \
        return r.h; }
MCUDA_H2(__hadd2, "add", a.x + b.x, a.y + b.y) MCUDA_H2(__hsub2, "sub", a.x - b.x, a.y - b.y) MCUDA_H2(__hmul2, "mul", a.x * b.x, a.y * b.y)
__host__ __device__ static __forceinline__ __half2 __hfma2(__half2 a, __half2 b, __half2 c) {
    union { unsigned u; __half2 h; } p, q, s, r;
    if (__builtin_mcuda_device()) { p.h = a; q.h = b; s.h = c; asm("fma.rn.f16x2 %0, %1, %2, %3;" : "=r"(r.u) : "r"(p.u), "r"(q.u), "r"(s.u)); }
    else { r.h.x = (__half)fmaf((float)a.x, (float)b.x, (float)c.x); r.h.y = (__half)fmaf((float)a.y, (float)b.y, (float)c.y); }
    return r.h; }
/* the larger and the smaller; a NaN loses to a number */
__host__ __device__ static __forceinline__ __half __hmax(__half a, __half b) { return a != a ? b : b != b ? a : a > b ? a : b; }
__host__ __device__ static __forceinline__ __half __hmin(__half a, __half b) { return a != a ? b : b != b ? a : a < b ? a : b; }
__host__ __device__ static __forceinline__ __half __hneg(__half a) { return -a; }
__host__ __device__ static __forceinline__ __half __hdiv(__half a, __half b) { return a / b; }
__device__ static __forceinline__ __half atomicAdd(__half *p, __half v) {
    unsigned r, b = __half_as_ushort(v);
    asm volatile("{ .reg .b16 a, o; cvt.u16.u32 a, %2; atom.add.noftz.f16 o, [%1], a; cvt.u32.u16 %0, o; }" : "=r"(r) : "l"(p), "r"(b));
    return __ushort_as_half((unsigned short)r);
}
#endif
