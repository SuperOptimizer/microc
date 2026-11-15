// mcc: --cuda-fast-math
/* --cuda-fast-math float math as nvcc's -use_fast_math writes it (ptx.c fast_libm; MCC_NOFASTEXP: the device library): expf /
   __expf / exp2f / exp10f (ex2.approx), logf / __logf / log2f / log10f (lg2.approx), sinf / cosf (sin / cos.approx), powf (__powf).
   tests/cudaqemu.sh takes the first-line "// mcc: FLAGS"; MCC_NOFASTEXP=1 runs the library forms. Against the host, to 4e-6
   relative + the argument's own rounding (|x| 2^-22 for exponentials, 2^-20 absolute for sin / cos on [-pi, pi]). */
#include <stdio.h>
#include <math.h>
#include <cuda_runtime.h>
enum { NF = 11 };
__global__ void k(float *o, const float *a) {
    int t = threadIdx.x;
    float x = a[t], p = fabsf(x) + 0.01f, *r = o + NF * t;
    r[0] = expf(x); r[1] = __expf(x); r[2] = exp2f(x); r[3] = logf(p); r[4] = __logf(p); r[5] = log2f(p);
    r[6] = sinf(x * 0.3f); r[7] = cosf(x * 0.3f); r[8] = powf(p, x * 0.2f); r[9] = log10f(p); r[10] = exp10f(x * 0.3f);
}
int main(void) {
    enum { N = 64 };
    float *o, *a;
    int bad = 0;
    cudaMallocManaged(&o, N * NF * 4); cudaMallocManaged(&a, N * 4);
    for (int t = 0; t < N; t++) a[t] = (t - 32) * 0.3125f;
    k<<<1, N>>>(o, a);
    printf("%d\n", (int)cudaDeviceSynchronize());
    for (int t = 0; t < N; t++) {
        const float xf = a[t], pf = fabsf(xf) + 0.01f, x3 = xf * 0.3f, x2 = xf * 0.2f;
        const double x = xf, p = pf, l2p = fabs(log2(p) * x2);
        const double r[NF] = {exp(x), exp(x), exp2(x), log(p), log(p), log2(p), sin(x3), cos(x3), pow(p, x2), log10(p), pow(10.0, x3)};
        const double ex[NF] = {fabs(x), fabs(x), fabs(x), 0, 0, 0, 0, 0, l2p + 1, 0, fabs(x3) * 3.33};
        for (int j = 0; j < NF; j++) {
            const double tol = 4e-6 * fabs(r[j]) + 1e-6 + ex[j] * 2.4e-7 * fabs(r[j]) + (j == 6 || j == 7 ? 1e-6 : 0);
            if (fabs(o[NF * t + j] - r[j]) > tol) { bad++; if (bad < 4) printf("t %d f %d: %.9g vs %.9g\n", t, j, o[NF * t + j], r[j]); }
        }
    }
    printf("%d\n", bad);
    return 0;
}
