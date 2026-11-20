#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <stdio.h>
#include <string.h>
/* 16-bit float round trips: a float narrowed to a half and widened again is rounded (the narrowing is the intended rounding and
   must stay); a half widened and narrowed by explicit conversions is what nvcc makes of it (a NaN canonical); a __half2 taken
   apart and stored is its bits, NaN payloads included (mcc's front end widens and narrows there, round_once folds it: without the
   fold a NaN came out 0x7fff); a float rounded through a half and stored as one is rounded once. Values: ones that round, the
   overflow to infinity, half subnormals and their underflow, signed zero, infinity, quiet NaNs. A signalling NaN is left out:
   nvcc's conversions quiet it, the fold copies it. */
__global__ void k(const float *y, const __half *h, float *ry, __half *rh, __half *rr, unsigned short *rb, const __half2 *h2, __half *rc, int n) {
    int i = threadIdx.x;
    if (i >= n)
        return;
    float v = y[i];
    ry[i] = __half2float(__float2half(v));                     /* h2f(f2h(y)): rounded */
    rh[i] = __float2half(__half2float(h[i]));                  /* f2h(h2f(x)): x */
    float w = __half2float(__float2half(v * 3.f));             /* rounded, then stored as a half: rounded once */
    rr[i] = __float2half(w);
    __half2 p = __floats2half2_rn(v, -v);                      /* packing, then the halves' bits */
    rb[2 * i] = __half_as_ushort(__low2half(p));
    rb[2 * i + 1] = __half_as_ushort(__high2half(p));
    __half2 q = h2[i];                                         /* a __half2 taken apart and stored: the bits (fstem's staging) */
    rc[2 * i] = __high2half(q);
    rc[2 * i + 1] = __low2half(q);
}

int main() {
    const float ys[] = {1.f / 3.f, 0.1f, 65519.f, 65520.f, 1e5f, 6e-8f, 2.9e-8f, 3e-8f, 1e-7f, 5.96e-8f, -0.f, 1.0009765625f,
                        1.00048828125f, 1.000732421875f, -2.5e-5f, 1e30f, -1e30f, 0.f};
    const unsigned short hs[] = {0x3c00, 0x0001, 0x03ff, 0x0400, 0x7bff, 0x7c00, 0xfc00, 0x8000, 0x7e00, 0xfe00, 0x3555, 0xc248,
                                 0x8001, 0x83ff, 0x0000, 0x5bff, 0x1234, 0xabcd};
    const int n = sizeof ys / sizeof ys[0];
    float *y, *ry; __half *h, *rh, *rr, *rc; unsigned short *rb; __half2 *h2;
    cudaMallocManaged(&y, n * sizeof *y); cudaMallocManaged(&ry, n * sizeof *ry); cudaMallocManaged(&h, n * sizeof *h);
    cudaMallocManaged(&rh, n * sizeof *rh); cudaMallocManaged(&rr, n * sizeof *rr); cudaMallocManaged(&rb, 2 * n * sizeof *rb);
    cudaMallocManaged(&h2, n * sizeof *h2); cudaMallocManaged(&rc, 2 * n * sizeof *rc);
    memcpy(y, ys, sizeof ys);
    memcpy(h, hs, sizeof hs);
    for (int i = 0; i < n; i++) { unsigned short pr[2] = {hs[i], hs[n - 1 - i]}; memcpy(&h2[i], pr, 4); }
    k<<<1, 32>>>(y, h, ry, rh, rr, rb, h2, rc, n);
    cudaError_t e = cudaDeviceSynchronize();
    for (int i = 0; i < n; i++) {
        unsigned a, b = 0, c = 0, d0 = 0, d1 = 0;
        memcpy(&a, &ry[i], 4); memcpy(&b, &rh[i], 2); memcpy(&c, &rr[i], 2); memcpy(&d0, &rc[2 * i], 2); memcpy(&d1, &rc[2 * i + 1], 2);
        printf("%2d y %08x h %04x r %04x p %04x %04x c %04x %04x\n", i, a, b, c, rb[2 * i], rb[2 * i + 1], d0, d1);
    }
    printf("err %d\n", (int)e);
    return 0;
}
