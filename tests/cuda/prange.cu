/* ptx.c range_fold: the sign correction of a division by a constant is left out only where the value cannot be
   negative. Each kernel's results against the same code run on the host: loop counters bounded by their test
   (signed, unsigned, the test on the counter or on its next value), and the cases that must keep it: a negative
   start, a counting-down loop, a counter that can wrap, a thread number shifted out of range, a bound with no
   range. */
#include <stdio.h>
#include <cuda_runtime.h>

/* the body as a macro: in the kernel itself (device division; threadIdx.x as t) and on the host */
#define DIVS(t, n, s) do { \
    s = 0;                                                                                                                       \
    _Pragma("unroll 2")                                                                                                          \
    for (int k = 0; k < 14; k++) s += (2 * k + t) / 9 * 7 + (2 * k) / 3 % 3 + k % 3 + (k + t) % 8;   /* folded */                \
    _Pragma("unroll 1")                                                                                                          \
    for (unsigned k = 0; k < 30u; k++) s += (int)k / 7 + (int)k % 5;                                /* folded */                 \
    _Pragma("unroll 1")                                                                                                          \
    for (int k = 0; k < (n & 127); k += 3) s += k / 5 + k % 16;                                      /* folded */                \
    _Pragma("unroll 1")                                                                                                          \
    for (int k = -20 + t; k < 20; k++) s += k / 3 + k % 4 + k % 7;                                    /* kept: negative start */ \
    _Pragma("unroll 1")                                                                                                          \
    for (int k = 20; k >= -5; k--) s += k / 3 + k % 8;                                                /* kept: counts down */    \
    _Pragma("unroll 1")                                                                                                          \
    for (int k = 0; k < n; k += 3) s += k / 5;                                                        /* kept: n unbounded */    \
    unsigned u = 0x7ffffff0u + (unsigned)t;                                                                                      \
    _Pragma("unroll 1")                                                                                                          \
    for (int j = 0; j < 40; j++) { s += (int)u / 9 + (int)u % 16; u += 1u; }                          /* kept: wraps */          \
    s += t / 3 + (t - 16) / 3 + (t - 7) % 8 + (int)((unsigned)t << 26) / 5 + (int)((unsigned)t << 26) % 32 + (t & 0xff) / 6;    /* t: 0 .. 63 */ \
} while (0)
static int divs(int t, int n) { int s; DIVS(t, n, s); return s; }
__global__ void k_div(int *out, int n) {
    int t = threadIdx.x, s;
    DIVS(t, n, s);
    out[t] = s;
}
int main(void) {
    enum { N = 64 };
    int *out, bad = 0;
    long long sum = 0;
    cudaMallocManaged(&out, N * 4);
    for (int n = 37; n <= 300; n += 263) {
        k_div<<<1, N>>>(out, n);
        printf("%d\n", (int)cudaDeviceSynchronize());
        for (int t = 0; t < N; t++) {
            bad += out[t] != divs(t, n);
            sum += out[t];
        }
    }
    printf("%lld %d\n", sum, bad);
    return 0;
}
