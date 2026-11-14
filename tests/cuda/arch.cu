/* mcc only: host code calls the atomic functions.  __CUDA_ARCH__ is defined for the unit, so a function for both sides takes its device form; called from
   host code, the atomic operations in it are the plain ones. */
#include <stdio.h>
#include <cuda_runtime.h>
template <class T> __host__ __device__ __forceinline__ void acc(float *p, T v) {
#ifdef __CUDA_ARCH__
    atomicAdd(p, (float)v);
#else
    *p += (float)v;
#endif
}
__host__ __device__ __forceinline__ int most(int *p, int v) { atomicMax(p, v); return atomicCAS(p, v, v + 1); }
__global__ void k(float *s, int *m) {
    acc(s, threadIdx.x + 1);
    most(m, (int)threadIdx.x);
}
int main() {
    float h = 0, *s;
    int hm = 5, *m, r;
    for (int i = 0; i < 64; i++) acc(&h, i + 1);
    r = most(&hm, 9);
    cudaMallocManaged((void **)&s, sizeof *s);
    cudaMallocManaged((void **)&m, sizeof *m);
    *s = 0; *m = -1;
    k<<<1, 64>>>(s, m);
    cudaDeviceSynchronize();
    printf("arch %d host %g %d %d dev %g %d\n", __CUDA_ARCH__ >= 300, h, r, hm, *s, *m);
    return 0;
}
