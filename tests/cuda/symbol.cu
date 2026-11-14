/* Objects that live on the device: __constant__ and __device__ variables, filled in from the host by
   cudaMemcpyToSymbol and by their initialisers, written by kernels, read back by cudaMemcpyFromSymbol. */
#include <stdio.h>
#include <cuda_runtime.h>

struct Par { float scale; int shift; double bias[2]; };
__constant__ float c_w[16];
__constant__ Par c_par;
__device__ int d_count[4];
__device__ const float d_tab[5] = {0.5f, 1.5f, -2.0f, 8.0f, 0.125f};
__device__ static const int d_perm[8] = {7, 2, 5, 0, 3, 6, 1, 4};
__device__ double d_acc = 10.0;

__device__ float weigh(int i) { return c_w[i & 15] * c_par.scale + d_tab[i % 5]; }

__global__ void k(float *out, int n)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    out[i] = weigh(i) + (float)(d_perm[i & 7] << c_par.shift) + (float)c_par.bias[i & 1];
    atomicAdd(&d_count[i & 3], i);
    if (i == 0) d_acc += 2.5;
}

int main(void)
{
    float w[16], *out, h[100]; Par p = {0.25f, 2, {100.0, -100.0}}; int cnt[4]; double acc; float tab[5];
    for (int i = 0; i < 16; i++) w[i] = (float)(i * i) - 20.0f;
    cudaMemcpyToSymbol(c_w, w, sizeof w);
    cudaMemcpyToSymbol(c_par, &p, sizeof p);
    cudaMalloc(&out, sizeof h);
    k<<<4, 32>>>(out, 100);
    k<<<1, 128>>>(out, 100);
    if (cudaDeviceSynchronize()) { printf("launch failed\n"); return 1; }
    cudaMemcpy(h, out, sizeof h, cudaMemcpyDeviceToHost);
    cudaMemcpyFromSymbol(cnt, d_count, sizeof cnt);
    cudaMemcpyFromSymbol(&acc, d_acc, sizeof acc);
    cudaMemcpyFromSymbol(tab, d_tab, sizeof tab);
    for (int i = 0; i < 100; i += 9) printf("%d %g\n", i, h[i]);
    printf("count %d %d %d %d acc %g tab %g %g\n", cnt[0], cnt[1], cnt[2], cnt[3], acc, tab[1], tab[4]);
    /* part of an object, at an offset */
    w[0] = 1000.0f;
    cudaMemcpyToSymbol(c_w, w, sizeof(float), 5 * sizeof(float));
    k<<<1, 8>>>(out, 8);
    cudaDeviceSynchronize();
    cudaMemcpy(h, out, 8 * sizeof(float), cudaMemcpyDeviceToHost);
    printf("%g %g %g\n", h[4], h[5], h[6]);
    return 0;
}
