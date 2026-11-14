#include <cuda_runtime.h>
#include <stdio.h>
/* __ldg: on the GPU a read of memory that nothing writes while the kernel runs (ld.global.nc, also of two and
   four numbers at once); the same numbers as plain reads on the host */
struct Rec { unsigned char b[8]; short s[4]; unsigned u[4]; float f[4]; long long q; double d; };
__host__ __device__ static unsigned mix(unsigned h, unsigned v) { return (h ^ v) * 16777619u; }
#define WORK(attr, name) attr static unsigned name(const Rec *r) { \
    unsigned h = 2166136261u; \
    uint4 u4 = __ldg((const uint4 *)r->u); uint2 u2 = __ldg((const uint2 *)(r->u + 1)); \
    int4 i4 = __ldg((const int4 *)r->u); int2 i2 = __ldg((const int2 *)(r->u + 2)); \
    float4 f4 = __ldg((const float4 *)r->f); float2 f2 = __ldg((const float2 *)(r->f + 2)); \
    h = mix(h, __ldg(r->b + 3)); h = mix(h, (unsigned)__ldg((const signed char *)r->b + 5)); \
    h = mix(h, (unsigned)__ldg(r->s + 1)); h = mix(h, __ldg((const unsigned short *)r->s + 2)); \
    h = mix(h, __ldg(r->u + 3)); h = mix(h, (unsigned)__ldg((const int *)r->u)); \
    h = mix(h, (unsigned)__ldg(&r->q)); h = mix(h, (unsigned)(__ldg((const unsigned long long *)&r->q) >> 32)); \
    h = mix(h, (unsigned)(int)__ldg(r->f + 1)); h = mix(h, (unsigned)(int)__ldg(&r->d)); \
    h = mix(h, u4.x); h = mix(h, u4.y); h = mix(h, u4.z); h = mix(h, u4.w); h = mix(h, u2.x); h = mix(h, u2.y); \
    h = mix(h, (unsigned)i4.x + (unsigned)i4.w); h = mix(h, (unsigned)i2.x - (unsigned)i2.y); \
    h = mix(h, (unsigned)(int)(f4.x + f4.y * 3.f + f4.z * 5.f + f4.w * 7.f)); h = mix(h, (unsigned)(int)(f2.x - f2.y)); \
    return h; \
}
WORK(__device__, dwork) WORK(, hwork)
__global__ void k(unsigned *o, const Rec *r, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n) o[i] = dwork(r + i);
}
int main() {
    enum { N = 1024 };
    unsigned *o, x = 7u; Rec *r; int bad = 0;
    cudaMallocManaged(&o, N * sizeof *o); cudaMallocManaged(&r, N * sizeof *r);
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < 8; j++) { x = x * 1664525u + 1013904223u; r[i].b[j] = (unsigned char)(x >> 24); }
        for (int j = 0; j < 4; j++) {
            x = x * 1664525u + 1013904223u; r[i].s[j] = (short)(x >> 16);
            x = x * 1664525u + 1013904223u; r[i].u[j] = x;
            x = x * 1664525u + 1013904223u; r[i].f[j] = (float)(int)(x >> 20) - 2048.f;
        }
        x = x * 1664525u + 1013904223u; r[i].q = (long long)x * 2654435761LL - 99;
        x = x * 1664525u + 1013904223u; r[i].d = (double)(int)(x >> 12) - 500000.;
    }
    k<<<N / 128, 128>>>(o, r, N);
    cudaDeviceSynchronize();
    for (int i = 0; i < N; i++) bad += o[i] != hwork(r + i);
    printf("%d\n", bad);
    return 0;
}
