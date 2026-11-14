/* cub on device memory: the radix sort of pairs (all of the key and a range of its bits, ascending and
   descending, into other arrays, which must leave the inputs alone) and the prefix sums, between kernels
   that make the input and check the output. Integers: one right answer. */
#include <stdio.h>
#include <stdlib.h>
#include <cuda_runtime.h>
#include <cub/device/device_radix_sort.cuh>
#include <cub/device/device_scan.cuh>

static const int N = 5000;

__global__ void k_fill(unsigned long long *k, float *v, unsigned *k32, unsigned *v32, int *cnt, int n)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    unsigned h = (unsigned)i * 2654435761u;
    h ^= h >> 13;
    k[i] = ((unsigned long long)(h % 97) << 32) | (h & 0xffff);
    v[i] = (float)i;
    k32[i] = h % 1009;
    v32[i] = (unsigned)i;
    cnt[i] = (int)(h % 7);
}

/* the number of places where the order is wrong, or where equal keys are not in the order of the input */
__global__ void k_check(const unsigned long long *k, const float *v, int n, int lo, int hi, int desc, unsigned *bad)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i + 1 >= n) return;
    unsigned long long m = hi - lo == 64 ? ~0ull : ((1ull << (hi - lo)) - 1);
    unsigned long long a = (k[i] >> lo) & m, b = (k[i + 1] >> lo) & m;
    if (desc ? a < b : a > b) atomicAdd(bad, 1u);
    if (a == b && v[i] > v[i + 1]) atomicAdd(bad, 1u);
}

static unsigned long long sum_k(const unsigned long long *d, int n, unsigned long long *h)
{
    unsigned long long s = 0;
    cudaMemcpy(h, d, (size_t)n * 8, cudaMemcpyDeviceToHost);
    for (int i = 0; i < n; i++) s += h[i] * (unsigned long long)(i + 1);
    return s;
}

int main(void)
{
    unsigned long long *k, *ko, *hk = (unsigned long long *)malloc(N * 8);
    float *v, *vo;
    unsigned *k32, *v32, *bad, hb = 0, *h32 = (unsigned *)malloc(N * 4);
    int *cnt, *off, *hi = (int *)malloc(N * 4);
    void *tmp;
    size_t tb = 0;
    cudaMalloc(&k, N * 8); cudaMalloc(&ko, N * 8); cudaMalloc(&v, N * 4); cudaMalloc(&vo, N * 4);
    cudaMalloc(&k32, N * 4); cudaMalloc(&v32, N * 4); cudaMalloc(&bad, 4); cudaMalloc(&cnt, N * 4); cudaMalloc(&off, N * 4);
    k_fill<<<(N + 255) / 256, 256>>>(k, v, k32, v32, cnt, N);
    unsigned long long before = sum_k(k, N, hk);

    cub::DeviceRadixSort::SortPairs(nullptr, tb, k, ko, v, vo, N, 0, 64);
    cudaMalloc(&tmp, tb + 16);
    cub::DeviceRadixSort::SortPairs(tmp, tb, k, ko, v, vo, N, 0, 64);
    cudaMemset(bad, 0, 4);
    k_check<<<(N + 255) / 256, 256>>>(ko, vo, N, 0, 64, 0, bad);
    cudaMemcpy(&hb, bad, 4, cudaMemcpyDeviceToHost);
    printf("pairs: bad %u, input kept %d, sum %llu\n", hb, sum_k(k, N, hk) == before, sum_k(ko, N, hk));

    cub::DeviceRadixSort::SortPairs(tmp, tb, k, ko, v, vo, N, 32, 40);
    cudaMemset(bad, 0, 4);
    k_check<<<(N + 255) / 256, 256>>>(ko, vo, N, 32, 40, 0, bad);
    cudaMemcpy(&hb, bad, 4, cudaMemcpyDeviceToHost);
    printf("bits 32..40: bad %u, sum %llu\n", hb, sum_k(ko, N, hk));

    cub::DeviceRadixSort::SortPairsDescending(tmp, tb, k, ko, v, vo, N, 0, 64);
    cudaMemset(bad, 0, 4);
    k_check<<<(N + 255) / 256, 256>>>(ko, vo, N, 0, 64, 1, bad);
    cudaMemcpy(&hb, bad, 4, cudaMemcpyDeviceToHost);
    printf("descending: bad %u, sum %llu\n", hb, sum_k(ko, N, hk));

    cudaFree(tmp); tb = 0;
    unsigned *k32o, *v32o;
    cudaMalloc(&k32o, N * 4); cudaMalloc(&v32o, N * 4);
    cub::DeviceRadixSort::SortPairs(nullptr, tb, k32, k32o, v32, v32o, N, 0, 32);
    cudaMalloc(&tmp, tb + 16);
    cub::DeviceRadixSort::SortPairs(tmp, tb, k32, k32o, v32, v32o, N, 0, 32);
    cudaMemcpy(h32, v32o, N * 4, cudaMemcpyDeviceToHost);
    unsigned long long s = 0;
    for (int i = 0; i < N; i++) s += (unsigned long long)h32[i] * (unsigned)(i + 1);
    printf("u32 pairs: values %llu\n", s);

    cudaFree(tmp); tb = 0;
    cub::DeviceScan::ExclusiveSum(nullptr, tb, cnt, off, N);
    cudaMalloc(&tmp, tb + 16);
    cub::DeviceScan::ExclusiveSum(tmp, tb, cnt, off, N);
    cudaMemcpy(hi, off, N * 4, cudaMemcpyDeviceToHost);
    long long e = 0;
    for (int i = 0; i < N; i++) e += hi[i] % 1000;
    printf("exclusive: last %d, mix %lld\n", hi[N - 1], e);
    cub::DeviceScan::InclusiveSum(tmp, tb, cnt, off, N);
    cudaMemcpy(hi, off, N * 4, cudaMemcpyDeviceToHost);
    e = 0;
    for (int i = 0; i < N; i++) e += hi[i] % 1000;
    printf("inclusive: last %d, mix %lld\n", hi[N - 1], e);
    if (cudaDeviceSynchronize() != cudaSuccess || cudaGetLastError() != cudaSuccess) printf("failed\n");
    return 0;
}
