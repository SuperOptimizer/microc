#include <stdio.h>
#include <cuda_runtime.h>
template <int MT, int NR> __global__ void k(const float *x, float *y, int n) {
    float acc[MT][NR][2]; float v[8]; int cnt = 0;
    #pragma unroll
    for (int m = 0; m < MT; m++)
        #pragma unroll
        for (int r = 0; r < NR; r++) { acc[m][r][0] = 0.f; acc[m][r][1] = 1.f; }
    #pragma unroll
    for (int j = 0; j < 8; j++) v[j] = x[threadIdx.x * 8 + j];
    #pragma unroll
    for (int m = 0; m < MT; m++) {
        #pragma unroll
        for (int r = 0; r < NR; r++) {
            if (r == 2) continue;
            #pragma unroll 2
            for (int j = 0; j < 8; j++) { acc[m][r][j & 1] += v[j] * (float)(m + r + 1); cnt++; }
        }
        if (m == 2) break;
    }
    #pragma unroll
    for (int j = 0; j < n; j++) cnt += j;
    float s = (float)cnt;
    #pragma unroll
    for (int m = 0; m < MT; m++) for (int r = 0; r < NR; r++) s += acc[m][r][0] - 2.f * acc[m][r][1];
    y[threadIdx.x] = s;
}
int main(void) {
    float x[64 * 8], y[64], *dx, *dy; double t = 0; int i;
    for (i = 0; i < 64 * 8; i++) x[i] = (float)(i % 37) * 0.25f - 3.f;
    cudaMalloc(&dx, sizeof x); cudaMalloc(&dy, sizeof y); cudaMemcpy(dx, x, sizeof x, cudaMemcpyHostToDevice);
    k<4, 3><<<1, 64>>>(dx, dy, 5);
    printf("%d\n", (int)cudaDeviceSynchronize());
    cudaMemcpy(y, dy, sizeof y, cudaMemcpyDeviceToHost);
    for (i = 0; i < 64; i++) t += y[i] * (i + 1);
    printf("%.3f %.3f %.3f\n", y[0], y[63], t);
    return 0;
}
