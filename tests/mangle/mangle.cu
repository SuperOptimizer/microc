/* Kernel names as nvcc writes them (the Itanium C++ ABI): template arguments with their types (Li4E), the return type of a
   function template's instance, template parameters in the signature (T_), substitutions (S_, S0_, ...), top-level qualifiers
   of parameters dropped, __half / __nv_bfloat16 as the classes they are in CUDA. tests/mangle.sh compiles this with mcc and
   compares the PTX's .entry names with mangle.exp, which clang++ wrote from the same declarations (nvcc's names are the same). */
#ifndef __CUDACC__
#define __global__
struct __half { unsigned short x; };
struct __nv_bfloat16 { unsigned short x; };
#else
#include <cuda_fp16.h>
#include <cuda_bf16.h>
#endif
struct gnp_t { int a; };
struct mx4_t { int x; };
struct mx8_t { int y; };
template <int N> struct f4p_nit { static constexpr int value = N; };

__global__ void k_plain(const float *a, const float *b, float *c, int n) {}
__global__ void k_repeat(const float *a, float *b, const float *c, float *d, long long n, const int *e) {}
__global__ void k_noargs() {}
__global__ void k_constparam(const int n, float *const p, const float *q) {}
__global__ void k_half(const __half *a, __half *b, const __nv_bfloat16 *c, __nv_bfloat16 *d) {}
__global__ void k_struct(gnp_t g, const gnp_t *h, mx4_t m, const mx4_t *n) {}
template <int B> __global__ void k_int(const float *x, const float *g, float *y, float *z, int n) {}
template <int N> __global__ void k_neg(int *x) {}
template <unsigned U, bool F, long L> __global__ void k_vals(float *x) {}
template <typename T> __global__ void k_type(T *y, const T *x, unsigned long n) {}
template <typename T, int V> __global__ void k_mix(const T *x, T *y, gnp_t g) {}
template <typename A, typename B> __global__ void k_two(const A *a, B *b, const A *c, int n) {}
template <typename T, typename R> __global__ void k_cls(const T *x, R r, const R *s) {}
template <typename T> __global__ void k_halft(const T *a, __half *b, const __half *c) {}
template <typename T, typename I> __global__ void k_tag(const T *x, int n) {}

template __global__ void k_int<4>(const float *, const float *, float *, float *, int);
template __global__ void k_int<16>(const float *, const float *, float *, float *, int);
template __global__ void k_neg<-3>(int *);
template __global__ void k_vals<7u, true, -2L>(float *);
template __global__ void k_type<float>(float *, const float *, unsigned long);
template __global__ void k_type<__half>(__half *, const __half *, unsigned long);
template __global__ void k_mix<mx4_t, 8>(const mx4_t *, mx4_t *, gnp_t);
template __global__ void k_two<float, mx8_t>(const float *, mx8_t *, const float *, int);
template __global__ void k_two<mx4_t, mx4_t>(const mx4_t *, mx4_t *, const mx4_t *, int);
template __global__ void k_cls<float, gnp_t>(const float *, gnp_t, const gnp_t *);
template __global__ void k_halft<__nv_bfloat16>(const __nv_bfloat16 *, __half *, const __half *);
template __global__ void k_tag<mx4_t, f4p_nit<3>>(const mx4_t *, int);
