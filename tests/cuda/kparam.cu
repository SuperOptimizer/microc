#include <stdio.h>
#include <stdint.h>
#include <cuda_runtime.h>
typedef struct { const float *gamma, *beta, *mean, *rstd; int G; int zs, zo, ns, zhw; } gnp_t;
typedef struct { const void *x2; int c_split; void *y2; int o_split; int accum; gnp_t gp2; int16_t up, yD; unsigned sr; unsigned wkey; int zlo, zhi; int stored_stats; unsigned osr; } split_t;
typedef struct { float a, b; } s8_t;
typedef struct { double d; int i; } s16_t;
typedef struct { int a, b, c; } s12_t;
typedef struct { char c[3]; } s3_t;
template <int A, int B> __global__ void k(const float *x, const uint8_t *w, float *y, int a, int b, int c, int d, int e, int f, int g, gnp_t gp, double *ds, int n, split_t sp, int z, unsigned u) {
    int i = threadIdx.x;
    if (i >= n) return;
    y[i] = x[i] * gp.gamma[i] + gp.beta[i] + (float)(a + 2 * b + 3 * c + 4 * d + 5 * e + 6 * f + 7 * g) + (float)(gp.G + gp.zs * 2 + gp.zo * 3 + gp.ns * 5 + gp.zhw * 7)
         + (float)(sp.c_split + sp.o_split * 2 + sp.accum * 3 + sp.up * 5 + sp.yD * 7 + sp.sr * 11 + sp.wkey * 13 + sp.zlo * 17 + sp.zhi * 19 + sp.stored_stats * 23 + sp.osr * 29)
         + sp.gp2.mean[i] + (float)sp.gp2.zhw + (float)(z * 31 + (int)u * 37 + A * 41 + B * 43) + (float)w[i];
    ds[i] = ((const float *)sp.x2)[i] + ((float *)sp.y2)[i];
}
__global__ void k2(float *y, s8_t s8, int n, s16_t s16, s12_t s12, float f, s3_t s3, double d, s8_t t8) {
    int i = threadIdx.x;
    if (i < n) y[i] = s8.a + 2 * s8.b + (float)s16.d * 3 + (float)(s16.i * 5 + s12.a * 7 + s12.b * 11 + s12.c * 13) + f * 17 + (float)(s3.c[0] + s3.c[1] * 2 + s3.c[2] * 3) + (float)d * 19 + t8.a * 23 + t8.b * 29 + (float)i;
}
int main(void) {
    enum { N = 16 };
    float *x, *y, *g, *b, *m; uint8_t *w; double *ds; double sum = 0;
    cudaMallocManaged(&x, N * 4); cudaMallocManaged(&y, N * 4); cudaMallocManaged(&g, N * 4); cudaMallocManaged(&b, N * 4); cudaMallocManaged(&m, N * 4);
    cudaMallocManaged(&w, N); cudaMallocManaged(&ds, N * 8);
    for (int i = 0; i < N; i++) { x[i] = (float)i; g[i] = 2.f; b[i] = .5f; m[i] = (float)(i * 3); w[i] = (uint8_t)(i + 1); }
    gnp_t gp = {g, b, m, x, 4, 1, 2, 3, 5};
    split_t sp = {x, 1, m, 2, 3, {b, g, m, x, 9, 8, 7, 6, 5}, 3, -2, 7u, 9u, 1, 2, 1, 4u};
    k<1, 4><<<1, 32>>>(x, w, y, 1, 2, 3, 4, 5, 6, 7, gp, ds, N, sp, 3, 5u);
    printf("%d\n", (int)cudaDeviceSynchronize());
    for (int i = 0; i < N; i++) sum += y[i] * (i + 1) + ds[i];
    printf("%.1f\n", sum);
    s8_t s8 = {1.f, 2.f}, t8 = {3.f, 4.f}; s16_t s16 = {1.5, 3}; s12_t s12 = {1, 2, 3}; s3_t s3 = {{1, 2, 3}};
    k2<<<1, 32>>>(y, s8, N, s16, s12, 0.25f, s3, 2.5, t8);
    printf("%d\n", (int)cudaDeviceSynchronize());
    sum = 0; for (int i = 0; i < N; i++) sum += y[i] * (i + 1);
    printf("%.1f\n", sum);
    return 0;
}
