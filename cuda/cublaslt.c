/* cublasLt on the CPU: descriptors and cublasLtMatmul for real 32-bit, half and bfloat16 matrices with
   single-precision accumulation. cublasLt.h says exactly what is supported and how results are rounded. */
#define MCUDA_INTERNAL
#include "mc.h"
#include <mcuda.h>
#include <cublasLt.h>
#include <stdlib.h>
#include <string.h>

#define MAGIC_LT 0x4c544831u /* "LTH1" */

struct cublasLtContext {
    u32 magic;
};

/* What the opaque descriptors hold. Each fits the public opaque type, so the Init functions can fill
   storage that the caller owns. */
typedef struct {
    u32 type;   /* cudaDataType_t */
    s32 order;  /* cublasLtOrder_t */
    u64 rows, cols;
    s64 ld;
    s32 batch;
    s32 pad;
    s64 stride; /* elements between the matrices of a batch */
    s64 plane;
} Layout;

typedef struct {
    s32 compute, scale, pmode, transa, transb, transc, fill;
    u32 epilogue;
    const void *bias;
    s64 bias_stride;
    s32 bias_type; /* -1: that of D */
    s32 sm_count;
    s8 fast_accum;
} Desc;

typedef struct {
    u32 search;
    u32 reduction;
    u64 max_ws;
    u32 align[4];
    float waves;
    u32 pad;
    u64 impl;
} Pref;

typedef struct {
    int attr;
    u32 off, size;
} Field;

#define F(attr, type, member) {attr, (u32)offsetof(type, member), (u32)sizeof(((type *)0)->member)}

static const Field layout_fields[] = {
    F(CUBLASLT_MATRIX_LAYOUT_TYPE, Layout, type),
    F(CUBLASLT_MATRIX_LAYOUT_ORDER, Layout, order),
    F(CUBLASLT_MATRIX_LAYOUT_ROWS, Layout, rows),
    F(CUBLASLT_MATRIX_LAYOUT_COLS, Layout, cols),
    F(CUBLASLT_MATRIX_LAYOUT_LD, Layout, ld),
    F(CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT, Layout, batch),
    F(CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET, Layout, stride),
    F(CUBLASLT_MATRIX_LAYOUT_PLANE_OFFSET, Layout, plane),
    {-1, 0, 0}
};

/* The attributes that are not here (auxiliary outputs, FP8 scales, alpha vectors) are not supported. */
static const Field desc_fields[] = {
    F(CUBLASLT_MATMUL_DESC_COMPUTE_TYPE, Desc, compute),
    F(CUBLASLT_MATMUL_DESC_SCALE_TYPE, Desc, scale),
    F(CUBLASLT_MATMUL_DESC_POINTER_MODE, Desc, pmode),
    F(CUBLASLT_MATMUL_DESC_TRANSA, Desc, transa),
    F(CUBLASLT_MATMUL_DESC_TRANSB, Desc, transb),
    F(CUBLASLT_MATMUL_DESC_TRANSC, Desc, transc),
    F(CUBLASLT_MATMUL_DESC_FILL_MODE, Desc, fill),
    F(CUBLASLT_MATMUL_DESC_EPILOGUE, Desc, epilogue),
    F(CUBLASLT_MATMUL_DESC_BIAS_POINTER, Desc, bias),
    F(CUBLASLT_MATMUL_DESC_BIAS_BATCH_STRIDE, Desc, bias_stride),
    F(CUBLASLT_MATMUL_DESC_BIAS_DATA_TYPE, Desc, bias_type),
    F(CUBLASLT_MATMUL_DESC_SM_COUNT_TARGET, Desc, sm_count),
    F(CUBLASLT_MATMUL_DESC_FAST_ACCUM, Desc, fast_accum),
    {-1, 0, 0}
};

static const Field pref_fields[] = {
    F(CUBLASLT_MATMUL_PREF_SEARCH_MODE, Pref, search),
    F(CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES, Pref, max_ws),
    F(CUBLASLT_MATMUL_PREF_REDUCTION_SCHEME_MASK, Pref, reduction),
    F(CUBLASLT_MATMUL_PREF_MIN_ALIGNMENT_A_BYTES, Pref, align[0]),
    F(CUBLASLT_MATMUL_PREF_MIN_ALIGNMENT_B_BYTES, Pref, align[1]),
    F(CUBLASLT_MATMUL_PREF_MIN_ALIGNMENT_C_BYTES, Pref, align[2]),
    F(CUBLASLT_MATMUL_PREF_MIN_ALIGNMENT_D_BYTES, Pref, align[3]),
    F(CUBLASLT_MATMUL_PREF_MAX_WAVES_COUNT, Pref, waves),
    F(CUBLASLT_MATMUL_PREF_IMPL_MASK, Pref, impl),
    {-1, 0, 0}
};

static cublasStatus_t attr_set(void *obj, const Field *f, int attr, const void *buf, size_t size)
{
    if (!obj || !buf)
        return CUBLAS_STATUS_INVALID_VALUE;
    for (; f->attr >= 0; f++)
        if (f->attr == attr) {
            if (size != f->size)
                return CUBLAS_STATUS_INVALID_VALUE;
            memcpy((u8 *)obj + f->off, buf, size);
            return CUBLAS_STATUS_SUCCESS;
        }
    return CUBLAS_STATUS_NOT_SUPPORTED;
}

/* With size 0 only the size of the attribute is reported. */
static cublasStatus_t attr_get(void *obj, const Field *f, int attr, void *buf, size_t size, size_t *written)
{
    if (!obj || (!size && !written) || (size && !buf))
        return CUBLAS_STATUS_INVALID_VALUE;
    for (; f->attr >= 0; f++)
        if (f->attr == attr) {
            if (size && size != f->size)
                return CUBLAS_STATUS_INVALID_VALUE;
            if (size)
                memcpy(buf, (u8 *)obj + f->off, size);
            if (written)
                *written = f->size;
            return CUBLAS_STATUS_SUCCESS;
        }
    return CUBLAS_STATUS_NOT_SUPPORTED;
}

/* ---- handle ---- */

cublasStatus_t cublasLtCreate(cublasLtHandle_t *ph)
{
    cublasLtHandle_t h;
    if (!ph)
        return CUBLAS_STATUS_INVALID_VALUE;
    *ph = 0;
    if (!mcuda_dev_count())
        return CUBLAS_STATUS_NOT_INITIALIZED;
    h = (cublasLtHandle_t)malloc(sizeof *h);
    if (!h)
        return CUBLAS_STATUS_ALLOC_FAILED;
    h->magic = MAGIC_LT;
    *ph = h;
    return CUBLAS_STATUS_SUCCESS;
}

cublasStatus_t cublasLtDestroy(cublasLtHandle_t h)
{
    if (!h || h->magic != MAGIC_LT)
        return CUBLAS_STATUS_NOT_INITIALIZED;
    h->magic = 0;
    free(h);
    return CUBLAS_STATUS_SUCCESS;
}

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
size_t cublasLtGetVersion(void) { return CUBLAS_VERSION; }
size_t cublasLtGetCudartVersion(void) { return CUDART_VERSION; }

cublasStatus_t cublasLtGetProperty(libraryPropertyType type, int *value)
{
    if (!value)
        return CUBLAS_STATUS_INVALID_VALUE;
    switch (type) {
    case MAJOR_VERSION: *value = CUBLAS_VER_MAJOR; break;
    case MINOR_VERSION: *value = CUBLAS_VER_MINOR; break;
    case PATCH_LEVEL: *value = CUBLAS_VER_PATCH; break;
    default: return CUBLAS_STATUS_INVALID_VALUE;
    }
    return CUBLAS_STATUS_SUCCESS;
}

/* ---- descriptors ---- */

cublasStatus_t cublasLtMatrixLayoutInit_internal(cublasLtMatrixLayout_t l, size_t size, cudaDataType type,
                                                 unsigned long long rows, unsigned long long cols, long long ld)
{
    Layout *p = (Layout *)l;
    if (!l || size < sizeof *p)
        return CUBLAS_STATUS_INVALID_VALUE;
    memset(l, 0, size);
    p->type = (u32)type;
    p->order = CUBLASLT_ORDER_COL;
    p->rows = rows;
    p->cols = cols;
    p->ld = ld;
    p->batch = 1;
    return CUBLAS_STATUS_SUCCESS;
}

cublasStatus_t cublasLtMatrixLayoutCreate(cublasLtMatrixLayout_t *pl, cudaDataType type, unsigned long long rows,
                                          unsigned long long cols, long long ld)
{
    if (!pl)
        return CUBLAS_STATUS_INVALID_VALUE;
    *pl = (cublasLtMatrixLayout_t)malloc(sizeof **pl);
    if (!*pl)
        return CUBLAS_STATUS_ALLOC_FAILED;
    return cublasLtMatrixLayoutInit_internal(*pl, sizeof **pl, type, rows, cols, ld);
}

cublasStatus_t cublasLtMatrixLayoutDestroy(cublasLtMatrixLayout_t l)
{
    free(l);
    return CUBLAS_STATUS_SUCCESS;
}

cublasStatus_t cublasLtMatrixLayoutSetAttribute(cublasLtMatrixLayout_t l, cublasLtMatrixLayoutAttribute_t a, const void *buf, size_t size)
{
    return attr_set(l, layout_fields, (int)a, buf, size);
}

cublasStatus_t cublasLtMatrixLayoutGetAttribute(cublasLtMatrixLayout_t l, cublasLtMatrixLayoutAttribute_t a, void *buf, size_t size, size_t *written)
{
    return attr_get(l, layout_fields, (int)a, buf, size, written);
}

cublasStatus_t cublasLtMatmulDescInit_internal(cublasLtMatmulDesc_t d, size_t size, cublasComputeType_t compute, cudaDataType_t scale)
{
    Desc *p = (Desc *)d;
    if (!d || size < sizeof *p)
        return CUBLAS_STATUS_INVALID_VALUE;
    memset(d, 0, size);
    p->compute = (s32)compute;
    p->scale = (s32)scale;
    p->pmode = CUBLASLT_POINTER_MODE_HOST;
    p->transa = CUBLAS_OP_N;
    p->transb = CUBLAS_OP_N;
    p->transc = CUBLAS_OP_N;
    p->fill = CUBLAS_FILL_MODE_FULL;
    p->epilogue = CUBLASLT_EPILOGUE_DEFAULT;
    p->bias_type = -1;
    return CUBLAS_STATUS_SUCCESS;
}

cublasStatus_t cublasLtMatmulDescCreate(cublasLtMatmulDesc_t *pd, cublasComputeType_t compute, cudaDataType_t scale)
{
    if (!pd)
        return CUBLAS_STATUS_INVALID_VALUE;
    *pd = (cublasLtMatmulDesc_t)malloc(sizeof **pd);
    if (!*pd)
        return CUBLAS_STATUS_ALLOC_FAILED;
    return cublasLtMatmulDescInit_internal(*pd, sizeof **pd, compute, scale);
}

cublasStatus_t cublasLtMatmulDescDestroy(cublasLtMatmulDesc_t d)
{
    free(d);
    return CUBLAS_STATUS_SUCCESS;
}

cublasStatus_t cublasLtMatmulDescSetAttribute(cublasLtMatmulDesc_t d, cublasLtMatmulDescAttributes_t a, const void *buf, size_t size)
{
    return attr_set(d, desc_fields, (int)a, buf, size);
}

cublasStatus_t cublasLtMatmulDescGetAttribute(cublasLtMatmulDesc_t d, cublasLtMatmulDescAttributes_t a, void *buf, size_t size, size_t *written)
{
    return attr_get(d, desc_fields, (int)a, buf, size, written);
}

cublasStatus_t cublasLtMatmulPreferenceInit_internal(cublasLtMatmulPreference_t pref, size_t size)
{
    Pref *p = (Pref *)pref;
    int i;
    if (!pref || size < sizeof *p)
        return CUBLAS_STATUS_INVALID_VALUE;
    memset(pref, 0, size);
    p->search = CUBLASLT_SEARCH_BEST_FIT;
    for (i = 0; i < 4; i++)
        p->align[i] = 256;
    return CUBLAS_STATUS_SUCCESS;
}

cublasStatus_t cublasLtMatmulPreferenceCreate(cublasLtMatmulPreference_t *pp)
{
    if (!pp)
        return CUBLAS_STATUS_INVALID_VALUE;
    *pp = (cublasLtMatmulPreference_t)malloc(sizeof **pp);
    if (!*pp)
        return CUBLAS_STATUS_ALLOC_FAILED;
    return cublasLtMatmulPreferenceInit_internal(*pp, sizeof **pp);
}

cublasStatus_t cublasLtMatmulPreferenceDestroy(cublasLtMatmulPreference_t pref)
{
    free(pref);
    return CUBLAS_STATUS_SUCCESS;
}

cublasStatus_t cublasLtMatmulPreferenceSetAttribute(cublasLtMatmulPreference_t pref, cublasLtMatmulPreferenceAttributes_t a, const void *buf, size_t size)
{
    return attr_set(pref, pref_fields, (int)a, buf, size);
}

cublasStatus_t cublasLtMatmulPreferenceGetAttribute(cublasLtMatmulPreference_t pref, cublasLtMatmulPreferenceAttributes_t a, void *buf, size_t size, size_t *written)
{
    return attr_get(pref, pref_fields, (int)a, buf, size, written);
}

/* ---- matmul ---- */

typedef union {
    float f;
    u32 u;
} Bits;

static int type_ok(u32 t) { return t == CUDA_R_32F || t == CUDA_R_16F || t == CUDA_R_16BF; }
static u64 type_size(u32 t) { return t == CUDA_R_32F ? 4 : 2; }

/* n elements of `type` from src, every `step`-th one, to floats. */
static void to_float(float *dst, const void *src, u32 type, u64 n, u64 step)
{
    u64 i;
    Bits b;
    if (type == CUDA_R_32F) {
        const float *s = (const float *)src;
        for (i = 0; i < n; i++)
            dst[i] = s[i * step];
    } else if (type == CUDA_R_16F) {
        const u16 *s = (const u16 *)src;
        for (i = 0; i < n; i++) {
            b.u = mc_f16_to_f32(s[i * step]);
            dst[i] = b.f;
        }
    } else {
        const u16 *s = (const u16 *)src;
        for (i = 0; i < n; i++) {
            b.u = mc_bf16_to_f32(s[i * step]);
            dst[i] = b.f;
        }
    }
}

/* The sum of a[l] * b[l] in four partial sums by l mod 4, added as (s0 + s1) + (s2 + s3). */
static float dot(const float *a, const float *b, u64 k)
{
    float s0 = 0, s1 = 0, s2 = 0, s3 = 0;
    u64 l = 0;
    for (; l + 4 <= k; l += 4) {
        s0 += a[l] * b[l];
        s1 += a[l + 1] * b[l + 1];
        s2 += a[l + 2] * b[l + 2];
        s3 += a[l + 3] * b[l + 3];
    }
    if (l < k)
        s0 += a[l] * b[l];
    if (l + 1 < k)
        s1 += a[l + 1] * b[l + 1];
    if (l + 2 < k)
        s2 += a[l + 2] * b[l + 2];
    return (s0 + s1) + (s2 + s3);
}

typedef struct {
    u64 m, n, k;
    int ta, tb;
    s32 batch;
    u32 bias_type; /* meaningful with a BIAS epilogue */
} Shape;

/* Is this a product that cublasLtMatmul computes? Fills *sh. */
static cublasStatus_t check(const Desc *d, const Layout *A, const Layout *B, const Layout *C, const Layout *D, Shape *sh)
{
    u64 kb;
    if (!d || !A || !B || !C || !D)
        return CUBLAS_STATUS_INVALID_VALUE;
    if (!type_ok(A->type) || !type_ok(B->type) || !type_ok(C->type) || !type_ok(D->type) || C->type != D->type)
        return CUBLAS_STATUS_NOT_SUPPORTED;
    if (A->order != CUBLASLT_ORDER_COL || B->order != CUBLASLT_ORDER_COL || C->order != CUBLASLT_ORDER_COL || D->order != CUBLASLT_ORDER_COL)
        return CUBLAS_STATUS_NOT_SUPPORTED;
    if (A->plane || B->plane || C->plane || D->plane)
        return CUBLAS_STATUS_NOT_SUPPORTED;
    if (d->compute != CUBLAS_COMPUTE_32F && d->compute != CUBLAS_COMPUTE_32F_PEDANTIC && d->compute != CUBLAS_COMPUTE_32F_FAST_16F &&
        d->compute != CUBLAS_COMPUTE_32F_FAST_16BF && d->compute != CUBLAS_COMPUTE_32F_FAST_TF32)
        return CUBLAS_STATUS_NOT_SUPPORTED;
    if (d->scale != CUDA_R_32F || d->pmode != CUBLASLT_POINTER_MODE_HOST)
        return CUBLAS_STATUS_NOT_SUPPORTED;
    if (d->transa < CUBLAS_OP_N || d->transa > CUBLAS_OP_C || d->transb < CUBLAS_OP_N || d->transb > CUBLAS_OP_C)
        return CUBLAS_STATUS_INVALID_VALUE;
    if (d->transc != CUBLAS_OP_N || d->fill != CUBLAS_FILL_MODE_FULL)
        return CUBLAS_STATUS_NOT_SUPPORTED;
    if (d->epilogue != CUBLASLT_EPILOGUE_DEFAULT && d->epilogue != CUBLASLT_EPILOGUE_RELU && d->epilogue != CUBLASLT_EPILOGUE_BIAS &&
        d->epilogue != CUBLASLT_EPILOGUE_RELU_BIAS)
        return CUBLAS_STATUS_NOT_SUPPORTED;
    sh->bias_type = d->bias_type < 0 ? D->type : (u32)d->bias_type;
    if ((d->epilogue & CUBLASLT_EPILOGUE_BIAS) && (!type_ok(sh->bias_type) || d->bias_stride))
        return CUBLAS_STATUS_NOT_SUPPORTED;
    sh->ta = d->transa != CUBLAS_OP_N;
    sh->tb = d->transb != CUBLAS_OP_N;
    sh->m = sh->ta ? A->cols : A->rows;
    sh->k = sh->ta ? A->rows : A->cols;
    kb = sh->tb ? B->cols : B->rows;
    sh->n = sh->tb ? B->rows : B->cols;
    if (!sh->m || !sh->n || !sh->k || kb != sh->k || D->rows != sh->m || D->cols != sh->n || C->rows != sh->m || C->cols != sh->n)
        return CUBLAS_STATUS_INVALID_VALUE;
    if (A->ld < (s64)A->rows || B->ld < (s64)B->rows || C->ld < (s64)C->rows || D->ld < (s64)D->rows)
        return CUBLAS_STATUS_INVALID_VALUE;
    if (sh->m > ((u64)1 << 31) || sh->n > ((u64)1 << 31) || sh->k > ((u64)1 << 31))
        return CUBLAS_STATUS_INVALID_VALUE;
    sh->batch = D->batch;
    if (sh->batch < 1 || A->batch != sh->batch || B->batch != sh->batch || C->batch != sh->batch)
        return CUBLAS_STATUS_INVALID_VALUE;
    if (sh->batch > 1 && (A->stride < 0 || B->stride < 0 || C->stride < 0 || D->stride < 0))
        return CUBLAS_STATUS_NOT_SUPPORTED;
    return CUBLAS_STATUS_SUCCESS;
}

cublasStatus_t cublasLtMatmulAlgoGetHeuristic(cublasLtHandle_t h, cublasLtMatmulDesc_t desc, cublasLtMatrixLayout_t A,
                                              cublasLtMatrixLayout_t B, cublasLtMatrixLayout_t C, cublasLtMatrixLayout_t D,
                                              cublasLtMatmulPreference_t pref, int requested,
                                              cublasLtMatmulHeuristicResult_t results[], int *returned)
{
    Shape sh;
    cublasStatus_t s;
    (void)pref;
    if (returned)
        *returned = 0;
    if (!h || h->magic != MAGIC_LT)
        return CUBLAS_STATUS_NOT_INITIALIZED;
    if (requested < 1 || !results || !returned)
        return CUBLAS_STATUS_INVALID_VALUE;
    s = check((const Desc *)desc, (const Layout *)A, (const Layout *)B, (const Layout *)C, (const Layout *)D, &sh);
    if (s)
        return s;
    memset(&results[0], 0, sizeof results[0]);
    results[0].algo.data[0] = MAGIC_LT; /* the one algorithm */
    results[0].workspaceSize = 0;
    results[0].state = CUBLAS_STATUS_SUCCESS;
    results[0].wavesCount = 1.0f;
    *returned = 1;
    return CUBLAS_STATUS_SUCCESS;
}

cublasStatus_t cublasLtMatmul(cublasLtHandle_t h, cublasLtMatmulDesc_t desc, const void *alpha_p, const void *A,
                              cublasLtMatrixLayout_t Adesc, const void *B, cublasLtMatrixLayout_t Bdesc, const void *beta_p,
                              const void *C, cublasLtMatrixLayout_t Cdesc, void *D, cublasLtMatrixLayout_t Ddesc,
                              const cublasLtMatmulAlgo_t *algo, void *workspace, size_t workspace_size, cudaStream_t stream)
{
    const Desc *d = (const Desc *)desc;
    const Layout *la = (const Layout *)Adesc, *lb = (const Layout *)Bdesc, *lc = (const Layout *)Cdesc, *ld = (const Layout *)Ddesc;
    Shape sh;
    cublasStatus_t s;
    float alpha, beta, *pa, *pb, *bias = 0, *col;
    u64 m, n, k, i, j, l;
    int relu, have_bias;
    s32 b;
    Bits bits;
    (void)algo; /* 0 is allowed: the heuristic's choice, and there is one algorithm */
    (void)workspace;
    (void)workspace_size;
    if (!h || h->magic != MAGIC_LT)
        return CUBLAS_STATUS_NOT_INITIALIZED;
    s = check(d, la, lb, lc, ld, &sh);
    if (s)
        return s;
    if (!alpha_p || !beta_p || !A || !B || !D || !mcuda_stream_ok(stream))
        return CUBLAS_STATUS_INVALID_VALUE;
    alpha = *(const float *)alpha_p;
    beta = *(const float *)beta_p;
    relu = (d->epilogue & CUBLASLT_EPILOGUE_RELU) != 0;
    have_bias = (d->epilogue & CUBLASLT_EPILOGUE_BIAS) != 0;
    if ((beta != 0 && !C) || (have_bias && !d->bias))
        return CUBLAS_STATUS_INVALID_VALUE;
    m = sh.m;
    n = sh.n;
    k = sh.k;
    /* op(A) by rows and op(B) by columns, as floats: each element of the product is then one dot product
       over two contiguous runs. The packed copies also make A or B overlapping D harmless. */
    pa = (float *)malloc(m * k * sizeof *pa);
    pb = (float *)malloc(n * k * sizeof *pb);
    col = (float *)malloc(m * sizeof *col);
    if (have_bias)
        bias = (float *)malloc(m * sizeof *bias);
    if (!pa || !pb || !col || (have_bias && !bias)) {
        free(pa);
        free(pb);
        free(col);
        free(bias);
        return CUBLAS_STATUS_ALLOC_FAILED;
    }
    if (have_bias)
        to_float(bias, d->bias, sh.bias_type, m, 1);
    for (b = 0; b < sh.batch; b++) {
        const u8 *ab = (const u8 *)A + (u64)b * (u64)la->stride * type_size(la->type);
        const u8 *bb = (const u8 *)B + (u64)b * (u64)lb->stride * type_size(lb->type);
        const u8 *cb = C ? (const u8 *)C + (u64)b * (u64)lc->stride * type_size(lc->type) : 0;
        u8 *db = (u8 *)D + (u64)b * (u64)ld->stride * type_size(ld->type);
        if (sh.ta) /* op(A)(i, l) = A(l, i): row i of op(A) is column i of A */
            for (i = 0; i < m; i++)
                to_float(pa + i * k, ab + i * (u64)la->ld * type_size(la->type), la->type, k, 1);
        else /* row i of A: every ld-th element from A(i, 0) */
            for (i = 0; i < m; i++)
                to_float(pa + i * k, ab + i * type_size(la->type), la->type, k, (u64)la->ld);
        if (sh.tb) /* op(B)(l, j) = B(j, l): column j of op(B) is row j of B */
            for (j = 0; j < n; j++)
                to_float(pb + j * k, bb + j * type_size(lb->type), lb->type, k, (u64)lb->ld);
        else
            for (j = 0; j < n; j++)
                to_float(pb + j * k, bb + j * (u64)lb->ld * type_size(lb->type), lb->type, k, 1);
        for (j = 0; j < n; j++) {
            const float *bj = pb + j * k;
            u8 *dj = db + j * (u64)ld->ld * type_size(ld->type);
            if (beta != 0) /* read the whole column of C before D's is written: C may be D */
                to_float(col, cb + j * (u64)lc->ld * type_size(lc->type), lc->type, m, 1);
            for (i = 0; i < m; i++) {
                float v = alpha * dot(pa + i * k, bj, k);
                if (beta != 0)
                    v += beta * col[i];
                if (have_bias)
                    v += bias[i];
                if (relu && v < 0)
                    v = 0;
                col[i] = v;
            }
            if (ld->type == CUDA_R_32F)
                memcpy(dj, col, m * sizeof *col);
            else if (ld->type == CUDA_R_16F)
                for (l = 0; l < m; l++) {
                    bits.f = col[l];
                    ((u16 *)dj)[l] = (u16)mc_f32_to_f16(bits.u);
                }
            else
                for (l = 0; l < m; l++) {
                    bits.f = col[l];
                    ((u16 *)dj)[l] = (u16)mc_f32_to_bf16(bits.u);
                }
        }
    }
    free(pa);
    free(pb);
    free(col);
    free(bias);
    return CUBLAS_STATUS_SUCCESS;
}
