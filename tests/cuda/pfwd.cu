/* Kernel records read as values (opt_param_fwd), #pragma unroll N as a factor, loops whose way back makes its moves
   before the branch (ptx.c moves_early): each kernel's results against the same code run on the host. */
#include <stdio.h>
#include <stdint.h>
#include <cuda_runtime.h>
typedef struct { const int *p; signed char c; short s; unsigned char u; int i; float f; long long l; int ns; unsigned short us; } rec_t;
typedef struct { rec_t r; int k; } outer_t;

__host__ __device__ __forceinline__ static int fields(const rec_t &q, int t) {
    return q.c * 3 + q.s * 5 + q.u * 7 + q.i * 11 + (int)(q.f * 4.f) + (int)(q.l % 1000) * 13 + q.ns + q.us * 17 + (q.p ? q.p[t & 3] : -1);
}
__host__ __device__ static int unrolled(int t, int n) {
    int s = 0, a[12] = {0};
#pragma unroll 3
    for (int k = 0; k < 10; k++) { if (k == t % 7) continue; s += k * k + t; a[k] = s; }
#pragma unroll 4
    for (int k = 17; k > 0; k -= 2) { if (k == 2 * (t % 5) + 1) break; s = s * 7 + k; }
#pragma unroll 2
    for (unsigned k = 0; k < 7u; k++) for (int m = 0; m < 3; m++) s += (int)k * m + t;
#pragma unroll 1
    for (int k = 0; k < n; k++) s ^= k << (t & 7);
#pragma unroll 2
    for (int k = 0; k < 4; k++) s += a[k + 3] * (k + 1);
    return s;
}
__host__ __device__ static int carried(int t, int n) {
    int a = 1 + t, b = 0, prev = 0, sum = 0;
    for (int k = 0; k < n; k++) { prev = a; int x = a + b; b = a; a = x & 0xffff; sum += prev; }
    unsigned u = t, v = 1;
    for (int k = n; k > 0; k--) { unsigned w = u * 3 + v; v = u; u = w; if (w == 77u) break; }
    return prev * 7 + a * 3 + b + sum + (int)(u ^ v);
}
__global__ void k_rec(int *out, rec_t r1, int n, rec_t r2, outer_t o, int flag) {
    int t = threadIdx.x;
    if (t >= n) return;
    const rec_t &q = r1.ns ? r1 : r2; /* (as gn_row(gp.ns ? gp : sp.gp2, ...)) */
    rec_t z = {};
    const rec_t &w = (t & flag) ? o.r : z;
    out[t] = fields(q, t) * 3 + fields(w, t) + o.k + fields(r2, t + 1) * 5;
}
__global__ void k_loops(int *out, int n, int m) {
    int t = threadIdx.x;
    if (t < n) out[t] = unrolled(t, m) + carried(t, m + (t & 3));
}
int main(void) {
    enum { N = 32 };
    int *out, *p, bad = 0;
    long long sum = 0;
    cudaMallocManaged(&out, N * 4); cudaMallocManaged(&p, 16);
    for (int i = 0; i < 4; i++) p[i] = 100 + i * i;
    rec_t r1 = {p, -5, -300, 200, 123456, 2.75f, -1234567LL, 0, 60000};
    rec_t r2 = {p + 1, 7, 1234, 9, -77, -1.5f, 99999LL, 3, 5};
    outer_t o = {{p, 1, 2, 3, 4, 5.f, 6LL, 7, 8}, 42};
    k_rec<<<1, N>>>(out, r1, N, r2, o, 2);
    printf("%d\n", (int)cudaDeviceSynchronize());
    for (int t = 0; t < N; t++) {
        const rec_t &q = r1.ns ? r1 : r2;
        rec_t z = {};
        const rec_t &w = (t & 2) ? o.r : z;
        int want = fields(q, t) * 3 + fields(w, t) + o.k + fields(r2, t + 1) * 5;
        bad += out[t] != want;
        sum += out[t];
    }
    r1.ns = 1;
    k_rec<<<1, N>>>(out, r1, N, r2, o, 1);
    printf("%d\n", (int)cudaDeviceSynchronize());
    for (int t = 0; t < N; t++) {
        rec_t z = {};
        const rec_t &w = (t & 1) ? o.r : z;
        int want = fields(r1, t) * 3 + fields(w, t) + o.k + fields(r2, t + 1) * 5;
        bad += out[t] != want;
        sum += out[t];
    }
    k_loops<<<1, N>>>(out, N, 9);
    printf("%d\n", (int)cudaDeviceSynchronize());
    for (int t = 0; t < N; t++) {
        int want = unrolled(t, 9) + carried(t, 9 + (t & 3));
        bad += out[t] != want;
        sum += out[t];
    }
    printf("%lld %d\n", sum, bad);
    return 0;
}
