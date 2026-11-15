// mcc: --cuda-fast-math
/* --cuda-fast-math expf / __expf / exp2f / logf / __logf / log2f as ex2.approx / lg2.approx (ptx.c fast_libm; MCC_NOFASTEXP: the
   library). Built with --cuda-fast-math (tests/cudaqemu.sh takes a
   first-line "// mcc: FLAGS"; MCC_NOFASTEXP=1 runs the library forms). Against the host to 4e-6 relative (+ |x| 2^-22 for the exponent's rounding). */
#include <stdio.h>
#include <math.h>
#include <cuda_runtime.h>
__global__ void k(float *o, const float *a) {
    int t = threadIdx.x;
    float x = a[t], p = fabsf(x) + 0.01f;
    o[6 * t] = expf(x); o[6 * t + 1] = __expf(x); o[6 * t + 2] = exp2f(x);
    o[6 * t + 3] = logf(p); o[6 * t + 4] = __logf(p); o[6 * t + 5] = log2f(p);
}
int main(void) {
    enum { N = 64 };
    float *o, *a;
    int bad = 0;
    cudaMallocManaged(&o, N * 24); cudaMallocManaged(&a, N * 4);
    for (int t = 0; t < N; t++) a[t] = (t - 32) * 0.3125f;
    k<<<1, N>>>(o, a);
    printf("%d\n", (int)cudaDeviceSynchronize());
    for (int t = 0; t < N; t++) {
        const double x = a[t], p = fabs(x) + 0.01, r[6] = {exp(x), exp(x), exp2(x), log(p), log(p), log2(p)};
        for (int j = 0; j < 6; j++) bad += fabs(o[6 * t + j] - r[j]) > 4e-6 * fabs(r[j]) + 1e-6 + (j < 3 ? fabs(x) * 2.4e-7 * fabs(r[j]) : 0);
    }
    printf("%d\n", bad);
    return 0;
}
