/* mcc CUDA: cub::DeviceScan, as far as it is used: computed on the host, the operands copied from the device
   and the result back (which is no copy at all where the host shares its memory with the device). */
#ifndef MCUDA_CUB_SCAN
#define MCUDA_CUB_SCAN
#include <cuda_runtime.h>
#include <stdlib.h>
namespace cub {
namespace DeviceScan {
template <class T> static inline cudaError_t __mcuda_sum(const T *in, T *out, int n, cudaStream_t stream, int inclusive) {
    cudaError_t e = cudaStreamSynchronize(stream);
    if (e != cudaSuccess || n <= 0) return e;
    T *h = (T *)malloc((size_t)n * sizeof(T));
    if (!h) return cudaErrorMemoryAllocation;
    e = cudaMemcpy(h, in, (size_t)n * sizeof(T), cudaMemcpyDeviceToHost);
    if (e == cudaSuccess) {
        T acc = 0;
        if (inclusive) for (int i = 0; i < n; i++) { acc += h[i]; h[i] = acc; }
        else for (int i = 0; i < n; i++) { const T v = h[i]; h[i] = acc; acc += v; }
        e = cudaMemcpy(out, h, (size_t)n * sizeof(T), cudaMemcpyHostToDevice);
    }
    free(h);
    return e;
}
template <class T> static inline cudaError_t ExclusiveSum(void *tmp, size_t &tmp_bytes, const T *in, T *out, int n, cudaStream_t stream = 0) {
    if (!tmp) { tmp_bytes = 1; return cudaSuccess; }
    return __mcuda_sum(in, out, n, stream, 0);
}
template <class T> static inline cudaError_t InclusiveSum(void *tmp, size_t &tmp_bytes, const T *in, T *out, int n, cudaStream_t stream = 0) {
    if (!tmp) { tmp_bytes = 1; return cudaSuccess; }
    return __mcuda_sum(in, out, n, stream, 1);
}
}
}
#endif
