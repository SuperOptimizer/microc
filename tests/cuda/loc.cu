/* Locals that the compiler keeps in a function's memory, in the shapes a run time on a real device turns
   into registers (cuda/hw/mod.c, locals) and the shapes it must leave alone: a value seen as another type,
   arrays and records read and written at constant places, a double seen as two words, a place read through
   a pointer with a variable index, the address of a local given to a function. Integers and bits: one
   right answer. */
#include <stdio.h>
#include <cuda_runtime.h>

struct rec { int a; float f; unsigned u[2]; double d; };
__device__ __noinline__ void bump(int *p, int by) { *p += by; }
__device__ __forceinline__ unsigned amax_u(unsigned a, float v) { unsigned u = __float_as_uint(v) & 0x7fffffffu; return a > u ? a : u; }

/* constant places only */
__global__ void k_const(const float *x, unsigned *y, int n)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    float v[8], acc[2][2]; unsigned amu = 0u; struct rec r; long long q[2];
    if (i >= n) return;
    v[0] = x[i]; v[1] = x[(i + 1) % n]; v[2] = v[0] * 2.0f; v[3] = v[1] - v[0]; v[4] = -v[2]; v[5] = v[3] * 4.0f; v[6] = v[4] + v[5]; v[7] = 0.5f;
    acc[0][0] = acc[0][1] = acc[1][0] = acc[1][1] = 0.0f;
    for (int it = 0; it < 3; it++) {
        amu = amax_u(amu, v[0]); amu = amax_u(amu, v[3]); amu = amax_u(amu, v[6]);
        acc[0][0] += v[0] + v[1]; acc[0][1] += v[2] * v[7]; acc[1][0] += v[4]; acc[1][1] += v[5] + v[6];
        v[0] = v[0] + 1.0f; v[6] = v[6] - 2.0f;
    }
    r.a = i * 3; r.f = acc[0][0] + acc[1][1]; r.u[0] = __float_as_uint(acc[0][1]); r.u[1] = amu; r.d = (double)acc[1][0] * 0.25;
    q[0] = __double_as_longlong(r.d); q[1] = (long long)r.a << 33 | r.u[1];
    y[i] = (unsigned)r.a ^ __float_as_uint(r.f) ^ r.u[0] * 3u ^ r.u[1] * 5u ^ (unsigned)(q[0] >> 32) ^ (unsigned)q[0] * 7u ^ (unsigned)(q[1] >> 30) ^ __float_as_uint(__uint_as_float(amu) * 2.0f);
}

/* a double written whole and read as two words, words written and read whole, bytes of a word */
__global__ void k_mixed(const float *x, unsigned *y, int n)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    union { double d; unsigned w[2]; unsigned char b[8]; long long l; } u;
    union { float f; unsigned char b[4]; unsigned w; } s;
    if (i >= n) return;
    u.d = (double)x[i] * 1.5 + 3.0;
    unsigned lo = u.w[0], hi = u.w[1];
    u.w[0] = hi ^ 0x55u; u.w[1] = lo + 7u;
    long long l = u.l;
    s.f = x[i] + 100.0f; s.b[0] ^= 0xffu; s.b[3] += 1;
    y[i] = lo * 3u ^ hi ^ (unsigned)l * 5u ^ (unsigned)(l >> 32) ^ s.w * 11u ^ u.b[5];
}

/* an address that leaves: a variable index, a call */
__global__ void k_escape(const float *x, unsigned *y, int n)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    float v[8]; int c[4], t = 5; unsigned h = 0u;
    if (i >= n) return;
    for (int k = 0; k < 8; k++) v[k] = x[(i + k) % n] * (float)(k + 1);
    c[0] = i; c[1] = 2; c[2] = 3; c[3] = 4;
    bump(&c[1], i & 3); bump(&t, c[1]);
    for (int k = 0; k < 8; k++) h = h * 31u + __float_as_uint(v[(k + i) & 7]);
    v[3] = v[c[2]] + 1.0f; c[2] = t;
    y[i] = h ^ __float_as_uint(v[3]) ^ (unsigned)(c[0] + c[1] * 16 + c[2] * 256 + c[3] * 4096 + t * 65536);
}

int main(void)
{
    enum { N = 1000 };
    static float hx[N]; static unsigned hy[N]; float *dx; unsigned *dy; int pass, i;
    for (i = 0; i < N; i++) hx[i] = (float)((i * 37) % 201 - 100) * 0.125f;
    cudaMalloc(&dx, sizeof hx); cudaMalloc(&dy, sizeof hy); cudaMemcpy(dx, hx, sizeof hx, cudaMemcpyHostToDevice);
    for (pass = 0; pass < 3; pass++) {
        unsigned long long s = 0;
        cudaMemset(dy, 0, sizeof hy);
        if (pass == 0) k_const<<<(N + 127) / 128, 128>>>(dx, dy, N);
        else if (pass == 1) k_mixed<<<(N + 127) / 128, 128>>>(dx, dy, N);
        else k_escape<<<(N + 127) / 128, 128>>>(dx, dy, N);
        cudaMemcpy(hy, dy, sizeof hy, cudaMemcpyDeviceToHost);
        for (i = 0; i < N; i++) s = s * 1000003ull + hy[i];
        printf("%s %016llx %08x %08x %08x\n", pass == 0 ? "const" : pass == 1 ? "mixed" : "escape", s, hy[0], hy[1], hy[N - 1]);
    }
    if (cudaDeviceSynchronize() != cudaSuccess || cudaGetLastError() != cudaSuccess) printf("failed\n");
    return 0;
}
