/* mcudahw: the matrix product of cublasLtMatmul as a kernel of our own (lt.c built with MCUDA_LT_OWN), so that
   nothing of NVIDIA's cuBLAS is loaded. It computes what the CPU run time computes (cuda/cublaslt.c, as
   cublasLt.h describes it), and rounds the same way: every product and every sum is rounded on its own
   (this file is compiled with -fmad=false), the sum over k runs in four partial sums by k mod 4 that are
   added as (s0 + s1) + (s2 + s3), and a 16-bit result is rounded once from the float.

   One block is 16 x 16 elements of D, a thread to each. The block goes over k sixteen at a time: its threads
   bring 16 x 16 of op(A) and of op(B) into shared memory as floats, and each then adds its sixteen products.
   What lies outside the matrices is loaded as 0, which adds nothing to a partial sum. */
#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <cuda_bf16.h>
#include <library_types.h>

#define TS 16

__device__ static __forceinline__ float lt_get(const void *p, long i, int type)
{
    if (type == CUDA_R_32F)
        return ((const float *)p)[i];
    if (type == CUDA_R_16F)
        return (float)((const __half *)p)[i];
    return (float)((const __nv_bfloat16 *)p)[i];
}

/* types: those of A, B, D (and C) and the bias, four bits each; flags: 1 A transposed, 2 B transposed, 4 relu;
   C and bias are 0 when there is none; sa .. sd: the elements between the matrices of a batch */
__global__ void __mcudahw_lt_k(const void *A, const void *B, const void *C, void *D, const void *bias, int m, int n, int k, long lda,
                               long ldb, long ldc, long ldd, long sa, long sb, long sc, long sd, int types, int flags, float alpha,
                               float beta)
{
    __shared__ float a[TS][TS], b[TS][TS];
    const int tx = threadIdx.x, ty = threadIdx.y;
    const int i = blockIdx.x * TS + tx, j = blockIdx.y * TS + ty;
    const long z = blockIdx.z;
    const int ta = types & 15, tb = types >> 4 & 15, td = types >> 8 & 15, tbias = types >> 12 & 15;
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0, v;
    int kt, l;
    for (kt = 0; kt < k; kt += TS) {
        float x = 0, y = 0;
        l = kt + ty; /* a[tx][ty]: op(A)(i, kt + ty) */
        if (i < m && l < k)
            x = lt_get(A, z * sa + (flags & 1 ? (long)i * lda + l : (long)l * lda + i), ta);
        l = kt + tx; /* b[tx][ty]: op(B)(kt + tx, j) */
        if (j < n && l < k)
            y = lt_get(B, z * sb + (flags & 2 ? (long)l * ldb + j : (long)j * ldb + l), tb);
        a[tx][ty] = x;
        b[tx][ty] = y;
        __syncthreads();
        for (l = 0; l < TS; l += 4) {
            s0 += a[tx][l] * b[l][ty];
            s1 += a[tx][l + 1] * b[l + 1][ty];
            s2 += a[tx][l + 2] * b[l + 2][ty];
            s3 += a[tx][l + 3] * b[l + 3][ty];
        }
        __syncthreads();
    }
    if (i >= m || j >= n)
        return;
    v = alpha * ((s0 + s1) + (s2 + s3));
    if (C)
        v += beta * lt_get(C, z * sc + (long)j * ldc + i, td);
    if (bias)
        v += lt_get(bias, i, tbias);
    if ((flags & 4) && v < 0)
        v = 0;
    if (td == CUDA_R_32F)
        ((float *)D)[z * sd + (long)j * ldd + i] = v;
    else if (td == CUDA_R_16F)
        ((__half *)D)[z * sd + (long)j * ldd + i] = (__half)v;
    else
        ((__nv_bfloat16 *)D)[z * sd + (long)j * ldd + i] = (__nv_bfloat16)v;
}

extern "C" int __mcudahw_lt_gemm(const void *A, const void *B, const void *C, void *D, const void *bias, int m, int n, int k, long lda,
                                 long ldb, long ldc, long ldd, long sa, long sb, long sc, long sd, int batch, int types, int flags,
                                 float alpha, float beta, void *stream)
{
    dim3 grid((unsigned)((m + TS - 1) / TS), (unsigned)((n + TS - 1) / TS), (unsigned)batch), block(TS, TS, 1);
    __mcudahw_lt_k<<<grid, block, 0, (cudaStream_t)stream>>>(A, B, C, D, bias, m, n, k, lda, ldb, ldc, ldd, sa, sb, sc, sd, types, flags,
                                                           alpha, beta);
    return 0;
}
