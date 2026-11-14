/* Tensor cores, written as programs write them: inline PTX in small wrappers. Each block is one warp that
   multiplies a 16 x K matrix by a K x 8 one; the entries are small integers, so every result is exact and
   does not depend on the order of the sums. mma of fp16 (fp32 and fp16 sums), bf16, tf32 and int8; ldmatrix,
   plain and transposed; asynchronous copies into shared memory, whole and zero filled. */
#include <stdio.h>
#include <cuda_runtime.h>
#include <cuda_fp16.h>

enum { NB = 5 };

__device__ __forceinline__ unsigned su32(const void *p) { return (unsigned)__cvta_generic_to_shared(p); }
__device__ __forceinline__ void cpa(void *s, const void *g, int bytes) {
    asm volatile("cp.async.cg.shared.global [%0], [%1], 16, %2;\n" :: "r"(su32(s)), "l"(g), "r"(bytes) : "memory");
}
__device__ __forceinline__ void cpa16(void *s, const void *g) {
    asm volatile("cp.async.cg.shared.global [%0], [%1], 16;\n" :: "r"(su32(s)), "l"(g));
}
__device__ __forceinline__ void cpa_commit() { asm volatile("cp.async.commit_group;\n" ::); }
template <int N> __device__ __forceinline__ void cpa_wait() { asm volatile("cp.async.wait_group %0;\n" :: "n"(N)); }
__device__ __forceinline__ void cpa_all() { asm volatile("cp.async.commit_group;\ncp.async.wait_group 0;\n" ::: "memory"); }

__device__ __forceinline__ void ldsm4(unsigned *r, const __half *p) {
    const unsigned a = su32(p);
    asm volatile("ldmatrix.sync.aligned.m8n8.x4.shared.b16 {%0,%1,%2,%3}, [%4];\n" : "=r"(r[0]), "=r"(r[1]), "=r"(r[2]), "=r"(r[3]) : "r"(a));
}
template <int OFF> __device__ __forceinline__ void ldsm4o(unsigned *r, const __half *p) {
    const unsigned a = su32(p);
    asm volatile("ldmatrix.sync.aligned.m8n8.x4.shared.b16 {%0,%1,%2,%3}, [%4+%5];\n" : "=r"(r[0]), "=r"(r[1]), "=r"(r[2]), "=r"(r[3]) : "r"(a), "n"(OFF));
}
__device__ __forceinline__ void ldsm4t(unsigned *r, const __half *p) {
    const unsigned a = su32(p);
    asm volatile("ldmatrix.sync.aligned.m8n8.x4.trans.shared.b16 {%0,%1,%2,%3}, [%4];\n" : "=r"(r[0]), "=r"(r[1]), "=r"(r[2]), "=r"(r[3]) : "r"(a));
}
__device__ __forceinline__ void mma16(float *c, const unsigned *a, unsigned b0, unsigned b1) {
    asm volatile("mma.sync.aligned.m16n8k16.row.col.f32.f16.f16.f32 {%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};\n"
                 : "+f"(c[0]), "+f"(c[1]), "+f"(c[2]), "+f"(c[3]) : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b0), "r"(b1));
}
__device__ __forceinline__ void mma16h(unsigned *c, const unsigned *a, const unsigned *b) {
    asm volatile("mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 {%0,%1}, {%2,%3,%4,%5}, {%6,%7}, {%0,%1};\n"
                 : "+r"(c[0]), "+r"(c[1]) : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b[0]), "r"(b[1]));
}
__device__ __forceinline__ void mma16b(float *c, const unsigned *a, const unsigned *b) {
    asm volatile("mma.sync.aligned.m16n8k16.row.col.f32.bf16.bf16.f32 {%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};\n"
                 : "+f"(c[0]), "+f"(c[1]), "+f"(c[2]), "+f"(c[3]) : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b[0]), "r"(b[1]));
}
__device__ __forceinline__ void mma8t(float *c, const unsigned *a, const unsigned *b) {
    asm volatile("mma.sync.aligned.m16n8k8.row.col.f32.tf32.tf32.f32 {%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3};\n"
                 : "+f"(c[0]), "+f"(c[1]), "+f"(c[2]), "+f"(c[3]) : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b[0]), "r"(b[1]));
}
/* the sums start from the bits of 1.5 2^23, so that read as floats they are 1.5 2^23 + the sum */
__device__ __forceinline__ void mma32s(float *d, unsigned a0, unsigned a1, unsigned a2, unsigned a3, unsigned b0, unsigned b1) {
    int r[4];
    asm("mma.sync.aligned.m16n8k32.row.col.s32.s8.s8.s32 {%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%10,%10,%10,%10};\n"
        : "=r"(r[0]), "=r"(r[1]), "=r"(r[2]), "=r"(r[3]) : "r"(a0), "r"(a1), "r"(a2), "r"(a3), "r"(b0), "r"(b1), "r"(0x4b400000));
#pragma unroll
    for (int e = 0; e < 4; e++) d[e] = __int_as_float(r[e]);
}
__device__ __forceinline__ unsigned pk(float a, float b) { __half2 h = __floats2half2_rn(a, b); return *(unsigned *)&h; }
__device__ __forceinline__ unsigned pkb(float a, float b) { return __float_as_uint(a) >> 16 | (__float_as_uint(b) >> 16) << 16; }
__device__ __forceinline__ unsigned pk8(const signed char *p, int step) {
    return (unsigned)(unsigned char)p[0] | (unsigned)(unsigned char)p[step] << 8 | (unsigned)(unsigned char)p[2 * step] << 16 | (unsigned)(unsigned char)p[3 * step] << 24;
}

/* A[16][16] and B[16][8] of fp16 come into shared memory by asynchronous copies, then into registers by ldmatrix.
   C = 2 A B: once from those, once from operands read straight from global memory. pad: what the copies of no
   bytes left behind. */
__global__ void k_f16(const __half *A, const __half *B, float *C, unsigned *pad)
{
    __shared__ __align__(16) __half sA[17][16];   /* the first row is not used: an address 16 bytes below a row is still inside */
    __shared__ __align__(16) __half sB[16][8];
    __shared__ __align__(16) unsigned sP[32][4];
    int l = threadIdx.x, g = l >> 2, t = l & 3;
    unsigned ra[4], rb[4], rc[4], ga[4]; float c[4] = {0, 0, 0, 0};
    A += blockIdx.x * 256; B += blockIdx.x * 128; C += blockIdx.x * 128;
    for (int k = 0; k < 4; k++) sP[l][k] = 0xdead0000u + l * 4 + k;
    __syncthreads();
    cpa16(&sA[1 + (l >> 1)][(l & 1) * 8], A + l * 8);
    cpa_commit();
    cpa(l < 16 ? (void *)sB[l] : (void *)sP[l], B + (l & 15) * 8, l < 16 ? 16 : 0);
    cpa_commit();
    cpa_wait<1>();
    cpa_all();
    __syncthreads();
    ldsm4(ra, &sA[1 + (l & 15)][(l >> 4) * 8]);
    ldsm4t(rb, sB[l & 15]);
    ldsm4o<16>(rc, &sA[1 + (l & 15)][(l >> 4) * 8] - 8);
    mma16(c, ra, rb[0], rb[1]);
    ga[0] = *(const unsigned *)(A + g * 16 + 2 * t);     ga[1] = *(const unsigned *)(A + (g + 8) * 16 + 2 * t);
    ga[2] = *(const unsigned *)(A + g * 16 + 2 * t + 8); ga[3] = *(const unsigned *)(A + (g + 8) * 16 + 2 * t + 8);
    mma16(c, ga, pk(__half2float(B[2 * t * 8 + g]), __half2float(B[(2 * t + 1) * 8 + g])),
          pk(__half2float(B[(2 * t + 8) * 8 + g]), __half2float(B[(2 * t + 9) * 8 + g])));
    C[g * 8 + 2 * t] = c[0]; C[g * 8 + 2 * t + 1] = c[1]; C[(g + 8) * 8 + 2 * t] = c[2]; C[(g + 8) * 8 + 2 * t + 1] = c[3];
    pad[blockIdx.x * 32 + l] = sP[l][0] ^ sP[l][3] ^ (rb[2] - rb[0]) ^ (rb[3] - rb[1])
        ^ (rc[0] - ra[0]) ^ (rc[1] - ra[1]) ^ (rc[2] - ra[2]) ^ (rc[3] - ra[3]);
}

/* the same product with fp16 sums, bf16 operands, tf32 operands (K = 8) and int8 operands (K = 32): 4 results a block */
__global__ void k_rest(const signed char *A, const signed char *B, float *C)
{
    int l = threadIdx.x, g = l >> 2, t = l & 3;
    unsigned a[4], b[2], h[2] = {0, 0}; float c[4];
    A += blockIdx.x * 16 * 32; B += blockIdx.x * 32 * 8; C += blockIdx.x * 4 * 128;
#define AE(r, k) ((float)A[(r) * 32 + (k)])
#define BE(k, n) ((float)B[(k) * 8 + (n)])
#define PUT(m, c0, c1, c2, c3) (C[(m) * 128 + g * 8 + 2 * t] = (c0), C[(m) * 128 + g * 8 + 2 * t + 1] = (c1), \
                                C[(m) * 128 + (g + 8) * 8 + 2 * t] = (c2), C[(m) * 128 + (g + 8) * 8 + 2 * t + 1] = (c3))
    a[0] = pk(AE(g, 2 * t), AE(g, 2 * t + 1));         a[1] = pk(AE(g + 8, 2 * t), AE(g + 8, 2 * t + 1));
    a[2] = pk(AE(g, 2 * t + 8), AE(g, 2 * t + 9));     a[3] = pk(AE(g + 8, 2 * t + 8), AE(g + 8, 2 * t + 9));
    b[0] = pk(BE(2 * t, g), BE(2 * t + 1, g));         b[1] = pk(BE(2 * t + 8, g), BE(2 * t + 9, g));
    h[0] = pk(1.0f, -2.0f);
    mma16h(h, a, b);
    mma16h(h, a, b);
    { __half2 h0 = *(__half2 *)&h[0], h1 = *(__half2 *)&h[1];
      PUT(0, __half2float(h0.x), __half2float(h0.y), __half2float(h1.x), __half2float(h1.y)); }

    a[0] = pkb(AE(g, 2 * t), AE(g, 2 * t + 1));        a[1] = pkb(AE(g + 8, 2 * t), AE(g + 8, 2 * t + 1));
    a[2] = pkb(AE(g, 2 * t + 8), AE(g, 2 * t + 9));    a[3] = pkb(AE(g + 8, 2 * t + 8), AE(g + 8, 2 * t + 9));
    b[0] = pkb(BE(2 * t, g), BE(2 * t + 1, g));        b[1] = pkb(BE(2 * t + 8, g), BE(2 * t + 9, g));
    c[0] = 1; c[1] = 2; c[2] = 3; c[3] = 4;
    mma16b(c, a, b);
    PUT(1, c[0], c[1], c[2], c[3]);

    a[0] = __float_as_uint(AE(g, t));     a[1] = __float_as_uint(AE(g + 8, t));
    a[2] = __float_as_uint(AE(g, t + 4)); a[3] = __float_as_uint(AE(g + 8, t + 4));
    b[0] = __float_as_uint(BE(t, g));     b[1] = __float_as_uint(BE(t + 4, g));
    c[0] = c[1] = c[2] = c[3] = 0.5f;
    mma8t(c, a, b);
    PUT(2, c[0], c[1], c[2], c[3]);

    mma32s(c, pk8(A + g * 32 + 4 * t, 1), pk8(A + (g + 8) * 32 + 4 * t, 1), pk8(A + g * 32 + 4 * t + 16, 1), pk8(A + (g + 8) * 32 + 4 * t + 16, 1),
           pk8(B + 4 * t * 8 + g, 8), pk8(B + (4 * t + 16) * 8 + g, 8));
    PUT(3, c[0] - 12582912.0f, c[1] - 12582912.0f, c[2] - 12582912.0f, c[3] - 12582912.0f);
}

static signed char hA[NB][16][32], hB[NB][32][8];
static __half hAh[NB][16][16], hBh[NB][16][8];
static float hC[NB][16][8], hR[NB][4][16][8];
static unsigned hP[NB][32];

static int dot(int b, int r, int n, int K) { int s = 0; for (int k = 0; k < K; k++) s += hA[b][r][k] * hB[b][k][n]; return s; }

int main(void)
{
    __half *dA, *dB; float *dC, *dR; unsigned *dP; signed char *dA8, *dB8;
    unsigned x = 12345; int bad[5] = {0, 0, 0, 0, 0}, badp = 0; long sum[5] = {0, 0, 0, 0, 0};
    static const char *const name[5] = {"f16 f32", "f16 f16", "bf16", "tf32", "s8"};
    for (int b = 0; b < NB; b++) {
        for (int i = 0; i < 16 * 32; i++) { x = x * 1103515245u + 12345u; hA[b][i / 32][i % 32] = (signed char)((x >> 16) % 7) - 3; }
        for (int i = 0; i < 32 * 8; i++) { x = x * 1103515245u + 12345u; hB[b][i / 8][i % 8] = (signed char)((x >> 16) % 7) - 3; }
        for (int i = 0; i < 16 * 16; i++) hAh[b][i / 16][i % 16] = __float2half((float)hA[b][i / 16][i % 16]);
        for (int i = 0; i < 16 * 8; i++) hBh[b][i / 8][i % 8] = __float2half((float)hB[b][i / 8][i % 8]);
    }
    cudaMalloc(&dA, sizeof hAh); cudaMalloc(&dB, sizeof hBh); cudaMalloc(&dC, sizeof hC); cudaMalloc(&dR, sizeof hR);
    cudaMalloc(&dP, sizeof hP); cudaMalloc(&dA8, sizeof hA); cudaMalloc(&dB8, sizeof hB);
    cudaMemcpy(dA, hAh, sizeof hAh, cudaMemcpyHostToDevice); cudaMemcpy(dB, hBh, sizeof hBh, cudaMemcpyHostToDevice);
    cudaMemcpy(dA8, hA, sizeof hA, cudaMemcpyHostToDevice); cudaMemcpy(dB8, hB, sizeof hB, cudaMemcpyHostToDevice);
    k_f16<<<NB, 32>>>(dA, dB, dC, dP);
    k_rest<<<NB, 32>>>(dA8, dB8, dR);
    if (cudaDeviceSynchronize() != cudaSuccess) { printf("failed: %s\n", cudaGetErrorString(cudaGetLastError())); return 1; }
    cudaMemcpy(hC, dC, sizeof hC, cudaMemcpyDeviceToHost); cudaMemcpy(hR, dR, sizeof hR, cudaMemcpyDeviceToHost);
    cudaMemcpy(hP, dP, sizeof hP, cudaMemcpyDeviceToHost);
    for (int b = 0; b < NB; b++) {
        for (int r = 0; r < 16; r++)
            for (int n = 0; n < 8; n++) {
                int w = b * 131 + r * 17 + n + 1, i = r * 8 + n;
                float want[5];
                want[0] = 2.0f * dot(b, r, n, 16);
                want[1] = 2.0f * dot(b, r, n, 16) + (r < 8 ? (n & 1 ? -2.0f : 1.0f) : 0.0f);
                want[2] = dot(b, r, n, 16) + (float)(1 + (r >= 8) * 2 + (n & 1));
                want[3] = dot(b, r, n, 8) + 0.5f;
                want[4] = (float)dot(b, r, n, 32);
                (void)i;
                for (int m = 0; m < 5; m++) {
                    float got = m ? hR[b][m - 1][r][n] : hC[b][r][n];
                    if (got != want[m]) { if (!bad[m]) printf("%s: block %d row %d column %d: %g, not %g\n", name[m], b, r, n, got, want[m]); bad[m]++; }
                    sum[m] += (long)(got * 2) * w;
                }
            }
        for (int l = 0; l < 32; l++) {
            unsigned want = l < 16 ? (0xdead0000u + l * 4) ^ (0xdead0000u + l * 4 + 3) : 0;
            if (hP[b][l] != want) { if (!badp) printf("pad: block %d lane %d: %x, not %x\n", b, l, hP[b][l], want); badp++; }
        }
    }
    for (int m = 0; m < 5; m++) printf("%-8s wrong %d sum %ld\n", name[m], bad[m], sum[m]);
    printf("copies and loads wrong %d\n", badp);
    return 0;
}
