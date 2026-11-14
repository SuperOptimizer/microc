/* Which products and sums a compiler fuses into one operation (nvcc's -fmad, on by default) shows in the last bit
   of the results. Every shape here is worked out on the device for 256 inputs and its results' bits are summed
   up; the sums expected are those of nvcc's code on a real GPU, so mcc has to fuse what LLVM fuses and nothing
   else. A shape that differs is named by its number. (Not covered: a product and its sum in different
   blocks, which nvcc fuses where it turns the branch into a selection.) */
#include <stdio.h>
#include <string.h>
#include <cuda_runtime.h>

typedef unsigned long long u64;
#define N 256
#define NS 36

/* each shape is a function of its own, not inlined: what is fused then depends on the shape alone */
#define SHAPES(T, P) \
    __device__ __noinline__ T P##0(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return a * b + c; } \
    __device__ __noinline__ T P##1(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return c + a * b; } \
    __device__ __noinline__ T P##2(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return a * b - c; } \
    __device__ __noinline__ T P##3(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return c - a * b; } \
    __device__ __noinline__ T P##4(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return a * b + c * d; } \
    __device__ __noinline__ T P##5(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return a * b - c * d; } \
    __device__ __noinline__ T P##6(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return (a * b + c) + d * e; } \
    __device__ __noinline__ T P##7(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; t = a * b; return (t + c) + t * d; } \
    __device__ __noinline__ T P##8(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; t = a * b; return (t + c) * (t - d); } \
    __device__ __noinline__ T P##9(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return -(a * b) + c; } \
    __device__ __noinline__ T P##10(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return -(a * b) - c; } \
    __device__ __noinline__ T P##11(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; t = a * b; if (flag) t += c; return t; } \
    __device__ __noinline__ T P##12(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; r = 0; for (int i = 0; i < 8; i++) r += x[i] * y[i]; return r; } \
    __device__ __noinline__ T P##13(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return a * a - b * b; } \
    __device__ __noinline__ T P##14(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return (a + b) * c + d; } \
    __device__ __noinline__ T P##15(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return a * (b + c); } \
    __device__ __noinline__ T P##16(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return a * b * c + d; } \
    __device__ __noinline__ T P##17(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return (T)((double)(a * b) + c); } \
    __device__ __noinline__ T P##18(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; r = a; r *= b; r += c; return r; } \
    __device__ __noinline__ T P##19(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return (flag ? a * b : c) + d; } \
    __device__ __noinline__ T P##20(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return a * b + (T)1; } \
    __device__ __noinline__ T P##21(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return a * (T)2 + c; } \
    __device__ __noinline__ T P##22(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return a / b + c; } \
    __device__ __noinline__ T P##23(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return (a * b < 0 ? -(a * b) : a * b) + c; } \
    __device__ __noinline__ T P##24(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return a * b + c * c; } \
    __device__ __noinline__ T P##25(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return a * b + c * d + e; } \
    __device__ __noinline__ T P##26(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return a * b + (c * d + e); } \
    __device__ __noinline__ T P##27(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return (a * b + c * d) + (d * e + a * c); } \
    __device__ __noinline__ T P##28(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; r = c; for (int i = 0; i < 8; i++) r = r * x[i] + y[i]; return r; } \
    __device__ __noinline__ T P##29(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return (-a) * b + c; } \
    __device__ __noinline__ T P##30(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return c - (a * b + d); } \
    __device__ __noinline__ T P##31(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return (a * b - c) - d * e; } \
    __device__ __noinline__ T P##32(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; t = a * b; x2 = t + c; return x2 * t; } \
    __device__ __noinline__ T P##33(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return a * b + c * d - e * a; } \
    __device__ __noinline__ T P##34(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return (a - b) * (a + b) + c; } \
    __device__ __noinline__ T P##35(T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { T t = 0, r = 0, x2 = 0; (void)t; (void)r; (void)x2; (void)flag; (void)x; (void)y; (void)d; (void)e; (void)c; return e + (a * b + c * d); } \
    __device__ T P(int s, T a, T b, T c, T d, T e, int flag, const T *x, const T *y) { switch (s) { \
        case 0: return P##0(a, b, c, d, e, flag, x, y); \
        case 1: return P##1(a, b, c, d, e, flag, x, y); \
        case 2: return P##2(a, b, c, d, e, flag, x, y); \
        case 3: return P##3(a, b, c, d, e, flag, x, y); \
        case 4: return P##4(a, b, c, d, e, flag, x, y); \
        case 5: return P##5(a, b, c, d, e, flag, x, y); \
        case 6: return P##6(a, b, c, d, e, flag, x, y); \
        case 7: return P##7(a, b, c, d, e, flag, x, y); \
        case 8: return P##8(a, b, c, d, e, flag, x, y); \
        case 9: return P##9(a, b, c, d, e, flag, x, y); \
        case 10: return P##10(a, b, c, d, e, flag, x, y); \
        case 11: return P##11(a, b, c, d, e, flag, x, y); \
        case 12: return P##12(a, b, c, d, e, flag, x, y); \
        case 13: return P##13(a, b, c, d, e, flag, x, y); \
        case 14: return P##14(a, b, c, d, e, flag, x, y); \
        case 15: return P##15(a, b, c, d, e, flag, x, y); \
        case 16: return P##16(a, b, c, d, e, flag, x, y); \
        case 17: return P##17(a, b, c, d, e, flag, x, y); \
        case 18: return P##18(a, b, c, d, e, flag, x, y); \
        case 19: return P##19(a, b, c, d, e, flag, x, y); \
        case 20: return P##20(a, b, c, d, e, flag, x, y); \
        case 21: return P##21(a, b, c, d, e, flag, x, y); \
        case 22: return P##22(a, b, c, d, e, flag, x, y); \
        case 23: return P##23(a, b, c, d, e, flag, x, y); \
        case 24: return P##24(a, b, c, d, e, flag, x, y); \
        case 25: return P##25(a, b, c, d, e, flag, x, y); \
        case 26: return P##26(a, b, c, d, e, flag, x, y); \
        case 27: return P##27(a, b, c, d, e, flag, x, y); \
        case 28: return P##28(a, b, c, d, e, flag, x, y); \
        case 29: return P##29(a, b, c, d, e, flag, x, y); \
        case 30: return P##30(a, b, c, d, e, flag, x, y); \
        case 31: return P##31(a, b, c, d, e, flag, x, y); \
        case 32: return P##32(a, b, c, d, e, flag, x, y); \
        case 33: return P##33(a, b, c, d, e, flag, x, y); \
        case 34: return P##34(a, b, c, d, e, flag, x, y); \
        case 35: return P##35(a, b, c, d, e, flag, x, y); \
        default: return 0; } }
SHAPES(float, sf)
SHAPES(double, sd)

__global__ void run(const float *in, u64 *sums)
{
    int s = blockIdx.x, i = threadIdx.x;
    const float *p = in + 24 * i;
    double q[24];
    for (int k = 0; k < 24; k++) q[k] = (double)p[k] * (1.0 + 1e-7 * (k + 1));
    float f = sf(s, p[0], p[1], p[2], p[3], p[4], i & 1, p + 5, p + 13);
    double g = sd(s, q[0], q[1], q[2], q[3], q[4], i & 1, q + 5, q + 13);
    unsigned fb; u64 gb;
    memcpy(&fb, &f, 4);
    memcpy(&gb, &g, 8);
    atomicAdd((unsigned long long *)&sums[2 * s], (unsigned long long)fb * (2 * i + 1));
    atomicAdd((unsigned long long *)&sums[2 * s + 1], (unsigned long long)(gb * (2 * i + 1)));
}

int main(void)
{
    static float h[24 * N];
    float *in; u64 *sums, r = 88172645463325252ULL;
    static u64 hs[2 * NS];
    for (int i = 0; i < 24 * N; i++) {
        r ^= r << 13; r ^= r >> 7; r ^= r << 17;
        h[i] = (float)((long long)(r % 2000001) - 1000000) / 7919.0f;
    }
    cudaMalloc(&in, sizeof h);
    cudaMalloc(&sums, sizeof hs);
    cudaMemcpy(in, h, sizeof h, cudaMemcpyHostToDevice);
    cudaMemset(sums, 0, sizeof hs);
    run<<<NS, N>>>(in, sums);
    if (cudaDeviceSynchronize()) { printf("launch failed\n"); return 1; }
    cudaMemcpy(hs, sums, sizeof hs, cudaMemcpyDeviceToHost);
    for (int s = 0; s < NS; s++) printf("%2d %016llx %016llx\n", s, hs[2 * s], hs[2 * s + 1]);
    return 0;
}
