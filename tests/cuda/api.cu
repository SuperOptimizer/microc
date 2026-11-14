/* The run time's calls as programs make them: streams and events, asynchronous copies and fills, copies of
   parts of rows, page-locked host memory, shared memory above the default made available to a kernel, what a
   launch that cannot be made reports, and the questions about the device whose answers do not depend on it. */
#include <stdio.h>
#include <string.h>
#include <cuda_runtime.h>

__global__ void scale(int *p, int n, int m) { int i = blockIdx.x * blockDim.x + threadIdx.x; if (i < n) p[i] = p[i] * m + 1; }
__global__ void big(int *out, int n)
{
    extern __shared__ int s[];
    int t = threadIdx.x;
    for (int i = t; i < n; i += blockDim.x) s[i] = i * 3 + 1;
    __syncthreads();
    long a = 0;
    for (int i = t; i < n; i += blockDim.x) a += s[n - 1 - i];
    out[t] = (int)(a % 1000003);
}
__global__ void nothing(int *p) { p[0] = 1; }

#define OK(call) do { cudaError_t e_ = (call); if (e_ != cudaSuccess) printf("line %d: %s\n", __LINE__, cudaGetErrorString(e_)); } while (0)

int main(void)
{
    enum { N = 4096 };
    cudaStream_t s1, s2; cudaEvent_t e1, e2, e3;
    int *d1, *d2, *d3, *h1, *h2, *m; void *hp; float ms = -1; size_t fr = 0, tot = 0; int dev = -1, cnt = 0, at = 0, nb = 0;
    long sum = 0; cudaError_t e; cudaDeviceProp pr; static int rows[6][10], back[6][10];
    OK(cudaGetDeviceCount(&cnt)); OK(cudaSetDevice(0)); OK(cudaGetDevice(&dev));
    printf("devices %d, current %d\n", cnt > 0, dev);
    OK(cudaStreamCreateWithFlags(&s1, cudaStreamNonBlocking)); OK(cudaStreamCreate(&s2));
    OK(cudaEventCreate(&e1)); OK(cudaEventCreateWithFlags(&e2, cudaEventDisableTiming)); OK(cudaEventCreateWithFlags(&e3, cudaEventBlockingSync));
    OK(cudaMalloc(&d1, N * sizeof(int))); OK(cudaMalloc(&d2, N * sizeof(int))); OK(cudaMalloc(&d3, 6 * 16 * sizeof(int)));
    OK(cudaMallocHost(&h1, N * sizeof(int))); OK(cudaHostAlloc(&hp, N * sizeof(int), cudaHostAllocPortable)); h2 = (int *)hp;
    for (int i = 0; i < N; i++) h1[i] = i - 100;

    /* a chain on one stream, a second stream that waits for it */
    OK(cudaEventRecord(e1, s1));
    OK(cudaMemcpyAsync(d1, h1, N * sizeof(int), cudaMemcpyHostToDevice, s1));
    scale<<<N / 256, 256, 0, s1>>>(d1, N, 3);
    scale<<<N / 128, 128, 0, s1>>>(d1, N - 7, 2);
    OK(cudaEventRecord(e2, s1));
    OK(cudaStreamWaitEvent(s2, e2, 0));
    OK(cudaMemcpyAsync(d2, d1, N * sizeof(int), cudaMemcpyDeviceToDevice, s2));
    OK(cudaMemsetAsync(d2 + 10, 0x01, 20 * sizeof(int), s2));
    scale<<<N / 256, 256, 0, s2>>>(d2, 50, -1);
    OK(cudaMemcpyAsync(h2, d2, N * sizeof(int), cudaMemcpyDeviceToHost, s2));
    OK(cudaEventRecord(e3, s2));
    OK(cudaEventSynchronize(e3)); OK(cudaStreamSynchronize(s1)); OK(cudaStreamSynchronize(s2));
    e = cudaStreamQuery(s1); printf("stream done: %s\n", cudaGetErrorString(e));
    e = cudaEventQuery(e3); printf("event done: %s\n", cudaGetErrorString(e));
    OK(cudaEventRecord(e2, s1)); OK(cudaEventSynchronize(e2));
    e = cudaEventElapsedTime(&ms, e1, e3); printf("time: %s, %s\n", cudaGetErrorString(e), ms >= 0 ? "not negative" : "negative");
    for (int i = 0; i < N; i++) sum += (long)h2[i] * (i % 13 + 1);
    printf("chain %ld: %d %d %d %d %d %d\n", sum, h2[0], h2[9], h2[10], h2[29], h2[49], h2[N - 1]);

    /* parts of rows: a 6 x 10 host array into a 6 x 16 device one and back */
    for (int i = 0; i < 60; i++) rows[i / 10][i % 10] = i * i;
    OK(cudaMemset(d3, 0xff, 6 * 16 * sizeof(int)));
    OK(cudaMemcpy2DAsync(d3 + 2, 16 * sizeof(int), rows, 10 * sizeof(int), 10 * sizeof(int), 6, cudaMemcpyHostToDevice, s1));
    scale<<<1, 96, 0, s1>>>(d3, 96, 1);
    OK(cudaMemcpy2DAsync(back, 10 * sizeof(int), d3 + 1, 16 * sizeof(int), 10 * sizeof(int), 6, cudaMemcpyDeviceToHost, s1));
    OK(cudaStreamSynchronize(s1));
    sum = 0; for (int i = 0; i < 60; i++) sum += (long)back[i / 10][i % 10] * (i + 1);
    printf("rows %ld: %d %d %d\n", sum, back[0][0], back[0][1], back[5][9]);

    /* 96 KB of shared memory for one block */
    OK(cudaFuncSetAttribute(big, cudaFuncAttributeMaxDynamicSharedMemorySize, 96 * 1024));
    big<<<1, 64, 96 * 1024>>>(d1, 24 * 1024);
    OK(cudaDeviceSynchronize()); OK(cudaGetLastError());
    OK(cudaMemcpy(h1, d1, 64 * sizeof(int), cudaMemcpyDeviceToHost));
    sum = 0; for (int i = 0; i < 64; i++) sum += h1[i];
    printf("shared %ld\n", sum);

    /* launches that cannot be made */
    nothing<<<1, 2048>>>(d1);
    e = cudaGetLastError(); printf("2048 threads: %s\n", cudaGetErrorString(e));
    e = cudaGetLastError(); printf("then: %s\n", cudaGetErrorString(e));
    nothing<<<0, 32>>>(d1);
    e = cudaPeekAtLastError(); printf("no blocks: %s\n", cudaGetErrorString(e)); cudaGetLastError();
    nothing<<<1, 32, 1 << 30>>>(d1);
    e = cudaGetLastError(); printf("too much shared memory: %s (%s)\n", cudaGetErrorString(e), cudaGetErrorName(e));
    e = cudaMemcpy(h1, d1, 16, (cudaMemcpyKind)9); printf("bad direction: %s\n", cudaGetErrorString(e));
    OK(cudaDeviceSynchronize());

    /* managed memory, any-direction copies */
    OK(cudaMallocManaged(&m, 256 * sizeof(int)));
    for (int i = 0; i < 256; i++) m[i] = i;
    scale<<<1, 256>>>(m, 256, 5);
    OK(cudaDeviceSynchronize());
    OK(cudaMemcpy(d1, m, 256 * sizeof(int), cudaMemcpyDefault)); OK(cudaMemcpy(h1, d1, 256 * sizeof(int), cudaMemcpyDefault));
    sum = 0; for (int i = 0; i < 256; i++) sum += m[i] + h1[i];
    printf("managed %ld\n", sum);

    OK(cudaMemGetInfo(&fr, &tot)); printf("memory: %s\n", fr > 0 && fr <= tot ? "some of it is free" : "?");
    OK(cudaDeviceGetAttribute(&at, cudaDevAttrMultiProcessorCount, 0)); printf("processors: %s\n", at > 0 ? "some" : "none");
    OK(cudaGetDeviceProperties(&pr, 0));
    printf("limits: %d threads a block, %d x %d x %d, warp %d, %s\n", pr.maxThreadsPerBlock, pr.maxThreadsDim[0], pr.maxThreadsDim[1], pr.maxThreadsDim[2],
           pr.warpSize, pr.sharedMemPerBlock >= 48 * 1024 && pr.major >= 8 ? "48 KB shared" : "?");
    OK(cudaOccupancyMaxActiveBlocksPerMultiprocessor(&nb, scale, 256, 0)); printf("occupancy: %s\n", nb > 0 ? "some" : "none");
    OK(cudaEventDestroy(e1)); OK(cudaEventDestroy(e2)); OK(cudaEventDestroy(e3)); OK(cudaStreamDestroy(s1)); OK(cudaStreamDestroy(s2));
    OK(cudaFree(d1)); OK(cudaFree(d2)); OK(cudaFree(d3)); OK(cudaFree(m)); OK(cudaFreeHost(h1)); OK(cudaFreeHost(h2));
    e = cudaFree(0); printf("free of nothing: %s\n", cudaGetErrorString(e));
    return 0;
}
