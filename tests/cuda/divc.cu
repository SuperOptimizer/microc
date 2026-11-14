#include <cuda_runtime.h>
#include <stdio.h>
/* 32-bit division and remainder by constants in device code, against the host's by the same numbers
   that it cannot know */
#define DS(X) X(3) X(5) X(7) X(10) X(37) X(96) X(180) X(641) X(1000) X(65537) X(1000003) X(715827883) X(2147483647)
__device__ unsigned mix(unsigned h, unsigned v) { return (h ^ v) * 16777619u; }
__global__ void k(unsigned *o, const int *a, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    int s = a[i]; unsigned u = (unsigned)s, h = 2166136261u;
#define X(d) h = mix(h, (unsigned)(s / d)); h = mix(h, (unsigned)(s % d)); h = mix(h, u / d##u); h = mix(h, u % d##u);
    DS(X)
#undef X
    h = mix(h, (unsigned)(s / -7)); h = mix(h, (unsigned)(s % -7)); h = mix(h, u / 3000000000u); h = mix(h, u % 3000000000u);
    o[i] = h;
}
static unsigned hmix(unsigned h, unsigned v) { return (h ^ v) * 16777619u; }
int main() {
    enum { N = 4096 };
    static const int D[] = { 3, 5, 7, 10, 37, 96, 180, 641, 1000, 65537, 1000003, 715827883, 2147483647 };
    volatile int vd; volatile unsigned vu;
    unsigned *o, x = 12345u; int *a, bad = 0;
    cudaMallocManaged(&o, N * sizeof *o); cudaMallocManaged(&a, N * sizeof *a);
    for (int i = 0; i < N; i++) {
        x = x * 1664525u + 1013904223u;
        a[i] = i < 64 ? i - 32 : i < 128 ? (int)(0x7fffffffu - (unsigned)(i - 64)) : i < 192 ? (int)(0x80000000u + (unsigned)(i - 128)) : (int)(x >> (i % 3 * 8));
    }
    k<<<N / 256, 256>>>(o, a, N); cudaDeviceSynchronize();
    for (int i = 0; i < N; i++) {
        int s = a[i]; unsigned u = (unsigned)s, h = 2166136261u;
        for (unsigned j = 0; j < sizeof D / sizeof D[0]; j++) {
            vd = D[j]; vu = (unsigned)D[j];
            h = hmix(h, (unsigned)(s / vd)); h = hmix(h, (unsigned)(s % vd)); h = hmix(h, u / vu); h = hmix(h, u % vu);
        }
        vd = -7; vu = 3000000000u;
        h = hmix(h, (unsigned)(s / vd)); h = hmix(h, (unsigned)(s % vd)); h = hmix(h, u / vu); h = hmix(h, u % vu);
        bad += h != o[i];
    }
    printf("%d\n", bad);
    return 0;
}
