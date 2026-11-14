/* nvcc: -gencode arch=compute_120a,code=sm_120a */
/* The 8-bit and 4-bit floats of the newest tensor cores (e4m3, e2m1), as inline PTX: the conversions both ways
   over special, random and all operands, and the matrix products whose operands carry a power of two for
   every 32 (or 16) terms of a row or column. Halves and small integers: every result is exact. */
#include <stdio.h>
#include <string.h>
#include <cuda_runtime.h>
#include <cuda_fp16.h>

enum { NI = 1024, NB = 3 };

__device__ __forceinline__ unsigned enc8(float lo, float hi) { unsigned short r; asm("cvt.rn.satfinite.e4m3x2.f32 %0, %1, %2;" : "=h"(r) : "f"(hi), "f"(lo)); return r; }
__device__ __forceinline__ unsigned enc4(float lo, float hi) {
    unsigned short r; asm("{ .reg .b8 t; cvt.rn.satfinite.e2m1x2.f32 t, %1, %2; cvt.u16.u8 %0, t; }" : "=h"(r) : "f"(hi), "f"(lo)); return r;
}
__device__ __forceinline__ unsigned dec8(unsigned v) { unsigned r; asm("cvt.rn.f16x2.e4m3x2 %0, %1;" : "=r"(r) : "h"((unsigned short)v)); return r; }
__device__ __forceinline__ unsigned dec4(unsigned b) { unsigned r; asm("{ .reg .b8 t; cvt.u8.u32 t, %1; cvt.rn.f16x2.e2m1x2 %0, t; }" : "=r"(r) : "r"(b & 0xff)); return r; }

__device__ __forceinline__ void mma_f8(float *c, const unsigned *a, const unsigned *b, unsigned sa, unsigned sb) {
    asm volatile("mma.sync.aligned.m16n8k32.row.col.kind::mxf8f6f4.block_scale.scale_vec::1X.f32.e4m3.e4m3.f32.ue8m0 "
                 "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3}, %10, {0, 0}, %11, {0, 0};\n"
                 : "+f"(c[0]), "+f"(c[1]), "+f"(c[2]), "+f"(c[3]) : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b[0]), "r"(b[1]), "r"(sa), "r"(sb));
}
template <int BA, int TA, int BB, int TB>
__device__ __forceinline__ void mma_f4(float *c, const unsigned *a, const unsigned *b, unsigned sa, unsigned sb) {
    asm volatile("mma.sync.aligned.m16n8k64.row.col.kind::mxf4.block_scale.scale_vec::2X.f32.e2m1.e2m1.f32.ue8m0 "
                 "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3}, %10, {%12, %13}, %11, {%14, %15};\n"
                 : "+f"(c[0]), "+f"(c[1]), "+f"(c[2]), "+f"(c[3]) : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b[0]), "r"(b[1]), "r"(sa), "r"(sb),
                   "n"(BA), "n"(TA), "n"(BB), "n"(TB));
}
__device__ __forceinline__ void mma_f4x4(float *c, const unsigned *a, const unsigned *b, unsigned sa, unsigned sb) {
    asm volatile("mma.sync.aligned.m16n8k64.row.col.kind::mxf4nvf4.block_scale.scale_vec::4X.f32.e2m1.e2m1.f32.ue8m0 "
                 "{%0,%1,%2,%3}, {%4,%5,%6,%7}, {%8,%9}, {%0,%1,%2,%3}, %10, {0, 0}, %11, {0, 0};\n"
                 : "+f"(c[0]), "+f"(c[1]), "+f"(c[2]), "+f"(c[3]) : "r"(a[0]), "r"(a[1]), "r"(a[2]), "r"(a[3]), "r"(b[0]), "r"(b[1]), "r"(sa), "r"(sb));
}

__global__ void k_cvt(const float *in, unsigned *e8, unsigned *e4, unsigned *d8, unsigned *d4)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < NI) { float x = in[i], y = in[(i * 7 + 3) % NI]; e8[i] = enc8(x, y); e4[i] = enc4(x, y); }
    if (i < 256) d4[i] = dec4((unsigned)i);
    d8[i] = dec8((unsigned)i);
}

/* A[16][64], B[64][8]: values both formats hold. sa[16][4], sb[8][4]: the powers (as ue8m0: 127 + the power) of
   each quarter of a row of A and of a column of B. Three products a block: e4m3 over the first 32 of K with the
   powers of the first quarter; e2m1 over all 64 with those of quarters 0 and 1 for the halves; e2m1 with all four. */
__global__ void k_mma(const float *A, const float *B, const unsigned char *sa, const unsigned char *sb, float *C)
{
    int l = threadIdx.x, g = l >> 2, t = l & 3;
    unsigned a[4], b[2], ra, rb; float c[4];
    A += blockIdx.x * 16 * 64; B += blockIdx.x * 64 * 8; sa += blockIdx.x * 64; sb += blockIdx.x * 32; C += blockIdx.x * 3 * 128;
    /* the powers: of row g in lane 4g, of row g + 8 in lane 4g + 1, of column n in lane 4n; nothing anyone reads elsewhere */
    ra = t == 0 ? *(const unsigned *)(sa + g * 4) : t == 1 ? *(const unsigned *)(sa + (g + 8) * 4) : 0x11223344u * (unsigned)l;
    rb = t == 0 ? *(const unsigned *)(sb + g * 4) : 0x55667788u ^ (unsigned)l;
#define AE(r, k) A[(r) * 64 + (k)]
#define BE(k, n) B[(k) * 8 + (n)]
#define P8(X, k) (enc8(X(k), X((k) + 1)) | enc8(X((k) + 2), X((k) + 3)) << 16)
#define P4(X, k) (enc4(X(k), X((k) + 1)) | enc4(X((k) + 2), X((k) + 3)) << 8 | enc4(X((k) + 4), X((k) + 5)) << 16 | enc4(X((k) + 6), X((k) + 7)) << 24)
#define PUT(m) (C[(m) * 128 + g * 8 + 2 * t] = c[0], C[(m) * 128 + g * 8 + 2 * t + 1] = c[1], \
                C[(m) * 128 + (g + 8) * 8 + 2 * t] = c[2], C[(m) * 128 + (g + 8) * 8 + 2 * t + 1] = c[3])
#define A0(k) AE(g, k)
#define A1(k) AE(g + 8, k)
#define B0(k) BE(k, g)
    a[0] = P8(A0, 4 * t); a[1] = P8(A1, 4 * t); a[2] = P8(A0, 4 * t + 16); a[3] = P8(A1, 4 * t + 16);
    b[0] = P8(B0, 4 * t); b[1] = P8(B0, 4 * t + 16);
    c[0] = 1; c[1] = -2; c[2] = 0.5f; c[3] = 0;
    mma_f8(c, a, b, ra, rb);
    PUT(0);
    a[0] = P4(A0, 8 * t); a[1] = P4(A1, 8 * t); a[2] = P4(A0, 8 * t + 32); a[3] = P4(A1, 8 * t + 32);
    b[0] = P4(B0, 8 * t); b[1] = P4(B0, 8 * t + 32);
    c[0] = c[1] = c[2] = c[3] = 0;
    mma_f4<0, 0, 0, 0>(c, a, b, ra, rb);
    PUT(1);
    c[0] = c[1] = c[2] = c[3] = 0.25f;
    mma_f4x4(c, a, b, ra, rb);
    PUT(2);
}

static float hin[NI], hA[NB][16][64], hB[NB][64][8], hC[NB][3][16][8];
static unsigned char hsa[NB][16][4], hsb[NB][8][4];
static unsigned he8[NI], he4[NI], hd8[65536], hd4[256];

static unsigned fnv(const unsigned *p, int n) { unsigned h = 2166136261u; for (int i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; return h; }
static float pw(int e) { float f = 1; for (; e > 0; e--) f *= 2; for (; e < 0; e++) f *= 0.5f; return f; }

int main(void)
{
    static const unsigned special[] = {0, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 1, 0x80000001u, 0x7f7fffffu, 0xff7fffffu,
        0x3f800000u, 0xbf800000u, 0x3f000000u, 0x3fc00000u, 0x43e00000u, 0x43e80000u, 0x43f00000u, 0xc3e00000u, 0x40c00000u, 0x40e00000u,
        0x40a00000u, 0x40200000u, 0x3e800000u, 0x3e000000u, 0x3c800000u, 0x3b800000u, 0x3b000000u, 0x3a800000u, 0x3f900000u, 0x3fb00000u};
    static const float both[] = {0, 0.5f, 1, 1.5f, 2, 3, 4, 6};
    float *din, *dA, *dB, *dC; unsigned *de8, *de4, *dd8, *dd4; unsigned char *dsa, *dsb;
    unsigned s = 4242; int ns = (int)(sizeof special / sizeof *special);
    static const char *const name[3] = {"e4m3, a power a row", "e2m1, a power every 32", "e2m1, a power every 16"};
    for (int i = 0; i < NI; i++) {
        unsigned u;
        s = s * 1664525u + 1013904223u;
        if (i < ns) u = special[i];
        else if (i % 3 == 0) u = (s & 0x807fffffu) | (110 + (s >> 23) % 28) << 23;   /* 2^-17 .. 2^10 */
        else if (i % 3 == 1) { float f = (float)((int)(s >> 8) % 257 - 128) * 0.125f; memcpy(&u, &f, 4); }   /* eighths: ties */
        else { float f = (float)((int)(s >> 8) % 4001 - 2000) * 0.25f; memcpy(&u, &f, 4); }
        memcpy(&hin[i], &u, 4);
    }
    for (int b = 0; b < NB; b++) {
        for (int i = 0; i < 16 * 64; i++) { s = s * 1664525u + 1013904223u; hA[b][i / 64][i % 64] = both[(s >> 16) % 8] * ((s >> 20) & 1 ? -1.0f : 1.0f); }
        for (int i = 0; i < 64 * 8; i++) { s = s * 1664525u + 1013904223u; hB[b][i / 8][i % 8] = both[(s >> 16) % 8] * ((s >> 20) & 1 ? -1.0f : 1.0f); }
        for (int i = 0; i < 64; i++) { s = s * 1664525u + 1013904223u; hsa[b][i / 4][i % 4] = (unsigned char)(127 + (int)((s >> 16) % 9) - 4); }
        for (int i = 0; i < 32; i++) { s = s * 1664525u + 1013904223u; hsb[b][i / 4][i % 4] = (unsigned char)(127 + (int)((s >> 16) % 7) - 3); }
    }
    cudaMalloc(&din, sizeof hin); cudaMalloc(&de8, sizeof he8); cudaMalloc(&de4, sizeof he4); cudaMalloc(&dd8, sizeof hd8); cudaMalloc(&dd4, sizeof hd4);
    cudaMalloc(&dA, sizeof hA); cudaMalloc(&dB, sizeof hB); cudaMalloc(&dC, sizeof hC); cudaMalloc(&dsa, sizeof hsa); cudaMalloc(&dsb, sizeof hsb);
    cudaMemcpy(din, hin, sizeof hin, cudaMemcpyHostToDevice); cudaMemcpy(dA, hA, sizeof hA, cudaMemcpyHostToDevice); cudaMemcpy(dB, hB, sizeof hB, cudaMemcpyHostToDevice);
    cudaMemcpy(dsa, hsa, sizeof hsa, cudaMemcpyHostToDevice); cudaMemcpy(dsb, hsb, sizeof hsb, cudaMemcpyHostToDevice);
    k_cvt<<<65536 / 256, 256>>>(din, de8, de4, dd8, dd4);
    k_mma<<<NB, 32>>>(dA, dB, dsa, dsb, dC);
    if (cudaDeviceSynchronize() != cudaSuccess) { printf("failed: %s\n", cudaGetErrorString(cudaGetLastError())); return 1; }
    cudaMemcpy(he8, de8, sizeof he8, cudaMemcpyDeviceToHost); cudaMemcpy(he4, de4, sizeof he4, cudaMemcpyDeviceToHost);
    cudaMemcpy(hd8, dd8, sizeof hd8, cudaMemcpyDeviceToHost); cudaMemcpy(hd4, dd4, sizeof hd4, cudaMemcpyDeviceToHost);
    cudaMemcpy(hC, dC, sizeof hC, cudaMemcpyDeviceToHost);
    printf("to e4m3 %08x\nto e2m1 %08x\nfrom e4m3 %08x\nfrom e2m1 %08x\n", fnv(he8, NI), fnv(he4, NI), fnv(hd8, 65536), fnv(hd4, 256));
    printf("specials:"); for (int i = 0; i < ns; i++) printf(" %04x/%02x", he8[i], he4[i]); printf("\n");
    printf("e4m3 codes:"); for (int i = 0x70; i < 0x82; i++) printf(" %04x", hd8[i] & 0xffff); printf("\n");
    printf("e2m1 codes:"); for (int i = 0; i < 16; i++) printf(" %04x", hd4[i] & 0xffff); printf(" %08x\n", hd4[0xa7]);
    for (int m = 0; m < 3; m++) {
        int wrong = 0; double sum = 0;
        for (int b = 0; b < NB; b++)
            for (int r = 0; r < 16; r++)
                for (int n = 0; n < 8; n++) {
                    static const float init[4] = {1, -2, 0.5f, 0};
                    float want = m == 0 ? init[(r >= 8) * 2 + (n & 1)] : m == 1 ? 0 : 0.25f, got = hC[b][m][r][n];
                    int K = m ? 64 : 32, per = m == 2 ? 16 : 32;
                    for (int q = 0; q < K / per; q++) {
                        float part = 0;
                        for (int k = q * per; k < (q + 1) * per; k++) part += hA[b][r][k] * hB[b][k][n];
                        want += part * pw(hsa[b][r][q] - 127 + hsb[b][n][q] - 127);
                    }
                    if (got != want) { if (!wrong) printf("%s: block %d row %d column %d: %g, not %g\n", name[m], b, r, n, got, want); wrong++; }
                    sum += (double)got * (r * 8 + n + 1);
                }
        printf("%-24s wrong %d sum %.4f\n", name[m], wrong, sum);
    }
    return 0;
}
