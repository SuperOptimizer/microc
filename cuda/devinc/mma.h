/* mcc CUDA: nvcuda::wmma for 16x16x16 tiles, with no instruction of its own. The lane l of a warp owns the
   eight elements 8 l .. 8 l + 7 (row-major) of an accumulator; A and B fragments only remember where their
   matrix lies, and mma_sync reads it there: so the matrices must stay as they were loaded until the
   multiplication, which is how the API is used. */
#ifndef MCUDA_MMA_H
#define MCUDA_MMA_H
#include <cuda_fp16.h>
namespace nvcuda {
namespace wmma {
struct matrix_a { char u; };
struct matrix_b { char u; };
struct accumulator { char u; };
struct row_major { char u; };
struct col_major { char u; };
enum layout_t { mem_row_major, mem_col_major };
template <class Use, int m, int n, int k, class T, class Layout = void> struct fragment {
    static constexpr int num_elements = 8;
    static constexpr bool row = !__is_same(Layout, col_major);
    T x[8];
    const T *p;
    unsigned ldm;
};
__device__ static __forceinline__ unsigned __wmma_lane(void) { return (threadIdx.x + blockDim.x * (threadIdx.y + blockDim.y * threadIdx.z)) & 31u; }
template <class F, class V> __device__ static __forceinline__ void fill_fragment(F &f, V v) { for (int i = 0; i < 8; i++) f.x[i] = v; }
template <class F, class T> __device__ static __forceinline__ void load_matrix_sync(F &f, const T *p, unsigned ldm) { f.p = p; f.ldm = ldm; }
template <class F, class T> __device__ static __forceinline__ void load_matrix_sync(F &f, const T *p, unsigned ldm, layout_t layout) {
    const unsigned l = __wmma_lane();
    for (int e = 0; e < 8; e++) { const unsigned i = (l * 8 + e) >> 4, j = (l * 8 + e) & 15; f.x[e] = layout == mem_row_major ? p[i * ldm + j] : p[j * ldm + i]; }
}
template <class T, class F> __device__ static __forceinline__ void store_matrix_sync(T *p, const F &f, unsigned ldm, layout_t layout) {
    const unsigned l = __wmma_lane();
    for (int e = 0; e < 8; e++) { const unsigned i = (l * 8 + e) >> 4, j = (l * 8 + e) & 15; if (layout == mem_row_major) p[i * ldm + j] = f.x[e]; else p[j * ldm + i] = f.x[e]; }
}
template <class D, class A, class B> __device__ static __forceinline__ void mma_sync(D &d, const A &a, const B &b, const D &c) {
    const unsigned l = __wmma_lane();
    for (int e = 0; e < 8; e++) {
        const unsigned i = (l * 8 + e) >> 4, j = (l * 8 + e) & 15;
        float s = (float)c.x[e];
        for (unsigned k = 0; k < 16; k++) {
            const float av = (float)(A::row ? a.p[i * a.ldm + k] : a.p[k * a.ldm + i]);
            const float bv = (float)(B::row ? b.p[k * b.ldm + j] : b.p[j * b.ldm + k]);
            s += av * bv;
        }
        d.x[e] = s;
    }
}
}
}
#endif
