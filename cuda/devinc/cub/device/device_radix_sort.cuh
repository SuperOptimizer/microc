/* mcc CUDA: cub::DeviceRadixSort, as far as it is used: sorted on the host, the operands copied from the
   device and the result back (which is no copy at all where the host shares its memory with the device). */
#ifndef MCUDA_CUB_SORT
#define MCUDA_CUB_SORT
#include <cuda_runtime.h>
#include <stdlib.h>
extern "C" int __mcuda_sort_pairs_bits(const void *keys_in, void *keys_out, int key_bytes, const void *vals_in, void *vals_out,
                                       int val_bytes, unsigned long n, int begin_bit, int end_bit, int descending);
namespace cub {
namespace DeviceRadixSort {
static inline cudaError_t __mcuda_sort(const void *keys_in, void *keys_out, size_t kb, const void *vals_in, void *vals_out, size_t vb, int n,
                                       int begin_bit, int end_bit, cudaStream_t stream, int descending) {
    cudaError_t e = cudaStreamSynchronize(stream);
    if (e != cudaSuccess || n <= 0) return e;
    char *h = (char *)malloc(2 * (size_t)n * (kb + vb));
    if (!h) return cudaErrorMemoryAllocation;
    char *ki = h, *ko = ki + (size_t)n * kb, *vi = ko + (size_t)n * kb, *vo = vi + (size_t)n * vb;
    e = cudaMemcpy(ki, keys_in, (size_t)n * kb, cudaMemcpyDeviceToHost);
    if (e == cudaSuccess) e = cudaMemcpy(vi, vals_in, (size_t)n * vb, cudaMemcpyDeviceToHost);
    if (e == cudaSuccess && __mcuda_sort_pairs_bits(ki, ko, (int)kb, vi, vo, (int)vb, (unsigned long)n, begin_bit, end_bit, descending)) e = cudaErrorUnknown;
    if (e == cudaSuccess) e = cudaMemcpy(keys_out, ko, (size_t)n * kb, cudaMemcpyHostToDevice);
    if (e == cudaSuccess) e = cudaMemcpy(vals_out, vo, (size_t)n * vb, cudaMemcpyHostToDevice);
    free(h);
    return e;
}
template <class K, class V> static inline cudaError_t SortPairs(void *tmp, size_t &tmp_bytes, const K *keys_in, K *keys_out, const V *vals_in, V *vals_out,
                                                                int n, int begin_bit = 0, int end_bit = sizeof(K) * 8, cudaStream_t stream = 0) {
    if (!tmp) { tmp_bytes = 1; return cudaSuccess; }
    return __mcuda_sort(keys_in, keys_out, sizeof(K), vals_in, vals_out, sizeof(V), n, begin_bit, end_bit, stream, 0);
}
template <class K, class V> static inline cudaError_t SortPairsDescending(void *tmp, size_t &tmp_bytes, const K *keys_in, K *keys_out, const V *vals_in, V *vals_out,
                                                                          int n, int begin_bit = 0, int end_bit = sizeof(K) * 8, cudaStream_t stream = 0) {
    if (!tmp) { tmp_bytes = 1; return cudaSuccess; }
    return __mcuda_sort(keys_in, keys_out, sizeof(K), vals_in, vals_out, sizeof(V), n, begin_bit, end_bit, stream, 1);
}
}
}
#endif
