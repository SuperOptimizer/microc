/* nvcc: -lcublasLt */
/* cuBLASLt's matrix product the ways programs ask for it: fp32 (plain and through tf32), a transposed first
   operand, alpha and beta with the sum in place, fp16 and bf16 operands with rows longer than the matrix, an
   fp16 result, a bias added to each column. The entries are small integers, so the results are exact. */
#include <stdio.h>
#include <string.h>
#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <cuda_bf16.h>
#include <cublasLt.h>

enum { M = 37, N = 21, K = 50, LD = 56 };
static cublasLtHandle_t lt; static void *ws; static size_t WS = 4 << 20;
static int hA[LD * 64], hB[LD * 64], hC[64 * 64], hbias[64];

static size_t esize(cudaDataType_t t) { return t == CUDA_R_32F ? 4 : 2; }
/* integers as numbers of type t on the device */
static void *up(const int *v, int n, cudaDataType_t t)
{
    void *d; static float f[64 * 64]; static __half h[64 * 64]; static __nv_bfloat16 b[64 * 64];
    for (int i = 0; i < n; i++) { f[i] = (float)v[i]; h[i] = __float2half((float)v[i]); b[i] = __float2bfloat16((float)v[i]); }
    cudaMalloc(&d, (size_t)n * esize(t));
    cudaMemcpy(d, t == CUDA_R_32F ? (void *)f : t == CUDA_R_16F ? (void *)h : (void *)b, (size_t)n * esize(t), cudaMemcpyHostToDevice);
    return d;
}

/* D (m x n, column major) = alpha op(A) B + beta D [+ bias]: A is m x k, or k x m when ta; rows of A and B are lda and ldb long */
static void run(const char *name, cublasComputeType_t ct, cudaDataType_t tin, cudaDataType_t tout, int ta, int m, int n, int k, int lda, int ldb,
                float alpha, float beta, int bias)
{
    cublasLtMatmulDesc_t op; cublasLtMatrixLayout_t la, lb, ld; cublasLtMatmulPreference_t pref; cublasLtMatmulHeuristicResult_t h; int nh = 0;
    const cublasOperation_t oa = ta ? CUBLAS_OP_T : CUBLAS_OP_N, ob = CUBLAS_OP_N;
    void *dA = up(hA, lda * (ta ? m : k), tin), *dB = up(hB, ldb * n, tin), *dD = up(hC, m * n, tout), *db = up(hbias, m, tout);
    static float of[64 * 64]; static __half oh[64 * 64]; int wrong = 0; long sum = 0; cublasStatus_t s;
    cublasLtMatmulDescCreate(&op, ct, CUDA_R_32F);
    cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_TRANSA, &oa, sizeof oa);
    cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_TRANSB, &ob, sizeof ob);
    if (bias) {
        const cublasLtEpilogue_t e = CUBLASLT_EPILOGUE_BIAS; const void *bp = db;
        cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_EPILOGUE, &e, sizeof e);
        cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_BIAS_DATA_TYPE, &tout, sizeof tout);
        cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_BIAS_POINTER, &bp, sizeof bp);
    }
    if (ta) cublasLtMatrixLayoutCreate(&la, tin, k, m, lda); else cublasLtMatrixLayoutCreate(&la, tin, m, k, lda);
    cublasLtMatrixLayoutCreate(&lb, tin, k, n, ldb); cublasLtMatrixLayoutCreate(&ld, tout, m, n, m);
    cublasLtMatmulPreferenceCreate(&pref);
    cublasLtMatmulPreferenceSetAttribute(pref, CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES, &WS, sizeof WS);
    s = cublasLtMatmulAlgoGetHeuristic(lt, op, la, lb, ld, ld, pref, 1, &h, &nh);
    cublasLtMatmulPreferenceDestroy(pref);
    if (s != CUBLAS_STATUS_SUCCESS || nh < 1) { printf("%-28s no way to do it (%d)\n", name, (int)s); return; }
    s = cublasLtMatmul(lt, op, &alpha, dA, la, dB, lb, &beta, dD, ld, dD, ld, &h.algo, ws, WS, 0);
    if (s != CUBLAS_STATUS_SUCCESS) { printf("%-28s failed (%d)\n", name, (int)s); return; }
    cudaMemcpy(tout == CUDA_R_32F ? (void *)of : (void *)oh, dD, (size_t)(m * n) * esize(tout), cudaMemcpyDeviceToHost);
    for (int j = 0; j < n; j++)
        for (int i = 0; i < m; i++) {
            long w = 0; float got = tout == CUDA_R_32F ? of[j * m + i] : __half2float(oh[j * m + i]), want;
            for (int x = 0; x < k; x++) w += (long)(ta ? hA[i * lda + x] : hA[x * lda + i]) * hB[j * ldb + x];
            want = alpha * (float)w + beta * (float)hC[j * m + i] + (bias ? (float)hbias[i] : 0.0f);
            if (got != want) { if (!wrong) printf("%s: row %d column %d: %g, not %g\n", name, i, j, got, want); wrong++; }
            sum += (long)got * (i * 7 + j + 1);
        }
    printf("%-28s wrong %d sum %ld\n", name, wrong, sum);
    cublasLtMatrixLayoutDestroy(la); cublasLtMatrixLayoutDestroy(lb); cublasLtMatrixLayoutDestroy(ld); cublasLtMatmulDescDestroy(op);
    cudaFree(dA); cudaFree(dB); cudaFree(dD); cudaFree(db);
}

int main(void)
{
    unsigned x = 777;
    for (int i = 0; i < LD * 64; i++) { x = x * 1103515245u + 12345u; hA[i] = (int)((x >> 16) % 7) - 3; x = x * 1103515245u + 12345u; hB[i] = (int)((x >> 16) % 5) - 2; }
    for (int i = 0; i < 64 * 64; i++) { x = x * 1103515245u + 12345u; hC[i] = (int)((x >> 16) % 9) - 4; }
    for (int i = 0; i < 64; i++) hbias[i] = i % 11 - 5;
    if (cublasLtCreate(&lt) != CUBLAS_STATUS_SUCCESS || cudaMalloc(&ws, WS) != cudaSuccess) { printf("no cuBLASLt\n"); return 1; }
    run("fp32", CUBLAS_COMPUTE_32F, CUDA_R_32F, CUDA_R_32F, 0, M, N, K, M, K, 1, 0, 0);
    run("fp32 through tf32", CUBLAS_COMPUTE_32F_FAST_TF32, CUDA_R_32F, CUDA_R_32F, 0, M, N, K, M, K, 1, 0, 0);
    run("transposed", CUBLAS_COMPUTE_32F_FAST_TF32, CUDA_R_32F, CUDA_R_32F, 1, M, N, K, K, K, 1, 0, 0);
    run("alpha, beta", CUBLAS_COMPUTE_32F, CUDA_R_32F, CUDA_R_32F, 1, M, N, K, LD, LD, 2, 1, 0);
    run("alpha, beta, not transposed", CUBLAS_COMPUTE_32F, CUDA_R_32F, CUDA_R_32F, 0, M, N, K, M + 3, LD, -1, 3, 0);
    run("fp16 to fp32", CUBLAS_COMPUTE_32F, CUDA_R_16F, CUDA_R_32F, 1, M, N, K, LD, K, 1, 0, 0);
    run("fp16 to fp32, bias", CUBLAS_COMPUTE_32F, CUDA_R_16F, CUDA_R_32F, 1, M, N, K, LD, K, 1, 0, 1);
    run("fp16 to fp32, bias, beta", CUBLAS_COMPUTE_32F, CUDA_R_16F, CUDA_R_32F, 1, M, N, K, LD, K, 1, 1, 1);
    run("fp16", CUBLAS_COMPUTE_32F, CUDA_R_16F, CUDA_R_16F, 1, M, N, K, LD, K, 1, 0, 0);
    run("fp16, bias, beta", CUBLAS_COMPUTE_32F, CUDA_R_16F, CUDA_R_16F, 1, M, N, K, LD, K, 1, 1, 1);
    run("bf16 to fp32", CUBLAS_COMPUTE_32F, CUDA_R_16BF, CUDA_R_32F, 1, M, N, K, LD, K, 1, 0, 0);
    run("bf16 to fp32, bias, beta", CUBLAS_COMPUTE_32F, CUDA_R_16BF, CUDA_R_32F, 0, M, N, K, M, LD, 1, 1, 1);
    run("one row", CUBLAS_COMPUTE_32F, CUDA_R_32F, CUDA_R_32F, 0, 1, 3, 2, 1, 2, 1, 0, 0);
    run("large", CUBLAS_COMPUTE_32F, CUDA_R_16F, CUDA_R_32F, 1, 64, 64, 56, 56, 56, 1, 0, 1);
    cublasLtDestroy(lt); cudaFree(ws);
    return 0;
}
