/* The C++ that CUDA programs are written in, in kernels and around them: templates over types and numbers
   (kernels too), if constexpr, lambdas that capture by reference and by value, auto, references to arrays,
   records passed to a kernel by value, the vector types, constants, overloads and default arguments, an
   unnamed namespace, goto, unions, locks. Integers, and floats that hold integers: one right answer. */
#include <stdio.h>
#include <string.h>
#include <type_traits>
#ifdef __linux__   /* mcc's own library has threads on Linux only */
#include <mutex>
#endif
#include <cuda_runtime.h>
#include <cuda_fp16.h>

enum { N = 1000 };
typedef struct { const int *src[3]; int *dst; long long n[3]; int mul[3]; float2 f; } job_t;
struct cur_t { int p, w, nzs; };
template <int V> struct num_c { static constexpr int value = V; };   /* a number as a type: a tag for generic lambdas */

namespace {
constexpr int TAPS = 5;
__host__ __device__ constexpr int sq(int x) { return x * x; }
__constant__ int c_tab[8] = {3, 1, 4, 1, 5, 9, 2, 6};
__device__ const float d_w[TAPS] = {1, 4, 6, 4, 1};
}   // namespace

template <typename T> __host__ __device__ __forceinline__ T twice(T v) { return v + v; }
__host__ __device__ __forceinline__ int pick(int a, int b = 7, int c = 100) { return a * c + b; }
__host__ __device__ __forceinline__ float pick(float a) { return a * 0.5f; }
template <typename T> __device__ __forceinline__ float as_f(T v) {
    if constexpr (std::is_same<T, __half>::value) return __half2float(v) + 1000.0f;
    else if constexpr (std::is_same<T, int>::value) return (float)(v * 2);
    else return (float)v;
}
template <int R, int NS> __device__ __forceinline__ int ring(const int *p, int i) {
    static_assert((NS & (NS - 1)) == 0, "a power of two");
    int a = 0;
#pragma unroll
    for (int k = -R; k <= R; k++) a += p[(i + k) & (NS - 1)] * (R + 1 - (k < 0 ? -k : k));
    return a;
}
__device__ __forceinline__ void rot(unsigned (&r)[2][3], unsigned (&s)[2], int by) {
    for (int j = 0; j < 2; j++) { for (int k = 0; k < 3; k++) r[j][k] = r[j][k] << by | r[j][k] >> (32 - by); s[j] ^= r[j][0]; }
}

__global__ void k_jobs(job_t q)
{
    const int k = blockIdx.y; const long long n = q.n[k];
    for (long long i = blockIdx.x * (long long)blockDim.x + threadIdx.x; i < n; i += (long long)gridDim.x * blockDim.x)
        atomicAdd(&q.dst[i % 64], q.src[k][i] * q.mul[k] + (int)(q.f.x - q.f.y) + c_tab[i & 7]);
}

template <typename T, int MODE> __global__ void k_tmpl(const T *__restrict__ in, float *__restrict__ out, int n)
{
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float v = as_f(in[i]);
    if constexpr (MODE == 0) v = twice(v);
    else if constexpr (MODE == 1) { const float4 q = make_float4(v, 1, 2, 3); v = q.x * q.w + q.y - q.z; }
    else {
        auto step = [&](float x, auto tag) { constexpr int STEP = decltype(tag)::value; return x * (float)sq(STEP) + d_w[i % TAPS]; };
        v = step(v, num_c<MODE + 1>{});
    }
    out[i] = v + (float)pick(i % 5) + pick((float)(i % 4 * 2));
}

__global__ void k_lambda(const int *in, int *out, int n)
{
    __shared__ int s[256];
    const int t = threadIdx.x, base = blockIdx.x * 256;
    int calls = 0;
    auto at = [&](int i) { calls++; return base + i < n ? in[base + i] : 0; };
    auto mix = [=](int a, int b) { return a * 3 - b + t; };
    cur_t c = {0, 1, 3};
    auto adv = [&](cur_t &x) { if (++x.p > x.nzs) { x.p = 0; x.w += 2; } };
    unsigned r[2][3] = {{1u + t, 2, 3}, {4, 5u * t, 6}}, sg[2] = {0, 0};
    union { float f; unsigned u; int2 i2; } un;
    s[t] = at(t);
    __syncthreads();
    int a = ring<2, 256>(s, t);
    for (int j = 0; j < 7; j++) adv(c);
    rot(r, sg, t % 31 + 1);
    un.i2 = make_int2(t, a); un.f = 2.0f;
    int tries = 0;
again:
    a = mix(a, at((t * 7 + tries) % 256));
    if (++tries < 3) goto again;
    switch (t & 3) { case 0: a += 11; break; case 1: a ^= 5; /* falls through */ case 2: a -= c.w * 10 + c.p; break; default: a = -a; }
    if (base + t < n) out[base + t] = a + calls * 1000 + (int)(sg[0] % 97) + (int)(sg[1] % 89) + (int)(un.u >> 28) + un.i2.y;
}

static int g_total;
#ifdef __linux__
static std::mutex g_mu; static thread_local int g_calls;
static int locked(int v) { std::lock_guard<std::mutex> lk(g_mu); g_calls++; g_total += v; return g_total; }
#else
static int g_calls;
static int locked(int v) { g_calls++; g_total += v; return g_total; }
#endif
template <class T, int K> static long weigh(const T *p, int n) { long s = 0; for (int i = 0; i < n; i++) s += (long)p[i] * (i % K + 1); return s; }

int main(void)
{
    static int h[3][N], hd[64], hl[N]; static float hf[N], ho[3][N]; static __half hh[N];
    int *d[3], *dd, *dl, *dlo; float *df, *dout; __half *dh; job_t q; int *nul = nullptr;
    for (int k = 0; k < 3; k++) for (int i = 0; i < N; i++) h[k][i] = (i * (k + 3)) % 23 - 11;
    for (int i = 0; i < N; i++) { hf[i] = (float)(i % 17 - 8); hh[i] = __float2half((float)(i % 9 - 4)); }
    for (int k = 0; k < 3; k++) { cudaMalloc(&d[k], sizeof h[0]); cudaMemcpy(d[k], h[k], sizeof h[0], cudaMemcpyHostToDevice); q.src[k] = d[k]; q.n[k] = N - 100 * k; q.mul[k] = k * 2 - 1; }
    cudaMalloc(&dd, sizeof hd); cudaMemset(dd, 0, sizeof hd); cudaMalloc(&df, sizeof hf); cudaMalloc(&dh, sizeof hh); cudaMalloc(&dout, sizeof ho[0]);
    cudaMalloc(&dl, sizeof hl); cudaMalloc(&dlo, sizeof hl);
    cudaMemcpy(df, hf, sizeof hf, cudaMemcpyHostToDevice); cudaMemcpy(dh, hh, sizeof hh, cudaMemcpyHostToDevice);
    q.dst = dd; q.f = make_float2(9.0f, 2.0f);
    k_jobs<<<dim3(3, 3), 64>>>(q);
    cudaMemcpy(hd, dd, sizeof hd, cudaMemcpyDeviceToHost);
    printf("jobs %ld\n", weigh<int, 7>(hd, 64));
    k_tmpl<float, 0><<<(N + 127) / 128, 128>>>(df, dout, N); cudaMemcpy(ho[0], dout, sizeof ho[0], cudaMemcpyDeviceToHost);
    k_tmpl<int, 1><<<(N + 127) / 128, 128>>>(d[1], dout, N); cudaMemcpy(ho[1], dout, sizeof ho[0], cudaMemcpyDeviceToHost);
    k_tmpl<__half, 2><<<(N + 127) / 128, 128>>>(dh, dout, N); cudaMemcpy(ho[2], dout, sizeof ho[0], cudaMemcpyDeviceToHost);
    for (int k = 0; k < 3; k++) { double s = 0; for (int i = 0; i < N; i++) s += (double)ho[k][i] * (i % 11 + 1); printf("template %d: %.1f (%g %g %g)\n", k, s, ho[k][0], ho[k][1], ho[k][N - 1]); }
    cudaMemcpy(dl, h[2], sizeof hl, cudaMemcpyHostToDevice);
    k_lambda<<<(N + 255) / 256, 256>>>(dl, dlo, N); cudaMemcpy(hl, dlo, sizeof hl, cudaMemcpyDeviceToHost);
    printf("lambda %ld (%d %d %d)\n", weigh<int, 13>(hl, N), hl[0], hl[1], hl[N - 1]);
    for (int i = 0; i < 5; i++) locked(twice(i) + pick(i, i));
    printf("host %d %d %d %d %g %d\n", g_total, g_calls, sq(TAPS), pick(1), (double)pick(3.0f), nul == nullptr);
    if (cudaDeviceSynchronize() != cudaSuccess || cudaGetLastError() != cudaSuccess) printf("failed\n");
    return 0;
}
