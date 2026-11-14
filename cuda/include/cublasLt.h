/* cublasLt as mcuda provides it: the descriptor boilerplate and a CPU cublasLtMatmul.
   Names, argument orders and enumerator values are NVIDIA's (cuBLAS 12). Plain C, also valid C++.

   cublasLtMatmul computes, for each matrix of a batch,
       D = epilogue(alpha * op(A) * op(B) + beta * C)
   and supports exactly this (anything else returns CUBLAS_STATUS_NOT_SUPPORTED, from
   cublasLtMatmulAlgoGetHeuristic already):
     - A, B, C, D each CUDA_R_32F, CUDA_R_16F or CUDA_R_16BF, in any mix; C and D with equal layouts
     - compute type CUBLAS_COMPUTE_32F, _32F_PEDANTIC, _32F_FAST_16F, _32F_FAST_16BF or _32F_FAST_TF32
       (all computed alike: products and sums in IEEE single precision, inputs not rounded to TF32),
       scale type CUDA_R_32F: alpha and beta are host floats (pointer mode HOST)
     - column-major layouts with any leading dimension >= rows; batch count and strided batch offset
     - CUBLAS_OP_N and CUBLAS_OP_T on A and B (OP_C is OP_T for real data), none on C
     - C == D (in place) or C distinct from D; C is not read when beta is 0
     - epilogues DEFAULT, BIAS, RELU and RELU_BIAS; the bias (one value per row of D, shared by the batch)
       has the data type CUBLASLT_MATMUL_DESC_BIAS_DATA_TYPE, by default that of D
   Each element is alpha * sum + beta * c + bias, evaluated in float; the sum over k runs in four
   interleaved partial sums (k mod 4) that are added as (s0 + s1) + (s2 + s3), always in the same order.
   A 16-bit result is rounded to nearest even once, from the float value. */
#ifndef CUBLASLT_H_
#define CUBLASLT_H_
#include <stddef.h>
#include <cublas_api.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cublasLtContext *cublasLtHandle_t;

typedef struct {
    unsigned long long data[8];
} cublasLtMatrixLayoutOpaque_t;
typedef cublasLtMatrixLayoutOpaque_t *cublasLtMatrixLayout_t;

typedef struct {
    unsigned long long data[8];
} cublasLtMatmulAlgo_t;

typedef struct {
    unsigned long long data[32];
} cublasLtMatmulDescOpaque_t;
typedef cublasLtMatmulDescOpaque_t *cublasLtMatmulDesc_t;

typedef struct {
    unsigned long long data[8];
} cublasLtMatmulPreferenceOpaque_t;
typedef cublasLtMatmulPreferenceOpaque_t *cublasLtMatmulPreference_t;

typedef enum {
    CUBLASLT_MATMUL_TILE_UNDEFINED = 0,
    CUBLASLT_MATMUL_TILE_8x8 = 1
} cublasLtMatmulTile_t;

typedef enum {
    CUBLASLT_ORDER_COL = 0, /* element (i, j) at j * ld + i */
    CUBLASLT_ORDER_ROW = 1  /* not supported */
} cublasLtOrder_t;

typedef enum {
    CUBLASLT_MATRIX_LAYOUT_TYPE = 0,                 /* cudaDataType_t (uint32_t) */
    CUBLASLT_MATRIX_LAYOUT_ORDER = 1,                /* cublasLtOrder_t (int32_t) */
    CUBLASLT_MATRIX_LAYOUT_ROWS = 2,                 /* uint64_t */
    CUBLASLT_MATRIX_LAYOUT_COLS = 3,                 /* uint64_t */
    CUBLASLT_MATRIX_LAYOUT_LD = 4,                   /* int64_t */
    CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT = 5,          /* int32_t, default 1 */
    CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET = 6, /* int64_t: elements from one matrix of the batch to the next, default 0 */
    CUBLASLT_MATRIX_LAYOUT_PLANE_OFFSET = 7          /* int64_t (complex planar layouts: not supported) */
} cublasLtMatrixLayoutAttribute_t;

typedef enum {
    CUBLASLT_POINTER_MODE_HOST = CUBLAS_POINTER_MODE_HOST,
    CUBLASLT_POINTER_MODE_DEVICE = CUBLAS_POINTER_MODE_DEVICE,
    CUBLASLT_POINTER_MODE_DEVICE_VECTOR = 2,
    CUBLASLT_POINTER_MODE_ALPHA_DEVICE_VECTOR_BETA_ZERO = 3,
    CUBLASLT_POINTER_MODE_ALPHA_DEVICE_VECTOR_BETA_HOST = 4
} cublasLtPointerMode_t;

typedef enum {
    CUBLASLT_MATMUL_DESC_COMPUTE_TYPE = 0,  /* cublasComputeType_t (int32_t) */
    CUBLASLT_MATMUL_DESC_SCALE_TYPE = 1,    /* cudaDataType_t (int32_t) */
    CUBLASLT_MATMUL_DESC_POINTER_MODE = 2,  /* cublasLtPointerMode_t (int32_t) */
    CUBLASLT_MATMUL_DESC_TRANSA = 3,        /* cublasOperation_t (int32_t) */
    CUBLASLT_MATMUL_DESC_TRANSB = 4,
    CUBLASLT_MATMUL_DESC_TRANSC = 5,
    CUBLASLT_MATMUL_DESC_FILL_MODE = 6,     /* cublasFillMode_t (int32_t) */
    CUBLASLT_MATMUL_DESC_EPILOGUE = 7,      /* cublasLtEpilogue_t (uint32_t) */
    CUBLASLT_MATMUL_DESC_BIAS_POINTER = 8,  /* const void * */
    CUBLASLT_MATMUL_DESC_BIAS_BATCH_STRIDE = 10, /* int64_t */
    CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_POINTER = 11,
    CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_LD = 12,
    CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_BATCH_STRIDE = 13,
    CUBLASLT_MATMUL_DESC_ALPHA_VECTOR_BATCH_STRIDE = 14,
    CUBLASLT_MATMUL_DESC_SM_COUNT_TARGET = 15, /* int32_t */
    CUBLASLT_MATMUL_DESC_A_SCALE_POINTER = 17,
    CUBLASLT_MATMUL_DESC_B_SCALE_POINTER = 18,
    CUBLASLT_MATMUL_DESC_C_SCALE_POINTER = 19,
    CUBLASLT_MATMUL_DESC_D_SCALE_POINTER = 20,
    CUBLASLT_MATMUL_DESC_AMAX_D_POINTER = 21,
    CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_DATA_TYPE = 22,
    CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_SCALE_POINTER = 23,
    CUBLASLT_MATMUL_DESC_EPILOGUE_AUX_AMAX_POINTER = 24,
    CUBLASLT_MATMUL_DESC_FAST_ACCUM = 25,   /* int8_t */
    CUBLASLT_MATMUL_DESC_BIAS_DATA_TYPE = 26 /* cudaDataType_t (int32_t); -1, the default: the type of D */
} cublasLtMatmulDescAttributes_t;

typedef enum {
    CUBLASLT_EPILOGUE_DEFAULT = 1,
    CUBLASLT_EPILOGUE_RELU = 2,
    CUBLASLT_EPILOGUE_RELU_AUX = (CUBLASLT_EPILOGUE_RELU | 128),
    CUBLASLT_EPILOGUE_BIAS = 4,
    CUBLASLT_EPILOGUE_RELU_BIAS = (CUBLASLT_EPILOGUE_RELU | CUBLASLT_EPILOGUE_BIAS),
    CUBLASLT_EPILOGUE_RELU_AUX_BIAS = (CUBLASLT_EPILOGUE_RELU_AUX | CUBLASLT_EPILOGUE_BIAS),
    CUBLASLT_EPILOGUE_DRELU = 8 | 128,
    CUBLASLT_EPILOGUE_DRELU_BGRAD = CUBLASLT_EPILOGUE_DRELU | 16,
    CUBLASLT_EPILOGUE_GELU = 32,
    CUBLASLT_EPILOGUE_GELU_AUX = (CUBLASLT_EPILOGUE_GELU | 128),
    CUBLASLT_EPILOGUE_GELU_BIAS = (CUBLASLT_EPILOGUE_GELU | CUBLASLT_EPILOGUE_BIAS),
    CUBLASLT_EPILOGUE_GELU_AUX_BIAS = (CUBLASLT_EPILOGUE_GELU_AUX | CUBLASLT_EPILOGUE_BIAS),
    CUBLASLT_EPILOGUE_DGELU = 64 | 128,
    CUBLASLT_EPILOGUE_DGELU_BGRAD = CUBLASLT_EPILOGUE_DGELU | 16,
    CUBLASLT_EPILOGUE_BGRADA = 256,
    CUBLASLT_EPILOGUE_BGRADB = 512
} cublasLtEpilogue_t;

typedef enum {
    CUBLASLT_SEARCH_BEST_FIT = 0,
    CUBLASLT_SEARCH_LIMITED_BY_ALGO_ID = 1
} cublasLtMatmulSearch_t;

typedef enum {
    CUBLASLT_MATMUL_PREF_SEARCH_MODE = 0,            /* uint32_t */
    CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES = 1,    /* uint64_t */
    CUBLASLT_MATMUL_PREF_REDUCTION_SCHEME_MASK = 3,  /* uint32_t */
    CUBLASLT_MATMUL_PREF_MIN_ALIGNMENT_A_BYTES = 5,  /* uint32_t */
    CUBLASLT_MATMUL_PREF_MIN_ALIGNMENT_B_BYTES = 6,
    CUBLASLT_MATMUL_PREF_MIN_ALIGNMENT_C_BYTES = 7,
    CUBLASLT_MATMUL_PREF_MIN_ALIGNMENT_D_BYTES = 8,
    CUBLASLT_MATMUL_PREF_MAX_WAVES_COUNT = 9,        /* float */
    CUBLASLT_MATMUL_PREF_IMPL_MASK = 12              /* uint64_t */
} cublasLtMatmulPreferenceAttributes_t;

typedef struct {
    cublasLtMatmulAlgo_t algo;
    size_t workspaceSize;  /* 0: the CPU implementation takes what it needs from the host heap */
    cublasStatus_t state;
    float wavesCount;
    int reserved[4];
} cublasLtMatmulHeuristicResult_t;

cublasStatus_t cublasLtCreate(cublasLtHandle_t *lightHandle);
cublasStatus_t cublasLtDestroy(cublasLtHandle_t lightHandle);
const char *cublasLtGetStatusName(cublasStatus_t status);
const char *cublasLtGetStatusString(cublasStatus_t status);
size_t cublasLtGetVersion(void);
size_t cublasLtGetCudartVersion(void);
cublasStatus_t cublasLtGetProperty(libraryPropertyType type, int *value);

cublasStatus_t cublasLtMatmul(cublasLtHandle_t lightHandle, cublasLtMatmulDesc_t computeDesc, const void *alpha,
                              const void *A, cublasLtMatrixLayout_t Adesc, const void *B,
                              cublasLtMatrixLayout_t Bdesc, const void *beta, const void *C,
                              cublasLtMatrixLayout_t Cdesc, void *D, cublasLtMatrixLayout_t Ddesc,
                              const cublasLtMatmulAlgo_t *algo, void *workspace, size_t workspaceSizeInBytes,
                              cudaStream_t stream);

/* The Init functions fill a descriptor that the caller allocated; Create allocates one and Destroy frees it. */
cublasStatus_t cublasLtMatrixLayoutInit_internal(cublasLtMatrixLayout_t matLayout, size_t size, cudaDataType type,
                                                 unsigned long long rows, unsigned long long cols, long long ld);
#define cublasLtMatrixLayoutInit(matLayout, type, rows, cols, ld) \
    cublasLtMatrixLayoutInit_internal(matLayout, sizeof(*(matLayout)), type, rows, cols, ld)
cublasStatus_t cublasLtMatrixLayoutCreate(cublasLtMatrixLayout_t *matLayout, cudaDataType type,
                                          unsigned long long rows, unsigned long long cols, long long ld);
cublasStatus_t cublasLtMatrixLayoutDestroy(cublasLtMatrixLayout_t matLayout);
cublasStatus_t cublasLtMatrixLayoutSetAttribute(cublasLtMatrixLayout_t matLayout, cublasLtMatrixLayoutAttribute_t attr,
                                                const void *buf, size_t sizeInBytes);
cublasStatus_t cublasLtMatrixLayoutGetAttribute(cublasLtMatrixLayout_t matLayout, cublasLtMatrixLayoutAttribute_t attr,
                                                void *buf, size_t sizeInBytes, size_t *sizeWritten);

cublasStatus_t cublasLtMatmulDescInit_internal(cublasLtMatmulDesc_t matmulDesc, size_t size,
                                               cublasComputeType_t computeType, cudaDataType_t scaleType);
#define cublasLtMatmulDescInit(matmulDesc, computeType, scaleType) \
    cublasLtMatmulDescInit_internal(matmulDesc, sizeof(*(matmulDesc)), computeType, scaleType)
cublasStatus_t cublasLtMatmulDescCreate(cublasLtMatmulDesc_t *matmulDesc, cublasComputeType_t computeType,
                                        cudaDataType_t scaleType);
cublasStatus_t cublasLtMatmulDescDestroy(cublasLtMatmulDesc_t matmulDesc);
cublasStatus_t cublasLtMatmulDescSetAttribute(cublasLtMatmulDesc_t matmulDesc, cublasLtMatmulDescAttributes_t attr,
                                              const void *buf, size_t sizeInBytes);
cublasStatus_t cublasLtMatmulDescGetAttribute(cublasLtMatmulDesc_t matmulDesc, cublasLtMatmulDescAttributes_t attr,
                                              void *buf, size_t sizeInBytes, size_t *sizeWritten);

cublasStatus_t cublasLtMatmulPreferenceInit_internal(cublasLtMatmulPreference_t pref, size_t size);
#define cublasLtMatmulPreferenceInit(pref) cublasLtMatmulPreferenceInit_internal(pref, sizeof(*(pref)))
cublasStatus_t cublasLtMatmulPreferenceCreate(cublasLtMatmulPreference_t *pref);
cublasStatus_t cublasLtMatmulPreferenceDestroy(cublasLtMatmulPreference_t pref);
cublasStatus_t cublasLtMatmulPreferenceSetAttribute(cublasLtMatmulPreference_t pref,
                                                    cublasLtMatmulPreferenceAttributes_t attr, const void *buf,
                                                    size_t sizeInBytes);
cublasStatus_t cublasLtMatmulPreferenceGetAttribute(cublasLtMatmulPreference_t pref,
                                                    cublasLtMatmulPreferenceAttributes_t attr, void *buf,
                                                    size_t sizeInBytes, size_t *sizeWritten);

/* One algorithm (the CPU one) when the operation is supported: *returnAlgoCount is 1 and
   heuristicResultsArray[0].state is CUBLAS_STATUS_SUCCESS. Otherwise CUBLAS_STATUS_NOT_SUPPORTED (or
   CUBLAS_STATUS_INVALID_VALUE for shapes that do not fit) and *returnAlgoCount is 0. */
cublasStatus_t cublasLtMatmulAlgoGetHeuristic(cublasLtHandle_t lightHandle, cublasLtMatmulDesc_t operationDesc,
                                              cublasLtMatrixLayout_t Adesc, cublasLtMatrixLayout_t Bdesc,
                                              cublasLtMatrixLayout_t Cdesc, cublasLtMatrixLayout_t Ddesc,
                                              cublasLtMatmulPreference_t preference, int requestedAlgoCount,
                                              cublasLtMatmulHeuristicResult_t heuristicResultsArray[],
                                              int *returnAlgoCount);

#ifdef __cplusplus
}
#endif
#endif
