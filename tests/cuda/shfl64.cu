/* Warp shuffles of 64-bit values and doubles keep all their bits (they are two 32-bit shuffles). */
#include <stdio.h>
#include <cuda_runtime.h>
__global__ void run(double *o, unsigned long long *q, long *l) {
    const int i = threadIdx.x;
    double a = 1.0 + (double)i * 1e-9; unsigned long long u = 0x0123456789abcdefull * (unsigned long long)(i + 1); long s = -(long)i * 0x100000001L;
    for (int k = 16; k > 0; k >>= 1) { a += __shfl_xor_sync(0xffffffffu, a, k); u ^= __shfl_down_sync(0xffffffffu, u, k); s += __shfl_sync(0xffffffffu, s, (i + k) & 31); }
    o[i] = a; q[i] = u; l[i] = s;
}
int main(void) {
    double o[32], *d; unsigned long long q[32], *dq, x = 0; long l[32], *dl, y = 0; int i;
    cudaMalloc(&d, sizeof o); cudaMalloc(&dq, sizeof q); cudaMalloc(&dl, sizeof l);
    run<<<1, 32>>>(d, dq, dl);
    printf("%d\n", (int)cudaDeviceSynchronize());
    cudaMemcpy(o, d, sizeof o, cudaMemcpyDeviceToHost); cudaMemcpy(q, dq, sizeof q, cudaMemcpyDeviceToHost); cudaMemcpy(l, dl, sizeof l, cudaMemcpyDeviceToHost);
    for (i = 0; i < 32; i++) { x = x * 31 + q[i]; y = y * 31 + l[i]; }
    printf("%.17g %.17g\n%016llx\n%ld\n", o[0], o[31], x, y);
    return 0;
}
