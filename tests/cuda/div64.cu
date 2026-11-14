#include <cuda_runtime.h>
#include <stdio.h>
/* 64-bit division and remainder, with numbers that fit 32 bits and that do not, signed and not */
__global__ void k(unsigned long long *o, const unsigned long long *a, const long long *b, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    unsigned long long ua = a[i], ub = (unsigned long long)b[i];
    long long sa = (long long)a[i], sb = b[i];
    o[4 * i] = ua / ub;
    o[4 * i + 1] = ua % ub;
    o[4 * i + 2] = (unsigned long long)(sa / sb);
    o[4 * i + 3] = (unsigned long long)(sa % sb);
}
int main() {
    static const unsigned long long A[8] = { 100, 0xffffffffull, 0x100000000ull, 12345678901234ull, 0xfffffffffffffff0ull, 7, 0x123456789abull, 0 };
    static const long long B[8] = { 7, 3, 3, 1000003, 5, -2, 0x100000001ll, -9 };
    unsigned long long *o, *a, h = 0; long long *b; int bad = 0;
    cudaMallocManaged(&o, 32 * sizeof *o); cudaMallocManaged(&a, 8 * sizeof *a); cudaMallocManaged(&b, 8 * sizeof *b);
    for (int i = 0; i < 8; i++) { a[i] = A[i]; b[i] = B[i]; }
    k<<<1, 8>>>(o, a, b, 8); cudaDeviceSynchronize();
    for (int i = 0; i < 8; i++) {
        unsigned long long ub = (unsigned long long)B[i]; long long sa = (long long)A[i];
        bad += o[4 * i] != A[i] / ub; bad += o[4 * i + 1] != A[i] % ub;
        bad += o[4 * i + 2] != (unsigned long long)(sa / B[i]); bad += o[4 * i + 3] != (unsigned long long)(sa % B[i]);
        h = h * 31 + o[4 * i] + o[4 * i + 1] * 3 + o[4 * i + 2] * 5 + o[4 * i + 3] * 7;
    }
    printf("%d %llu\n", bad, h % 1000003);
    return 0;
}
