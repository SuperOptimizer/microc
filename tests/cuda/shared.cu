/* Blocks of threads: shared memory (fixed and sized at the launch), barriers, grids and blocks of two and three
   dimensions, a reduction in a block, a block that is not a whole number of warps. */
#include <stdio.h>
#include <cuda_runtime.h>

__global__ void reduce(const int *in, int *out, int n)
{
    __shared__ int s[256];
    int t = threadIdx.x, i = blockIdx.x * blockDim.x + t;
    s[t] = i < n ? in[i] : 0;
    __syncthreads();
    for (int w = blockDim.x / 2; w > 0; w >>= 1) {
        if (t < w) s[t] += s[t + w];
        __syncthreads();
    }
    if (t == 0) out[blockIdx.x] = s[0];
}

__global__ void dyn(float *out, int n)
{
    extern __shared__ float buf[];
    __shared__ int turn;
    int t = threadIdx.x;
    if (t == 0) turn = 3;
    buf[t] = (float)(t * t);
    __syncthreads();
    out[blockIdx.x * n + t] = buf[(t + turn) % n] + buf[n - 1 - t];
}

__global__ void dims(int *out)
{
    int id = (blockIdx.z * gridDim.y + blockIdx.y) * gridDim.x + blockIdx.x;
    int th = (threadIdx.z * blockDim.y + threadIdx.y) * blockDim.x + threadIdx.x;
    int per = blockDim.x * blockDim.y * blockDim.z;
    out[id * per + th] = id * 1000 + th + 100000 * (gridDim.x * 100 + gridDim.y * 10 + gridDim.z) ;
}

__global__ void odd(int *out)
{
    __shared__ int s[50];
    int t = threadIdx.x;
    s[t] = t + 1;
    __syncthreads();
    int a = 0;
    for (int j = 0; j < 50; j++) a += s[j] * ((j + t) % 3);
    __syncthreads();
    s[t] = a;
    __syncthreads();
    out[t] = s[(t + 7) % 50];
}

int main(void)
{
    static int h[1000], ho[8], hd[2 * 3 * 2 * 4 * 3 * 2], hodd[50]; static float hf[3 * 40];
    int *in, *out, *d3, *o5; float *f; long sum = 0; int bad = 0;
    for (int i = 0; i < 1000; i++) h[i] = (i * 37) % 101 - 50;
    cudaMalloc(&in, sizeof h); cudaMalloc(&out, sizeof ho); cudaMalloc(&f, sizeof hf); cudaMalloc(&d3, sizeof hd); cudaMalloc(&o5, sizeof hodd);
    cudaMemcpy(in, h, sizeof h, cudaMemcpyHostToDevice);
    reduce<<<4, 256>>>(in, out, 1000);
    dyn<<<3, 40, 40 * sizeof(float)>>>(f, 40);
    dims<<<dim3(2, 3, 2), dim3(4, 3, 2)>>>(d3);
    odd<<<1, 50>>>(o5);
    if (cudaDeviceSynchronize()) { printf("launch failed: %s\n", cudaGetErrorString(cudaGetLastError())); return 1; }
    cudaMemcpy(ho, out, 4 * sizeof(int), cudaMemcpyDeviceToHost);
    cudaMemcpy(hf, f, sizeof hf, cudaMemcpyDeviceToHost);
    cudaMemcpy(hd, d3, sizeof hd, cudaMemcpyDeviceToHost);
    cudaMemcpy(hodd, o5, sizeof hodd, cudaMemcpyDeviceToHost);
    for (int i = 0; i < 1000; i++) sum += h[i];
    printf("reduce %d %d %d %d (sum %ld)\n", ho[0], ho[1], ho[2], ho[3], sum);
    printf("dyn %g %g %g %g\n", hf[0], hf[39], hf[40 + 17], hf[80 + 38]);
    for (int i = 0; i < 12 * 24; i++) bad += hd[i] != (i / 24) * 1000 + i % 24 + 100000 * 232;
    printf("dims %d wrong, last %d\n", bad, hd[12 * 24 - 1]);
    printf("odd %d %d %d\n", hodd[0], hodd[25], hodd[49]);
    return 0;
}
