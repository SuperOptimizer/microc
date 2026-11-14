#include <stdio.h>
#include <cuda_runtime.h>
struct big { int a[50]; short s; };
struct odd { char c[131]; };
__global__ void k(const big *in, big *out, odd *o, int n) {
    int i = threadIdx.x;
    if (i >= n) return;
    big t = in[i];
    int z[40] = {0};
    odd q = {{0}};
    for (int j = 0; j < 50; j++) t.a[j] += z[j % 40] + i;
    t.s = (short)(t.s + q.c[i % 131]);
    q.c[5] = (char)i;
    out[i] = t; o[i] = q;
}
int main(void) {
    big *in, *out; odd *o; int n = 8; long sum = 0;
    cudaMallocManaged(&in, n * sizeof *in); cudaMallocManaged(&out, n * sizeof *out); cudaMallocManaged(&o, n * sizeof *o);
    for (int i = 0; i < n; i++) { for (int j = 0; j < 50; j++) in[i].a[j] = i * 100 + j; in[i].s = (short)i; }
    k<<<1, 32>>>(in, out, o, n);
    printf("%d\n", (int)cudaDeviceSynchronize());
    for (int i = 0; i < n; i++) { for (int j = 0; j < 50; j++) sum += out[i].a[j] * (j + 1); sum += out[i].s + o[i].c[5]; }
    printf("%ld\n", sum);
    return 0;
}
