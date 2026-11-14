#include <cuda_runtime.h>
#include <stdio.h>
struct gnp_t { int g; float eps; const float *w; int n[8]; };
struct split_t { int a; int pad[7]; gnp_t g2; float z[10]; };
__device__ __forceinline__ float use(const gnp_t &g, int i) { return g.eps * g.n[3] + g.g; }
__global__ void k(float *o, gnp_t gp, split_t sp, int which) {
    const gnp_t &g = which ? gp : sp.g2;
    int i = threadIdx.x;
    o[i] = use(g, i) + sp.a + sp.z[3];
}
int main() {
    float *o; gnp_t gp = {}; split_t sp = {};
    gp.g = 2; gp.eps = 0.5f; for (int i = 0; i < 8; i++) { gp.n[i] = i; sp.g2.n[i] = 10 * i; }
    sp.g2.g = 7; sp.g2.eps = 2.f; sp.a = 100; sp.z[3] = 0.25f;
    cudaMallocManaged(&o, 64 * sizeof(float));
    k<<<1, 8>>>(o, gp, sp, 1); cudaDeviceSynchronize();
    float s = 0; for (int i = 0; i < 8; i++) s += o[i];
    k<<<1, 8>>>(o, gp, sp, 0); cudaDeviceSynchronize();
    for (int i = 0; i < 8; i++) s += o[i];
    printf("%g\n", s);
    return 0;
}
