#include <stdio.h>
#include <cuda_runtime.h>
template <int N> __global__ void __launch_bounds__(N * 64, 2) k(float *x, int n) { int i = blockIdx.x * blockDim.x + threadIdx.x; if (i < n) x[i] = (float)i * 0.5f; }
__global__ void __launch_bounds__(256) k2(float *x, int n) { int i = blockIdx.x * blockDim.x + threadIdx.x; if (i < n) x[i] += 1.f; }
int main(void) {
    float *x; int n = 1024; double s = 0;
    cudaMallocManaged(&x, n * 4);
    k<2><<<8, 128>>>(x, n); k2<<<4, 256>>>(x, n);
    printf("%d\n", (int)cudaDeviceSynchronize());
    for (int i = 0; i < n; i++) s += x[i];
    printf("%.1f\n", s);
    return 0;
}
