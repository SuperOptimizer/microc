/* mcuda_device.h: __ldg of the vector types (ld.global.nc.v4 / .v2) against plain reads on the host. */
#include <stdio.h>
#include <cuda_runtime.h>
__global__ void k_ldg(float *out, const uint4 *__restrict__ a, const float4 *__restrict__ b, const int2 *__restrict__ c,
                      const float2 *__restrict__ d, const int4 *__restrict__ e, const uint2 *__restrict__ f) {
    int t = threadIdx.x;
    uint4 x = __ldg(a + t); float4 y = __ldg(b + t); int2 z = __ldg(c + t); float2 w = __ldg(d + t); int4 u = __ldg(e + t); uint2 v = __ldg(f + t);
    out[t] = (float)(x.x + 2 * x.y + 3 * x.z + 5 * x.w) + y.x - y.y * 2.f + y.z * 4.f - y.w + (float)(z.x - 3 * z.y) + w.x * 8.f - w.y
             + (float)(u.x + u.y - u.z + 7 * u.w) + (float)(v.x ^ v.y);
}
int main(void) {
    enum { N = 32 };
    uint4 *a; float4 *b; int2 *c; float2 *d; int4 *e; uint2 *f; float *out;
    int bad = 0; double sum = 0;
    cudaMallocManaged(&a, N * 16); cudaMallocManaged(&b, N * 16); cudaMallocManaged(&c, N * 8); cudaMallocManaged(&d, N * 8);
    cudaMallocManaged(&e, N * 16); cudaMallocManaged(&f, N * 8); cudaMallocManaged(&out, N * 4);
    for (int t = 0; t < N; t++) {
        a[t] = uint4{(unsigned)t, (unsigned)t * 3u, 7u, (unsigned)(100 - t)}; b[t] = float4{t * 0.5f, -t * 0.25f, 1.5f, (float)t};
        c[t] = int2{-t, t * 2}; d[t] = float2{t * 0.125f, 3.f}; e[t] = int4{t, -2 * t, 9, -t}; f[t] = uint2{(unsigned)t * 5u, 0xa5u};
    }
    k_ldg<<<1, N>>>(out, a, b, c, d, e, f);
    printf("%d\n", (int)cudaDeviceSynchronize());
    for (int t = 0; t < N; t++) {
        uint4 x = a[t]; float4 y = b[t]; int2 z = c[t]; float2 w = d[t]; int4 u = e[t]; uint2 v = f[t];
        float r = (float)(x.x + 2 * x.y + 3 * x.z + 5 * x.w) + y.x - y.y * 2.f + y.z * 4.f - y.w + (float)(z.x - 3 * z.y) + w.x * 8.f - w.y
                  + (float)(u.x + u.y - u.z + 7 * u.w) + (float)(v.x ^ v.y);
        bad += out[t] != r; sum += out[t];
    }
    printf("%.3f %d\n", sum, bad);
    return 0;
}
