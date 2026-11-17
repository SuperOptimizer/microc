/* cublasLtMatmul on numbers that do not come out exact: every way the run times support it (operand and result
   types, transposed operands, alpha and beta, in place, bias, relu, a batch, rows longer than the matrix), and of
   each result a sum of its bits. tests/hostfs/gpu.sh runs this on the CPU's run time and on the GPU's (our kernel,
   cuda/hw/ltk.cu): the two must print the same, bit for bit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <cuda_bf16.h>
#include <cublasLt.h>

static cublasLtHandle_t lt;
static unsigned seed = 12345;
static float rnd(void)
{
    seed = seed * 1664525u + 1013904223u;
    return (float)(int)(seed >> 8 & 0xffff) / 4099.0f - 7.9f;
}
static size_t esize(cudaDataType_t t) { return t == CUDA_R_32F ? 4 : 2; }

/* n random numbers of type t on the device */
static void *up(size_t n, cudaDataType_t t)
{
    void *d, *h = malloc(n * 4);
    size_t i;
    for (i = 0; i < n; i++) {
        float v = rnd();
        if (t == CUDA_R_32F) ((float *)h)[i] = v;
        else if (t == CUDA_R_16F) ((__half *)h)[i] = __float2half(v);
        else ((__nv_bfloat16 *)h)[i] = __float2bfloat16(v);
    }
    cudaMalloc(&d, n * esize(t));
    cudaMemcpy(d, h, n * esize(t), cudaMemcpyHostToDevice);
    free(h);
    return d;
}

static void run(const char *name, cudaDataType_t ta, cudaDataType_t tb, cudaDataType_t td, int transa, int transb, int m, int n, int k,
                int pad, int batch, float alpha, float beta, int epi)
{
    cublasLtMatmulDesc_t op; cublasLtMatrixLayout_t la, lb, ld;
    const cublasOperation_t oa = transa ? CUBLAS_OP_T : CUBLAS_OP_N, ob = transb ? CUBLAS_OP_T : CUBLAS_OP_N;
    const int ra = transa ? k : m, ca = transa ? m : k, rb = transb ? n : k, cb = transb ? k : n;
    const int lda = ra + pad, ldb = rb + pad, ldd = m + pad;
    const long long sa = (long long)lda * ca + 3, sb = (long long)ldb * cb + 5, sd = (long long)ldd * n + 1;
    void *dA = up((size_t)sa * batch, ta), *dB = up((size_t)sb * batch, tb), *dD = up((size_t)sd * batch, td), *dbias = up((size_t)m, td);
    const size_t bytes = (size_t)sd * batch * esize(td);
    unsigned char *out = (unsigned char *)malloc(bytes);
    unsigned h = 2166136261u; size_t i; cublasStatus_t s;
    cublasLtMatmulDescCreate(&op, CUBLAS_COMPUTE_32F, CUDA_R_32F);
    cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_TRANSA, &oa, sizeof oa);
    cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_TRANSB, &ob, sizeof ob);
    if (epi) {
        const cublasLtEpilogue_t e = epi == 1 ? CUBLASLT_EPILOGUE_RELU : epi == 2 ? CUBLASLT_EPILOGUE_BIAS : CUBLASLT_EPILOGUE_RELU_BIAS;
        const void *bp = dbias;
        cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_EPILOGUE, &e, sizeof e);
        cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_BIAS_POINTER, &bp, sizeof bp);
    }
    cublasLtMatrixLayoutCreate(&la, ta, ra, ca, lda); cublasLtMatrixLayoutCreate(&lb, tb, rb, cb, ldb); cublasLtMatrixLayoutCreate(&ld, td, m, n, ldd);
    if (batch > 1) {
        cublasLtMatrixLayoutSetAttribute(la, CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT, &batch, sizeof batch);
        cublasLtMatrixLayoutSetAttribute(lb, CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT, &batch, sizeof batch);
        cublasLtMatrixLayoutSetAttribute(ld, CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT, &batch, sizeof batch);
        cublasLtMatrixLayoutSetAttribute(la, CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET, &sa, sizeof sa);
        cublasLtMatrixLayoutSetAttribute(lb, CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET, &sb, sizeof sb);
        cublasLtMatrixLayoutSetAttribute(ld, CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET, &sd, sizeof sd);
    }
    s = cublasLtMatmul(lt, op, &alpha, dA, la, dB, lb, &beta, dD, ld, dD, ld, 0, 0, 0, 0);
    if (s != CUBLAS_STATUS_SUCCESS) { printf("%-34s failed (%d)\n", name, (int)s); return; }
    cudaDeviceSynchronize();
    cudaMemcpy(out, dD, bytes, cudaMemcpyDeviceToHost);
    for (i = 0; i < bytes; i++) h = (h ^ out[i]) * 16777619u;
    printf("%-34s %08x %s\n", name, h, cudaGetErrorString(cudaGetLastError()));
    free(out);
    cublasLtMatrixLayoutDestroy(la); cublasLtMatrixLayoutDestroy(lb); cublasLtMatrixLayoutDestroy(ld); cublasLtMatmulDescDestroy(op);
    cudaFree(dA); cudaFree(dB); cudaFree(dD); cudaFree(dbias);
}

int main(void)
{
    const cudaDataType_t F = CUDA_R_32F, H = CUDA_R_16F, B = CUDA_R_16BF;
    if (cublasLtCreate(&lt) != CUBLAS_STATUS_SUCCESS) { printf("no cuBLASLt\n"); return 1; }
    run("fp32", F, F, F, 0, 0, 37, 21, 50, 0, 1, 1.0f, 0.0f, 0);
    run("fp32, k 1", F, F, F, 0, 0, 5, 7, 1, 0, 1, 1.0f, 0.0f, 0);
    run("fp32, k 3", F, F, F, 0, 0, 16, 16, 3, 0, 1, 1.0f, 0.0f, 0);
    run("fp32, k 17", F, F, F, 0, 0, 17, 33, 17, 2, 1, 1.0f, 0.0f, 0);
    run("fp32, A transposed", F, F, F, 1, 0, 37, 21, 50, 3, 1, 1.0f, 0.0f, 0);
    run("fp32, B transposed", F, F, F, 0, 1, 37, 21, 50, 3, 1, 1.0f, 0.0f, 0);
    run("fp32, both transposed", F, F, F, 1, 1, 37, 21, 50, 1, 1, 1.0f, 0.0f, 0);
    run("fp32, alpha and beta", F, F, F, 0, 0, 40, 40, 64, 0, 1, -0.37f, 1.25f, 0);
    run("fp32, relu", F, F, F, 0, 0, 33, 18, 31, 0, 1, 1.0f, 0.5f, 1);
    run("fp32, bias", F, F, F, 1, 0, 33, 18, 31, 2, 1, 1.0f, 0.0f, 2);
    run("fp32, relu and bias", F, F, F, 0, 1, 33, 18, 31, 2, 1, 0.5f, 1.0f, 3);
    run("fp32, a batch of 5", F, F, F, 0, 0, 19, 23, 29, 1, 5, 1.0f, 0.0f, 0);
    run("fp32, a batch of 3, transposed", F, F, F, 1, 1, 19, 23, 29, 1, 3, 2.0f, 1.0f, 3);
    run("fp16 to fp32", H, H, F, 0, 0, 37, 21, 50, 0, 1, 1.0f, 0.0f, 0);
    run("fp16 to fp16", H, H, H, 1, 0, 37, 21, 50, 4, 1, 1.0f, 1.0f, 2);
    run("bf16 to fp32", B, B, F, 0, 1, 37, 21, 50, 0, 1, 1.0f, 0.0f, 0);
    run("bf16 to bf16", B, B, B, 0, 0, 37, 21, 50, 1, 2, 0.75f, 0.5f, 3);
    run("fp16 and bf16 to fp32", H, B, F, 1, 1, 24, 48, 40, 0, 1, 1.0f, 0.0f, 0);
    run("fp32, 300 x 200 x 257", F, F, F, 0, 0, 300, 200, 257, 0, 1, 1.0f, 0.0f, 0);
    run("fp32, 513 x 129 x 1025", F, F, F, 1, 0, 513, 129, 1025, 0, 1, 1.0f, 1.0f, 2);
    return 0;
}
