#include <stdio.h>
#include <cuda_runtime.h>
#include <cuda_fp16.h>
struct h2 { unsigned short a, b; };
__global__ void run(const float *x, unsigned *o) {
    const int i = threadIdx.x;
    float f = x[i]; unsigned u = __float_as_uint(f) ^ 0x80000000u; float g = __uint_as_float(u);
    union { float f; unsigned u; unsigned short s[2]; unsigned char c[4]; } v; v.f = g * 3.f;
    h2 q; q.a = v.s[1]; q.b = (unsigned short)(v.c[0] + v.c[3]);
    h2 r = q; unsigned w; { union { h2 h; unsigned u; } t; t.h = r; w = t.u; }
    double d = (double)f * 1.5; long long ll = __double_as_longlong(d);
    union { long long l; unsigned u[2]; float ff[2]; } z; z.l = ll; z.ff[1] = g;
    o[i] = u ^ v.u ^ w ^ z.u[0] ^ (z.u[1] >> 3) ^ (unsigned)(signed char)v.c[2];
}
int main(void) {
    float x[32], *dx; unsigned o[32], h = 2166136261u, *dout; int i;
    for (i = 0; i < 32; i++) x[i] = (float)(i * i) * 0.37f - 20.f;
    cudaMalloc(&dx, sizeof x); cudaMalloc(&dout, sizeof o); cudaMemcpy(dx, x, sizeof x, cudaMemcpyHostToDevice);
    run<<<1, 32>>>(dx, dout);
    printf("%d\n", (int)cudaDeviceSynchronize());
    cudaMemcpy(o, dout, sizeof o, cudaMemcpyDeviceToHost);
    for (i = 0; i < 32; i++) h = (h ^ o[i]) * 16777619u;
    printf("%08x %08x %08x\n", o[0], o[31], h);
    return 0;
}
