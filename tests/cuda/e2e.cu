/* mcc only (a host object read by device code). A whole CUDA program, compiled by mcc and run by mcuda: kernels with shared memory, a host table the device
   code reads (bound through the module's symbol table), a device function, the C library from a kernel,
   an intrinsic written as inline PTX, cudaMalloc / cudaMemcpy, a stream and an event. */
#include <stdio.h>
#include <math.h>
#include <cuda_runtime.h>

static const float tab[4] = {1.0f, 2.0f, 3.0f, 4.0f};
__device__ static float sq(float x) { return x * x + tab[(int)x & 3]; }
__device__ float helper(const float *p, int n)
{
    float s = 0;
    int i;
    for (i = 0; i < n; i++)
        s += sq(p[i]);
    return sqrtf(s);
}
__global__ void saxpy(int n, float a, const float *x, float *y)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    __shared__ float tmp[64];
    if (i < n) {
        tmp[threadIdx.x & 63] = a * x[i];
        y[i] = tmp[threadIdx.x & 63] + y[i] + helper(x, 2) + expf(0.0f);
    }
}
__global__ void fill(double *d, long n, unsigned long long seed)
{
    long i = blockIdx.x * blockDim.x + threadIdx.x;
    unsigned r;
    if (i >= n) return;
    asm volatile("popc.b64 %0, %1;" : "=r"(r) : "l"(seed + i));
    d[i] = (double)r / (i + 1);
}
/* the sum of a block's 64 values, by shared memory and a barrier: out[block] */
__global__ void blocksum(const float *x, float *out)
{
    __shared__ float s[64];
    int t = threadIdx.x, step;
    s[t] = x[blockIdx.x * 64 + t];
    __syncthreads();
    for (step = 32; step > 0; step >>= 1) {
        if (t < step) s[t] += s[t + step];
        __syncthreads();
    }
    if (t == 0) out[blockIdx.x] = s[0];
}
/* printf in a kernel: every kind of argument */
__global__ void say(int n, float f)
{
    unsigned __int128 w = (unsigned __int128)(n + threadIdx.x) << 100 | 12345;
    __int128 sw = (__int128)0 - (__int128)w; /* not -w: mcc does not negate the high half */
    printf("dev %d: %d %6.2f %s %lld %c %x\n", (int)threadIdx.x, n + (int)threadIdx.x, f, "text", 5000000000LL, 'c', 255u);
    printf("dev %d: %d %d %d\n", (int)threadIdx.x, (int)(w % 1000003), (int)(w / ((unsigned __int128)1 << 90) % 100000), (int)(sw % 1000003));
}

int main(void)
{
    float *x, *y, *o, hx[256], hy[256], ho[4];
    double *d, hd[10];
    int i;
    cudaStream_t st;
    cudaEvent_t ev;
    dim3 g(4, 1, 1);
    printf("malloc %d %d %d %d\n", (int)cudaMalloc(&x, sizeof hx), (int)cudaMalloc(&y, sizeof hy), (int)cudaMalloc(&d, sizeof hd),
           (int)cudaMalloc(&o, sizeof ho));
    for (i = 0; i < 256; i++) { hx[i] = i; hy[i] = 1; }
    cudaMemcpy(x, hx, sizeof hx, cudaMemcpyHostToDevice);
    cudaMemcpy(y, hy, sizeof hy, cudaMemcpyHostToDevice);
    cudaStreamCreate(&st);
    cudaEventCreate(&ev);
    saxpy<<<g, 32>>>(100, 2.0f, x, y);
    printf("saxpy %s\n", cudaGetErrorString(cudaGetLastError()));
    fill<<<1, 16, 0, st>>>(d, 10, 0xff00ull);
    blocksum<<<4, 64, 0, st>>>(x, o);
    cudaEventRecord(ev, st);
    cudaEventSynchronize(ev);
    cudaStreamSynchronize(st);
    cudaMemcpy(hy, y, sizeof hy, cudaMemcpyDeviceToHost);
    cudaMemcpy(hd, d, sizeof hd, cudaMemcpyDeviceToHost);
    cudaMemcpy(ho, o, sizeof ho, cudaMemcpyDeviceToHost);
    /* y[i] = 1 + 2 i + sqrt(0 + 1 + 1 + 2) + 1 below 100, untouched above */
    printf("%g %g %g %g %g\n", hy[0], hy[50], hy[99], hy[100], hd[3]);
    printf("%g %g %g %g\n", ho[0], ho[1], ho[2], ho[3]);
    say<<<1, 2>>>(7, 2.5f);
    cudaDeviceSynchronize();
    printf("free %d %d, last %s\n", (int)cudaFree(x), (int)cudaFree(y), cudaGetErrorString(cudaGetLastError()));
    return 0;
}
