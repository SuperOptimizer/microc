#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <stdio.h>
/* device code's rewrites of sums: a choice between x + j and x + k, constants taken to the outside of sums
   and out of an int index, a number put together from its own two halves; and the half pair's arithmetic
   (one instruction for both halves on the GPU), against the host's */
struct P { int a, b; };
__host__ __device__ static unsigned mix(unsigned h, unsigned v) { return (h ^ v) * 16777619u; }
__host__ __device__ static unsigned bits(__half2 h) { return (unsigned)__half_as_ushort(h.x) | (unsigned)__half_as_ushort(h.y) << 16; }
#define WORK(attr, name) attr static unsigned name(const int *t, const float *f, int i, int n) { \
    unsigned h = 2166136261u, u = (unsigned)t[i]; \
    int x = t[i] & 255, y = t[(i + 7) % n] & 255, c = t[(i + 3) % n] & 1, d = (t[(i + 5) % n] >> 1) & 1; \
    const int *p = t + 256; \
    int s1 = c ? x + 9 : x + 4, s2 = c ? x - 3 : x, s3 = d ? y + 100 : y - 100, s4 = c ? x + 17 : y + 17; \
    long long w = c ? (long long)x + 5000000000LL : (long long)x - 7; \
    h = mix(h, (unsigned)s1); h = mix(h, (unsigned)s2); h = mix(h, (unsigned)s3); h = mix(h, (unsigned)s4); \
    h = mix(h, (unsigned)w); h = mix(h, (unsigned)(w >> 32)); \
    h = mix(h, (unsigned)p[x + 3]); h = mix(h, (unsigned)p[x - 200]); h = mix(h, (unsigned)p[(x + 1) + y]); \
    h = mix(h, (unsigned)*(p - (x - 40))); h = mix(h, (unsigned)*(p + (y + 2) * 2)); h = mix(h, (unsigned)p[s1 + 1]); \
    h = mix(h, (unsigned)p[(c ? x + 2 : x + 6) + y + 1]); h = mix(h, (unsigned)((x + 5) + y + (y - 9))); \
    h = mix(h, (u & 0xffff) | (u >> 16) << 16); h = mix(h, (u >> 16 & 0xffff) | (u & 0xffff) << 16); h = mix(h, u >> 9 & 0x7fffff); \
    { \
        __half2 a = __floats2half2_rn(f[i], f[(i + 1) % n]), b = __floats2half2_rn(f[(i + 2) % n], 0.5f), e = __float2half2_rn(f[(i + 4) % n]); \
        __half2 r = __hfma2(a, b, e), q = __hadd2(r, a), m = __hmul2(q, b), z = __hsub2(m, e); \
        r = __hfma2(r, b, q); z.x = __hadd(z.x, r.y); \
        h = mix(h, bits(r)); h = mix(h, bits(q)); h = mix(h, bits(m)); h = mix(h, bits(z)); \
    } \
    return h; \
}
WORK(__device__, dwork) WORK(, hwork)
__global__ void k(unsigned *o, const int *t, const float *f, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) o[i] = dwork(t, f, i, n);
}
int main() {
    enum { N = 2048 };
    unsigned *o, x = 99u; int *t, bad = 0; float *f;
    cudaMallocManaged(&o, N * sizeof *o); cudaMallocManaged(&t, N * sizeof *t); cudaMallocManaged(&f, N * sizeof *f);
    for (int i = 0; i < N; i++) {
        x = x * 1664525u + 1013904223u; t[i] = (int)x;
        x = x * 1664525u + 1013904223u; f[i] = (float)(int)(x >> 20) / 512.0f - 4.0f;
    }
    k<<<N / 256, 256>>>(o, t, f, N); cudaDeviceSynchronize();
    for (int i = 0; i < N; i++) bad += o[i] != hwork(t, f, i, N);
    printf("%d\n", bad);
    return 0;
}
