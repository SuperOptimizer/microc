/* mcudahw: cublasLt. Every function goes to NVIDIA's library of that name, opened at the first call
   (libcublasLt.so.13, or the one that MCUDA_LIBCUBLASLT names); the descriptors are that library's own, which
   the header's opaque types have the size of. The library works in the thread's current context, which is the
   run time's (the device's primary context), and a stream of the run time is the driver's stream. */
#ifdef MCUDA_LT_OWN
/* Built with MCUDA_LT_OWN: nothing of NVIDIA's but the driver. The descriptors and the checks are those of the
   CPU run time (cuda/cublaslt.c), and the product is the kernel of ltk.cu, launched on the caller's stream. */
#include "hw.h"
int __mcudahw_lt_gemm(const void *A, const void *B, const void *C, void *D, const void *bias, int m, int n, int k, long lda, long ldb,
                      long ldc, long ldd, long sa, long sb, long sc, long sd, int batch, int types, int flags, float alpha, float beta,
                      void *stream);
#define MCUDA_LT_HW
#include "../cublaslt.c"
#else
#include "hw.h"
#include <cublasLt.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

typedef cublasStatus_t St;
typedef unsigned long long U64;
#define LT(X) \
    X(Create, (cublasLtHandle_t *)) \
    X(Destroy, (cublasLtHandle_t)) \
    X(GetProperty, (libraryPropertyType, int *)) \
    X(Matmul, (cublasLtHandle_t, cublasLtMatmulDesc_t, const void *, const void *, cublasLtMatrixLayout_t, const void *, \
               cublasLtMatrixLayout_t, const void *, const void *, cublasLtMatrixLayout_t, void *, cublasLtMatrixLayout_t, \
               const cublasLtMatmulAlgo_t *, void *, size_t, cudaStream_t)) \
    X(MatrixLayoutInit_internal, (cublasLtMatrixLayout_t, size_t, cudaDataType, U64, U64, long long)) \
    X(MatrixLayoutCreate, (cublasLtMatrixLayout_t *, cudaDataType, U64, U64, long long)) \
    X(MatrixLayoutDestroy, (cublasLtMatrixLayout_t)) \
    X(MatrixLayoutSetAttribute, (cublasLtMatrixLayout_t, cublasLtMatrixLayoutAttribute_t, const void *, size_t)) \
    X(MatrixLayoutGetAttribute, (cublasLtMatrixLayout_t, cublasLtMatrixLayoutAttribute_t, void *, size_t, size_t *)) \
    X(MatmulDescInit_internal, (cublasLtMatmulDesc_t, size_t, cublasComputeType_t, cudaDataType_t)) \
    X(MatmulDescCreate, (cublasLtMatmulDesc_t *, cublasComputeType_t, cudaDataType_t)) \
    X(MatmulDescDestroy, (cublasLtMatmulDesc_t)) \
    X(MatmulDescSetAttribute, (cublasLtMatmulDesc_t, cublasLtMatmulDescAttributes_t, const void *, size_t)) \
    X(MatmulDescGetAttribute, (cublasLtMatmulDesc_t, cublasLtMatmulDescAttributes_t, void *, size_t, size_t *)) \
    X(MatmulPreferenceInit_internal, (cublasLtMatmulPreference_t, size_t)) \
    X(MatmulPreferenceCreate, (cublasLtMatmulPreference_t *)) \
    X(MatmulPreferenceDestroy, (cublasLtMatmulPreference_t)) \
    X(MatmulPreferenceSetAttribute, (cublasLtMatmulPreference_t, cublasLtMatmulPreferenceAttributes_t, const void *, size_t)) \
    X(MatmulPreferenceGetAttribute, (cublasLtMatmulPreference_t, cublasLtMatmulPreferenceAttributes_t, void *, size_t, size_t *)) \
    X(MatmulAlgoGetHeuristic, (cublasLtHandle_t, cublasLtMatmulDesc_t, cublasLtMatrixLayout_t, cublasLtMatrixLayout_t, \
                               cublasLtMatrixLayout_t, cublasLtMatrixLayout_t, cublasLtMatmulPreference_t, int, \
                               cublasLtMatmulHeuristicResult_t *, int *))

static struct {
#define X(n, args) St (*n) args;
    LT(X)
#undef X
    size_t (*GetVersion)(void);
    size_t (*GetCudartVersion)(void);
} lt;
static const char *const names[] = {
#define X(n, args) "cublasLt" #n,
    LT(X)
#undef X
    "cublasLtGetVersion", "cublasLtGetCudartVersion", 0};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int state; /* 0: not tried, 1: open, 2: there is none */

static void open_it(void)
{
    static const char *const tries[] = {"libcublasLt.so.13", "libcublasLt.so.12", "libcublasLt.so", "/usr/local/cuda/lib64/libcublasLt.so", 0};
    void *lib = 0, **slot = (void **)(void *)&lt;
    const char *path = getenv("MCUDA_LIBCUBLASLT");
    int i;
    if (path && path[0])
        lib = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    else
        for (i = 0; !lib && tries[i]; i++)
            lib = dlopen(tries[i], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        fprintf(stderr, "mcuda: cannot open NVIDIA's cublasLt (%s): set MCUDA_LIBCUBLASLT to its path\n", path && path[0] ? path : tries[0]);
        state = 2;
        return;
    }
    for (i = 0; names[i]; i++) {
        slot[i] = dlsym(lib, names[i]); /* (the members are all pointers to functions, in the order of the names) */
        if (!slot[i]) {
            fprintf(stderr, "mcuda: the cublasLt library has no %s\n", names[i]);
            state = 2;
            return;
        }
    }
    state = 1;
}

/* 0 when the library is open and the run time's context is this thread's */
static int ready(void)
{
    if (state != 1) {
        pthread_mutex_lock(&lock);
        if (!state)
            open_it();
        pthread_mutex_unlock(&lock);
        if (state != 1)
            return 1;
    }
    return hw_enter() != 0;
}
#define READY \
    if (ready()) \
        return CUBLAS_STATUS_NOT_INITIALIZED

const char *cublasGetStatusName(cublasStatus_t s)
{
    switch (s) {
    case CUBLAS_STATUS_SUCCESS: return "CUBLAS_STATUS_SUCCESS";
    case CUBLAS_STATUS_NOT_INITIALIZED: return "CUBLAS_STATUS_NOT_INITIALIZED";
    case CUBLAS_STATUS_ALLOC_FAILED: return "CUBLAS_STATUS_ALLOC_FAILED";
    case CUBLAS_STATUS_INVALID_VALUE: return "CUBLAS_STATUS_INVALID_VALUE";
    case CUBLAS_STATUS_ARCH_MISMATCH: return "CUBLAS_STATUS_ARCH_MISMATCH";
    case CUBLAS_STATUS_MAPPING_ERROR: return "CUBLAS_STATUS_MAPPING_ERROR";
    case CUBLAS_STATUS_EXECUTION_FAILED: return "CUBLAS_STATUS_EXECUTION_FAILED";
    case CUBLAS_STATUS_INTERNAL_ERROR: return "CUBLAS_STATUS_INTERNAL_ERROR";
    case CUBLAS_STATUS_NOT_SUPPORTED: return "CUBLAS_STATUS_NOT_SUPPORTED";
    case CUBLAS_STATUS_LICENSE_ERROR: return "CUBLAS_STATUS_LICENSE_ERROR";
    }
    return "unknown cublas status";
}

const char *cublasGetStatusString(cublasStatus_t s)
{
    switch (s) {
    case CUBLAS_STATUS_SUCCESS: return "success";
    case CUBLAS_STATUS_NOT_INITIALIZED: return "the library was not initialized";
    case CUBLAS_STATUS_ALLOC_FAILED: return "resource allocation failed";
    case CUBLAS_STATUS_INVALID_VALUE: return "an unsupported value or parameter was passed to the function";
    case CUBLAS_STATUS_ARCH_MISMATCH: return "the function requires a feature absent from the device architecture";
    case CUBLAS_STATUS_MAPPING_ERROR: return "an access to GPU memory space failed";
    case CUBLAS_STATUS_EXECUTION_FAILED: return "the GPU program failed to execute";
    case CUBLAS_STATUS_INTERNAL_ERROR: return "an internal operation failed";
    case CUBLAS_STATUS_NOT_SUPPORTED: return "the functionality requested is not supported";
    case CUBLAS_STATUS_LICENSE_ERROR: return "the functionality requested requires some license";
    }
    return "unknown cublas status";
}


const char *cublasLtGetStatusName(cublasStatus_t s) { return cublasGetStatusName(s); }
const char *cublasLtGetStatusString(cublasStatus_t s) { return cublasGetStatusString(s); }
size_t cublasLtGetVersion(void) { return ready() ? 0 : lt.GetVersion(); }
size_t cublasLtGetCudartVersion(void) { return ready() ? 0 : lt.GetCudartVersion(); }

St cublasLtCreate(cublasLtHandle_t *h)
{
    READY;
    return lt.Create(h);
}
St cublasLtDestroy(cublasLtHandle_t h)
{
    READY;
    return lt.Destroy(h);
}
St cublasLtGetProperty(libraryPropertyType type, int *value)
{
    READY;
    return lt.GetProperty(type, value);
}
St cublasLtMatmul(cublasLtHandle_t h, cublasLtMatmulDesc_t desc, const void *alpha, const void *A, cublasLtMatrixLayout_t Ad,
                  const void *B, cublasLtMatrixLayout_t Bd, const void *beta, const void *C, cublasLtMatrixLayout_t Cd, void *D,
                  cublasLtMatrixLayout_t Dd, const cublasLtMatmulAlgo_t *algo, void *work, size_t worksize, cudaStream_t stream)
{
    READY;
    return lt.Matmul(h, desc, alpha, A, Ad, B, Bd, beta, C, Cd, D, Dd, algo, work, worksize, stream);
}
St cublasLtMatrixLayoutInit_internal(cublasLtMatrixLayout_t m, size_t size, cudaDataType type, U64 rows, U64 cols, long long ld)
{
    READY;
    return lt.MatrixLayoutInit_internal(m, size, type, rows, cols, ld);
}
St cublasLtMatrixLayoutCreate(cublasLtMatrixLayout_t *m, cudaDataType type, U64 rows, U64 cols, long long ld)
{
    READY;
    return lt.MatrixLayoutCreate(m, type, rows, cols, ld);
}
St cublasLtMatrixLayoutDestroy(cublasLtMatrixLayout_t m)
{
    READY;
    return lt.MatrixLayoutDestroy(m);
}
St cublasLtMatrixLayoutSetAttribute(cublasLtMatrixLayout_t m, cublasLtMatrixLayoutAttribute_t attr, const void *buf, size_t n)
{
    READY;
    return lt.MatrixLayoutSetAttribute(m, attr, buf, n);
}
St cublasLtMatrixLayoutGetAttribute(cublasLtMatrixLayout_t m, cublasLtMatrixLayoutAttribute_t attr, void *buf, size_t n, size_t *written)
{
    READY;
    return lt.MatrixLayoutGetAttribute(m, attr, buf, n, written);
}
St cublasLtMatmulDescInit_internal(cublasLtMatmulDesc_t d, size_t size, cublasComputeType_t compute, cudaDataType_t scale)
{
    READY;
    return lt.MatmulDescInit_internal(d, size, compute, scale);
}
St cublasLtMatmulDescCreate(cublasLtMatmulDesc_t *d, cublasComputeType_t compute, cudaDataType_t scale)
{
    READY;
    return lt.MatmulDescCreate(d, compute, scale);
}
St cublasLtMatmulDescDestroy(cublasLtMatmulDesc_t d)
{
    READY;
    return lt.MatmulDescDestroy(d);
}
St cublasLtMatmulDescSetAttribute(cublasLtMatmulDesc_t d, cublasLtMatmulDescAttributes_t attr, const void *buf, size_t n)
{
    READY;
    return lt.MatmulDescSetAttribute(d, attr, buf, n);
}
St cublasLtMatmulDescGetAttribute(cublasLtMatmulDesc_t d, cublasLtMatmulDescAttributes_t attr, void *buf, size_t n, size_t *written)
{
    READY;
    return lt.MatmulDescGetAttribute(d, attr, buf, n, written);
}
St cublasLtMatmulPreferenceInit_internal(cublasLtMatmulPreference_t p, size_t size)
{
    READY;
    return lt.MatmulPreferenceInit_internal(p, size);
}
St cublasLtMatmulPreferenceCreate(cublasLtMatmulPreference_t *p)
{
    READY;
    return lt.MatmulPreferenceCreate(p);
}
St cublasLtMatmulPreferenceDestroy(cublasLtMatmulPreference_t p)
{
    READY;
    return lt.MatmulPreferenceDestroy(p);
}
St cublasLtMatmulPreferenceSetAttribute(cublasLtMatmulPreference_t p, cublasLtMatmulPreferenceAttributes_t attr, const void *buf, size_t n)
{
    READY;
    return lt.MatmulPreferenceSetAttribute(p, attr, buf, n);
}
St cublasLtMatmulPreferenceGetAttribute(cublasLtMatmulPreference_t p, cublasLtMatmulPreferenceAttributes_t attr, void *buf, size_t n,
                                        size_t *written)
{
    READY;
    return lt.MatmulPreferenceGetAttribute(p, attr, buf, n, written);
}
St cublasLtMatmulAlgoGetHeuristic(cublasLtHandle_t h, cublasLtMatmulDesc_t desc, cublasLtMatrixLayout_t Ad, cublasLtMatrixLayout_t Bd,
                                  cublasLtMatrixLayout_t Cd, cublasLtMatrixLayout_t Dd, cublasLtMatmulPreference_t pref, int want,
                                  cublasLtMatmulHeuristicResult_t *results, int *got)
{
    READY;
    return lt.MatmulAlgoGetHeuristic(h, desc, Ad, Bd, Cd, Dd, pref, want, results, got);
}
#endif
