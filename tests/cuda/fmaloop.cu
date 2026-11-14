/* Which of two products that are added is fused with the sum, inside a for loop: the one whose factors are
   read from memory first, whatever the order they are written in and however long the integer arithmetic
   behind them (nvcc ranks the loop's counter below what the body reads). Expected: nvcc on a real GPU. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <cuda_runtime.h>

__device__ __noinline__ float l0(const float *dyo, const float *dxo, int z, int yy, int xx) {
    float r = 0.f;
    for (int k = 0; k < 64; k++) {
        const int fz = 4 * z + (k >> 4), fy = 4 * yy + (k >> 2 & 3), fx = 4 * xx + (k & 3);
        const float dy = dyo[fz] + (float)fy, dx = dxo[fz] + (float)fx;
        r += sqrtf(dy * dy + dx * dx);
    }
    return r;
}
__device__ __noinline__ float l1(const float *dyo, const float *dxo, int z, int yy, int xx) {
    float r = 0.f;
    for (int k = 0; k < 64; k++) {
        const int fz = 4 * z + (k >> 4), fy = 4 * yy + (k >> 2 & 3), fx = 4 * xx + (k & 3);
        const float dy = dyo[fz] + (float)fy, dx = dxo[fz] + (float)fx;
        r += sqrtf(dx * dx + dy * dy);
    }
    return r;
}
__device__ __noinline__ float l2(const float *dyo, const float *dxo, int z, int yy, int xx) {
    float r = 0.f;
    for (int k = 0; k < 64; k++) {
        const int fz = 4 * z + (k >> 4), fy = 4 * yy + (k & 3), fx = 4 * xx + (k >> 2 & 3);
        const float dy = dyo[fz] + (float)fy, dx = dxo[fz] + (float)fx;
        r += sqrtf(dy * dy + dx * dx);
    }
    return r;
}
__device__ __noinline__ float l3(const float *dyo, const float *dxo, int z, int yy, int xx) {
    float r = 0.f;
    for (int k = 0; k < 64; k++) {
        const int fz = 4 * z + (k >> 4), fy = 4 * yy + (k & 3), fx = 4 * xx + (k >> 2 & 3);
        const float dy = dyo[fz] + (float)fy, dx = dxo[fz] + (float)fx;
        r += sqrtf(dx * dx + dy * dy);
    }
    return r;
}
__device__ __noinline__ float l4(const float *dyo, const float *dxo, int z, int yy, int xx) {
    float r = 0.f;
    for (int k = 0; k < 64; k++) {
        const int fz = 4 * z + (k >> 4), fy = yy + k, fx = xx + k;
        const float dy = dyo[fz] + (float)fy, dx = dxo[fz] + (float)fx;
        r += sqrtf(dy * dy + dx * dx);
    }
    return r;
}
__device__ __noinline__ float l5(const float *dyo, const float *dxo, int z, int yy, int xx) {
    float r = 0.f;
    for (int k = 0; k < 64; k++) {
        const int fz = 4 * z + (k >> 4), fy = yy + k, fx = xx + k;
        const float dy = dyo[fz] + (float)fy, dx = dxo[fz] + (float)fx;
        r += sqrtf(dx * dx + dy * dy);
    }
    return r;
}
__global__ void run(const float *dyo, const float *dxo, float *out) {
    const int i = threadIdx.x, z = i & 3, yy = i >> 2 & 3, xx = i >> 4;
    out[6 * i + 0] = l0(dyo, dxo, z, yy, xx);
    out[6 * i + 1] = l1(dyo, dxo, z, yy, xx);
    out[6 * i + 2] = l2(dyo, dxo, z, yy, xx);
    out[6 * i + 3] = l3(dyo, dxo, z, yy, xx);
    out[6 * i + 4] = l4(dyo, dxo, z, yy, xx);
    out[6 * i + 5] = l5(dyo, dxo, z, yy, xx);
}
int main(void) {
    float dy[32], dx[32], out[6 * 64]; float *ddy, *ddx, *dout; unsigned h[6]; int i, c;
    for (i = 0; i < 32; i++) { dy[i] = -40.3f + 1.37f * (float)i; dx[i] = 17.21f - 0.73f * (float)i; }
    cudaMalloc(&ddy, sizeof dy); cudaMalloc(&ddx, sizeof dx); cudaMalloc(&dout, sizeof out);
    cudaMemcpy(ddy, dy, sizeof dy, cudaMemcpyHostToDevice); cudaMemcpy(ddx, dx, sizeof dx, cudaMemcpyHostToDevice);
    run<<<1, 64>>>(ddy, ddx, dout);
    printf("%d\n", (int)cudaDeviceSynchronize());
    cudaMemcpy(out, dout, sizeof out, cudaMemcpyDeviceToHost);
    for (c = 0; c < 6; c++) {
        h[c] = 2166136261u;
        for (i = 0; i < 64; i++) { unsigned u; memcpy(&u, &out[6 * i + c], 4); h[c] = (h[c] ^ u) * 16777619u; }
        printf("%08x\n", h[c]);
    }
    return 0;
}
