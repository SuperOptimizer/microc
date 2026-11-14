/* mcc CUDA: bfloat16. __nv_bfloat16 is the compiler's __bf16. */
#ifndef MCUDA_BF16_H
#define MCUDA_BF16_H
typedef __bf16 __nv_bfloat16;
typedef __nv_bfloat16 nv_bfloat16;
struct __nv_bfloat162 {
    __nv_bfloat16 x, y;
    __nv_bfloat162() {}
    __nv_bfloat162(__nv_bfloat16 a, __nv_bfloat16 b) : x(a), y(b) {}
};
typedef __nv_bfloat162 nv_bfloat162;
__host__ __device__ static __forceinline__ __nv_bfloat16 __float2bfloat16(float f) { return (__nv_bfloat16)f; }
__host__ __device__ static __forceinline__ __nv_bfloat16 __float2bfloat16_rn(float f) { return (__nv_bfloat16)f; }
__host__ __device__ static __forceinline__ float __bfloat162float(__nv_bfloat16 h) { return (float)h; }
__host__ __device__ static __forceinline__ __nv_bfloat162 __floats2bfloat162_rn(float a, float b) { __nv_bfloat162 r = {(__nv_bfloat16)a, (__nv_bfloat16)b}; return r; }
__host__ __device__ static __forceinline__ __nv_bfloat162 __float2bfloat162_rn(float a) { __nv_bfloat162 r = {(__nv_bfloat16)a, (__nv_bfloat16)a}; return r; }
__host__ __device__ static __forceinline__ __nv_bfloat162 __halves2bfloat162(__nv_bfloat16 a, __nv_bfloat16 b) { __nv_bfloat162 r = {a, b}; return r; }
__host__ __device__ static __forceinline__ float2 __bfloat1622float2(__nv_bfloat162 h) { float2 r = {(float)h.x, (float)h.y}; return r; }
__host__ __device__ static __forceinline__ unsigned short __bfloat16_as_ushort(__nv_bfloat16 h) { union { unsigned short u; __nv_bfloat16 h; } v; v.h = h; return v.u; }
__host__ __device__ static __forceinline__ __nv_bfloat16 __ushort_as_bfloat16(unsigned short u) { union { unsigned short u; __nv_bfloat16 h; } v; v.u = u; return v.h; }
#endif
