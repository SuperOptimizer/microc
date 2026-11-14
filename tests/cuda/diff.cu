/* The same functions run as device code (PTX, by the engine) and as host code: every result must agree.
   Each takes two pseudo-random 64-bit numbers and gives 64 bits. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <cuda_runtime.h>
#include <cuda_fp16.h>

typedef unsigned long long u64;
typedef long long i64;
typedef unsigned u32;
#define HD __host__ __device__ static

HD u64 mix(u64 x) { x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 29; x *= 0xc4ceb9fe1a85ec53ULL; return x ^ (x >> 32); }
HD u64 fbits(float f) { union { float f; u32 u; } v; v.f = f; return f != f ? 0x7fc00000u : v.u; }
HD u64 dbits(double f) { union { double f; u64 u; } v; v.f = f; return f != f ? 0x7ff8ULL << 48 : v.u; }
HD float tof(u64 a) { return (float)((i64)(a % 2000001) - 1000000) / 1024.0f; }
HD double tod(u64 a) { return (double)((i64)(a % 2000000001ULL) - 1000000000) / 65536.0; }

struct Big { long a[9]; int b[7]; char c[5]; short d; double e; float f[3]; };   /* over 128 bytes */
struct Mid { u64 a[15]; int t; };                                                   /* 124 bytes of words: a copy written out */
struct Bits { unsigned a : 3; int b : 7; unsigned c : 11; int d : 5; u64 e : 40; };
/* (tables inside functions: nvcc does not let device code read an object of the host's at file scope) */
HD int tab(int i) { const int t[8] = {3, 1, 4, 1, 5, 9, 2, 6}; return t[i]; }
HD double dtab(int i) { const double t[4] = {0.5, -1.25, 3.0, 1e10}; return t[i]; }

HD u64 f_int32(u64 a, u64 b) {
    int x = (int)a, y = (int)b; u32 ux = (u32)a, uy = (u32)b; u64 r = 0;
    r = mix(r + (u32)(x + y)); r = mix(r + (u32)(x - y)); r = mix(r + (u32)(x * y));
    if (y != 0 && !(x == (-2147483647 - 1) && y == -1)) { r = mix(r + (u32)(x / y)); r = mix(r + (u32)(x % y)); }
    if (uy != 0) { r = mix(r + ux / uy); r = mix(r + ux % uy); }
    r = mix(r + (u32)(x >> (y & 31))); r = mix(r + (ux >> (y & 31))); r = mix(r + (ux << (y & 31)));
    r = mix(r + (u32)(x & y)); r = mix(r + (u32)(x | y)); r = mix(r + (u32)(x ^ y)); r = mix(r + (u32)~x); r = mix(r + (0u - ux));
    r = mix(r + (x < y) + 2 * (x <= y) + 4 * (ux < uy) + 8 * (ux >= uy) + 16 * (x == y) + 32 * (x != y) + 64 * (x > y));
    r = mix(r + (u32)(x / 7)); r = mix(r + (u32)(x % 7)); r = mix(r + ux / 10); r = mix(r + ux % 1000); r = mix(r + (u32)(x / 16)); r = mix(r + (u32)(x % 8));
    r = mix(r + (x < 0 ? 0u - ux : ux)); r = mix(r + (u32)(x > y ? x : y)); r = mix(r + (ux < uy ? ux : uy));
    return r;
}
HD u64 f_int64(u64 a, u64 b) {
    i64 x = (i64)a, y = (i64)b; u64 r = 0;
    r = mix(r + (u64)(x + y)); r = mix(r + (u64)(x - y)); r = mix(r + (u64)(x * y));
    if (y != 0 && !(x == (-9223372036854775807LL - 1) && y == -1)) { r = mix(r + (u64)(x / y)); r = mix(r + (u64)(x % y)); }
    if (b != 0) { r = mix(r + a / b); r = mix(r + a % b); }
    r = mix(r + (u64)(x >> (b & 63))); r = mix(r + (a >> (b & 63))); r = mix(r + (a << (b & 63)));
    r = mix(r + (x < y) + 2 * (a < b) + 4 * (x >= y) + 8 * (a >= b));
    r = mix(r + (u64)(x / 1000)); r = mix(r + a % 1000000007ULL); r = mix(r + (u64)(int)a); r = mix(r + (u64)(u32)b);
    r = mix(r + (u64)(i64)(short)a); r = mix(r + (u64)(i64)(signed char)b); r = mix(r + (unsigned char)a + (unsigned short)b);
    return r;
}
HD u64 f_small(u64 a, u64 b) {
    signed char c[4]; unsigned char uc[4]; short s[2]; unsigned short us[2]; u64 r = 0; int i;
    for (i = 0; i < 4; i++) { c[i] = (signed char)(a >> (8 * i)); uc[i] = (unsigned char)(b >> (8 * i)); }
    s[0] = (short)a; s[1] = (short)(b >> 7); us[0] = (unsigned short)(a >> 3); us[1] = (unsigned short)b;
    for (i = 0; i < 4; i++) { c[i] = (signed char)(c[i] + uc[i]); uc[i] = (unsigned char)(uc[i] * 3 + c[(i + 1) & 3]); r = mix(r + (u64)(i64)c[i] + uc[i]); }
    s[0] = (short)(s[0] * s[1]); us[1] = (unsigned short)(us[1] - us[0]); s[1] >>= 2; us[0] >>= 3;
    return mix(r + (u64)(i64)s[0] + (u64)(i64)s[1] * 3 + us[0] * 5 + us[1] * 7);
}
HD u64 f_f32(u64 a, u64 b) {
    float x = tof(a), y = tof(b); u64 r = 0;
    r = mix(r + fbits(x + y)); r = mix(r + fbits(x - y)); r = mix(r + fbits(x * y)); r = mix(r + fbits(y != 0 ? x / y : x));
    r = mix(r + fbits(-x)); r = mix(r + fbits(fabsf(x))); r = mix(r + fbits(sqrtf(fabsf(y)))); r = mix(r + fbits(fminf(x, y))); r = mix(r + fbits(fmaxf(x, y)));
    r = mix(r + fbits(floorf(x))); r = mix(r + fbits(ceilf(y))); r = mix(r + fbits(truncf(x))); r = mix(r + fbits(rintf(y)));
    r = mix(r + (x < y) + 2 * (x <= y) + 4 * (x == y) + 8 * (x != y) + 16 * (x > y) + 32 * (x >= y));
    r = mix(r + (u32)(int)x); r = mix(r + (u64)(i64)y); r = mix(r + (u32)fabsf(x)); r = mix(r + fbits((float)(int)a)); r = mix(r + fbits((float)(u32)b));
    r = mix(r + fbits((float)a)); r = mix(r + fbits((float)(i64)b)); r = mix(r + dbits((double)x * y)); r = mix(r + fbits(x > 0 ? x : y));
    r = mix(r + fbits(copysignf(x, y)));
    return r;
}
HD u64 f_f64(u64 a, u64 b) {
    double x = tod(a), y = tod(b); u64 r = 0;
    r = mix(r + dbits(x + y)); r = mix(r + dbits(x - y)); r = mix(r + dbits(x * y)); r = mix(r + dbits(y != 0 ? x / y : x));
    r = mix(r + dbits(-x)); r = mix(r + dbits(fabs(x))); r = mix(r + dbits(sqrt(fabs(y)))); r = mix(r + dbits(fmin(x, y))); r = mix(r + dbits(fmax(x, y)));
    r = mix(r + dbits(floor(x))); r = mix(r + dbits(ceil(y))); r = mix(r + dbits(trunc(x))); r = mix(r + dbits(rint(y)));
    r = mix(r + (x < y) + 2 * (x <= y) + 4 * (x == y) + 8 * (x != y));
    r = mix(r + (u32)(int)x); r = mix(r + (u64)(i64)y); r = mix(r + dbits((double)a)); r = mix(r + dbits((double)(i64)b)); r = mix(r + fbits((float)x));
    r = mix(r + (u64)fabs(x)); r = mix(r + dbits((double)(u32)a)); r = mix(r + dbits(dtab(a & 3) * x));
    return r;
}
HD u64 f_nan(u64 a, u64 b) {
    float z = (float)(a & 1) - (float)(a & 1), n = z / z, inf = 1.0f / z, x = tof(b); u64 r = 0;   /* 0 or NaN and infinity made at run time */
    r = mix(r + (n < x) + 2 * (n > x) + 4 * (n == x) + 8 * (n != x) + 16 * (n <= x) + 32 * (n >= x) + 64 * !(n < x) + 128 * (n != n));
    r = mix(r + (inf > x) + 2 * (-inf < x) + 4 * (inf == inf)); r = mix(r + fbits(fminf(n, x))); r = mix(r + fbits(fmaxf(x, n)));
    r = mix(r + fbits(x + inf)); r = mix(r + fbits(n * 0.0f)); r = mix(r + (n < x ? 1 : 2)); r = mix(r + (x >= n ? 3 : 4));
    return r;
}
HD u64 f_half(u64 a, u64 b) {
    float x = tof(a) / 8.0f, y = tof(b) * 100.0f; __half h = __float2half(x), g = __float2half(y); u64 r = 0;
    r = mix(r + __half_as_ushort(h)); r = mix(r + __half_as_ushort(g)); r = mix(r + fbits(__half2float(h))); r = mix(r + __half_as_ushort(__hadd(h, g)));
    r = mix(r + __half_as_ushort(__hmul(h, h))); r = mix(r + fbits(__half2float(__ushort_as_half((unsigned short)a))));
    r = mix(r + __half_as_ushort(__float2half((float)tod(a) * 1e-9f))); r = mix(r + __half_as_ushort(__hmax(h, g)));
    return r;
}
HD u64 f_flow(u64 a, u64 b) {
    u64 r = 0; int i, j, n = (int)(a & 15) + 1; u32 x = (u32)b;
    for (i = 0; i < n; i++) {
        switch ((x >> i) & 7) {
        case 0: r += 1; break;
        case 1: r ^= x; /* falls through */
        case 2: r = mix(r); break;
        case 5: continue;
        case 6: if (x & 64) goto out; r += 9; break;
        default: r += (x >> 5) & 3;
        }
        for (j = i; j < n; j += 3) { if ((x >> j) & 1) break; r += tab((i + j) & 7); }
        do { r = mix(r + i); x = x * 1664525u + 1013904223u; } while ((x & 3) == 0);
    }
out:
    while (x > 1000) x /= 3;
    return mix(r + x + (a > b ? a - b : a < b / 2 ? 1 : (x & 1) ? 7 : 11));
}
HD u64 big_sum(const Big *p) { u64 r = 0; int i; for (i = 0; i < 9; i++) r = mix(r + (u64)p->a[i]); for (i = 0; i < 7; i++) r = mix(r + (u32)p->b[i]);
    for (i = 0; i < 5; i++) r = mix(r + (u64)(i64)p->c[i]); r = mix(r + (u64)(i64)p->d); r = mix(r + dbits(p->e)); for (i = 0; i < 3; i++) r = mix(r + fbits(p->f[i])); return r; }
HD Big big_make(u64 a, u64 b) { Big g = {}; int i; for (i = 0; i < 9; i++) g.a[i] = (long)mix(a + i); for (i = 0; i < 7; i++) g.b[i] = (int)mix(b + i);
    for (i = 0; i < 5; i++) g.c[i] = (char)(a >> (i * 5)); g.d = (short)b; g.e = tod(a); for (i = 0; i < 3; i++) g.f[i] = tof(b + i); return g; }
HD u64 big_val(Big g, int k) { g.a[k % 9] += k; return big_sum(&g); }
HD u64 f_struct(u64 a, u64 b) {
    Big g = big_make(a, b), h = g, z = {}; Mid m = {}, m2; Bits bf; u64 r; int i;
    h.a[3] ^= 5; z.b[2] = 7; r = mix(big_sum(&g) + big_sum(&h) * 3 + big_sum(&z) * 5 + big_val(g, (int)(a & 31)));
    for (i = 0; i < 15; i++) m.a[i] = mix(a ^ (u64)i); m.t = (int)b; m2 = m; m2.a[14] += 1; for (i = 0; i < 15; i++) r = mix(r + m2.a[i] - m.a[i] + m.a[i] / 3); r = mix(r + (u32)m2.t);
    memset(&m, 0, sizeof m); m.a[7] = 9; r = mix(r + m.a[0] + m.a[7] + m.a[14] + (u32)m.t);
    bf.a = (unsigned)a; bf.b = (int)b; bf.c = (unsigned)(a >> 7); bf.d = (int)(b >> 9); bf.e = a * b; bf.b += 3; bf.c ^= 0x55; bf.e >>= 3;
    return mix(r + bf.a + (u64)(i64)bf.b * 3 + bf.c * 5 + (u64)(i64)bf.d * 7 + bf.e);
}
HD u64 f_array(u64 a, u64 b) {
    int v[40]; float w[24]; u64 r = 0; int i, n = 40; long idx = (long)(a % 37);
    for (i = 0; i < n; i++) v[i] = (int)mix(a + i) % 1000;
    for (i = 1; i < n; i++) { int k = v[i], j = i - 1; while (j >= 0 && v[j] > k) { v[j + 1] = v[j]; j--; } v[j + 1] = k; }
    for (i = 0; i < n; i++) r = mix(r + (u32)v[i]);
    for (i = 0; i < 24; i++) w[i] = tof(b + i) * (float)tab(i & 7);
    for (i = 23; i > 0; i--) w[i - 1] += w[i] * 0.5f;
    { int *p = v + idx, *q = &v[39]; r = mix(r + (u64)(q - p) + (u32)*p + (u32)p[1] + (u32)q[-2]); }
    return mix(r + fbits(w[0]) + fbits(w[idx % 24]) * 3);
}
HD u64 f_math(u64 a, u64 b) {
    float x = tof(a) / 64.0f, y = fabsf(tof(b)) / 512.0f + 0.25f; u64 r = 0;
    /* only what must be exactly rounded, or is an exact case */
    r = mix(r + fbits(fmaf(x, y, x))); r = mix(r + dbits(fma((double)x, (double)y, 1.0))); r = mix(r + fbits(sqrtf(y)));
    r = mix(r + fbits(roundf(x))); r = mix(r + dbits(round((double)x * 3))); r = mix(r + fbits(fmodf(x, y))); r = mix(r + (u64)lrintf(x)); r = mix(r + (u64)(i64)lroundf(x * 7));
    r = mix(r + fbits(ldexpf(x, (int)(b & 7) - 3))); r = mix(r + fbits(expf(0.0f) + logf(1.0f) + powf(2.0f, 3.0f)));
    return r;
}
HD u64 f_bits(u64 a, u64 b) {
    u32 x = (u32)a | 1; u64 r = 0;
    /* (written out: nvcc's device code has its own names for these, see the warp kernel) */
    { u32 n = 0, t; u64 w; for (t = x; t; t &= t - 1) n++; r = mix(r + n); for (n = 0, w = b; w; w &= w - 1) n++; r = mix(r + n);
      for (n = 0; !(x >> (31 - n) & 1); n++) ; r = mix(r + n); for (n = 0; !(x >> n & 1); n++) ; r = mix(r + n);
      for (n = 0, w = b | 1; !(w >> (63 - n) & 1); n++) ; r = mix(r + n); for (n = 0, w = b | (1ULL << 63); !(w >> n & 1); n++) ; r = mix(r + n);
      r = mix(r + ((x >> 24) | (x >> 8 & 0xff00) | (x << 8 & 0xff0000) | (x << 24)));
      w = (b >> 32) | (b << 32); w = (w >> 16 & 0x0000ffff0000ffffULL) | (w << 16 & 0xffff0000ffff0000ULL);
      r = mix(r + ((w >> 8 & 0x00ff00ff00ff00ffULL) | (w << 8 & 0xff00ff00ff00ff00ULL))); }
    r = mix(r + ((x << 7) | (x >> 25))); r = mix(r + ((b << 13) | (b >> 51)));
    return r;
}
#define NF 13
HD u64 call(int k, u64 a, u64 b) {
    switch (k) {
    case 0: return f_int32(a, b); case 1: return f_int64(a, b); case 2: return f_small(a, b); case 3: return f_f32(a, b);
    case 4: return f_f64(a, b); case 5: return f_nan(a, b); case 6: return f_half(a, b); case 7: return f_flow(a, b);
    case 8: return f_struct(a, b); case 9: return f_array(a, b); case 10: return f_math(a, b); case 11: return f_bits(a, b);
    default: return mix(a) ^ mix(b + (u64)k);
    }
}
static const char *const names[NF] = {"int32", "int64", "small", "f32", "f64", "nan", "half", "flow", "struct", "array", "math", "bits", "mix"};
#define N 512
HD void inputs(int k, int i, u64 *a, u64 *b) {
    static const u64 edge[8] = {0, 1, ~0ULL, 0x80000000ULL, 0x7fffffffULL, 1ULL << 63, 0xffffffffULL, 0x100000000ULL};
    *a = i < 64 ? edge[i & 7] : mix((u64)i * 77 + (u64)k);
    *b = i < 64 ? edge[(i >> 3) & 7] : mix((u64)i * 131 + (u64)k * 7 + 1);
}
__global__ void run(int k, u64 *out)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x; u64 a, b;
    if (i >= N) return;
    inputs(k, i, &a, &b);
    out[i] = call(k, a, b);
}
/* atomics and shared memory: every thread adds to shared and global counters */
__global__ void atoms(int *gi, float *gf, u64 *gl, int *gmax, unsigned *gor)
{
    __shared__ int cnt;
    int t = threadIdx.x;
    if (t == 0) cnt = 0;
    __syncthreads();
    atomicAdd(&cnt, t);
    atomicAdd(gi, 1); atomicAdd(gf, 0.5f); atomicAdd(gl, (u64)t << 20); atomicMax(gmax, (int)(mix(t + blockIdx.x * 64) % 100000)); atomicOr(gor, 1u << (t & 31));
    __syncthreads();
    if (t == 0) atomicAdd(gi + 1, cnt);
}
/* warp operations: one warp, every lane writes what it got; the host works the same out from the definitions */
#define NW 13
HD int wval(int l) { return (int)(mix((u64)l + 5) % 1000) - 500; }
__global__ void warp(int *out)
{
    int l = threadIdx.x, v = wval(l), *o = out + l * NW; float x = (float)v * 0.25f, s;
    o[0] = __shfl_sync(0xffffffffu, v, 5); o[1] = __shfl_sync(0xffffffffu, v, (l * 7 + 3) & 31); o[2] = __shfl_up_sync(0xffffffffu, v, 3);
    o[3] = __shfl_down_sync(0xffffffffu, v, 5); o[4] = __shfl_xor_sync(0xffffffffu, v, 9); o[5] = (int)__ballot_sync(0xffffffffu, v > 0);
    o[6] = __any_sync(0xffffffffu, v > 498) * 2 + __all_sync(0xffffffffu, v > -501); o[7] = __shfl_sync(0xffffffffu, v, l + 1, 8);
    s = x; for (int k = 16; k; k >>= 1) s += __shfl_xor_sync(0xffffffffu, s, k);
    o[8] = (int)s; o[9] = (int)__match_any_sync(0xffffffffu, (unsigned)(v & 3));
    o[10] = __ffs(v) + 100 * __clz(v) + 10000 * __popc((unsigned)v) + 1000000 * __ffsll((long long)v << 20);
    { int pr[2]; pr[0] = v; pr[1] = l;   /* assembly with several results, each kept in memory */
      asm volatile("sub.s32 %1, %1, %2;\n\tadd.s32 %0, %0, %2;" : "+r"(pr[0]), "+r"(pr[1]) : "r"(v)); o[12] = pr[0] * 3 + pr[1]; }
    o[11] = (int)__brev((unsigned)v) ^ __reduce_max_sync(0xffffffffu, v) ^ (int)__shfl_sync(0xffffffffu, x, 31 - l);
}
static int ffs_(u64 x) { int n = 1; if (!x) return 0; while (!(x & 1)) { x >>= 1; n++; } return n; }
static int clz_(u32 x) { int n = 0; while (n < 32 && !(x >> (31 - n) & 1)) n++; return n; }
static int popc_(u32 x) { int n = 0; while (x) { n += x & 1; x >>= 1; } return n; }
static u32 brev_(u32 x) { u32 r = 0; for (int i = 0; i < 32; i++) r |= (x >> i & 1) << (31 - i); return r; }
static int warp_check(const int *out)
{
    int bad = 0, v[32], mx = -1000, any = 0, all = 1; u32 ballot = 0; float sum = 0, part[32];
    for (int l = 0; l < 32; l++) { v[l] = wval(l); if (v[l] > mx) mx = v[l]; ballot |= (u32)(v[l] > 0) << l; any |= v[l] > 498; all &= v[l] > -501; part[l] = (float)v[l] * 0.25f; }
    for (int k = 16; k; k >>= 1) { float t[32]; for (int l = 0; l < 32; l++) t[l] = part[l] + part[l ^ k]; memcpy(part, t, sizeof t); }
    (void)sum;
    for (int l = 0; l < 32; l++) {
        int e[NW]; u32 match = 0;
        for (int j = 0; j < 32; j++) match |= (u32)((v[j] & 3) == (v[l] & 3)) << j;
        e[0] = v[5]; e[1] = v[(l * 7 + 3) & 31]; e[2] = l >= 3 ? v[l - 3] : v[l]; e[3] = l + 5 < 32 ? v[l + 5] : v[l]; e[4] = v[l ^ 9]; e[5] = (int)ballot;
        e[6] = any * 2 + all; e[7] = v[(l & ~7) | ((l + 1) & 7)]; e[8] = (int)part[l]; e[9] = (int)match;
        e[10] = ffs_((u32)v[l]) + 100 * clz_((u32)v[l]) + 10000 * popc_((u32)v[l]) + 1000000 * ffs_((u64)((long long)v[l] << 20));
        e[11] = (int)brev_((u32)v[l]) ^ mx ^ (int)((float)v[31 - l] * 0.25f);
        e[12] = (v[l] + v[l]) * 3 + (l - v[l]);
        for (int q = 0; q < NW; q++) if (out[l * NW + q] != e[q]) { if (!bad) printf("warp: lane %d value %d: device %d host %d\n", l, q, out[l * NW + q], e[q]); bad++; }
    }
    printf("warp: %d of %d differ\n", bad, 32 * NW);
    return bad;
}
int main(void)
{
    u64 *d, h[N], a, b; int k, i, bad, total = 0, *gi, hi[3]; float *gf, hf; u64 *gl, hl; unsigned *gor, hor;
    if (cudaMalloc(&d, sizeof h)) { printf("no device\n"); return 1; }
    for (k = 0; k < NF; k++) {
        cudaMemset(d, 0, sizeof h);
        run<<<N / 64, 64>>>(k, d);
        if (cudaDeviceSynchronize()) { printf("%s: %s\n", names[k], cudaGetErrorString(cudaGetLastError())); total++; continue; }
        cudaMemcpy(h, d, sizeof h, cudaMemcpyDeviceToHost);
        for (i = bad = 0; i < N; i++) {
            inputs(k, i, &a, &b);
            if (h[i] != call(k, a, b)) { if (!bad) printf("%s: input %d (%llx, %llx): device %llx host %llx\n", names[k], i, a, b, h[i], call(k, a, b)); bad++; }
        }
        printf("%s: %d of %d differ\n", names[k], bad, N);
        total += bad;
    }
    cudaMalloc(&gi, sizeof hi); cudaMalloc(&gf, 4); cudaMalloc(&gl, 8); cudaMalloc(&gor, 4);
    cudaMemset(gi, 0, sizeof hi); cudaMemset(gf, 0, 4); cudaMemset(gl, 0, 8); cudaMemset(gor, 0, 4);
    atoms<<<8, 64>>>(gi, gf, gl, gi + 2, gor);
    cudaDeviceSynchronize();
    cudaMemcpy(hi, gi, sizeof hi, cudaMemcpyDeviceToHost); cudaMemcpy(&hf, gf, 4, cudaMemcpyDeviceToHost); cudaMemcpy(&hl, gl, 8, cudaMemcpyDeviceToHost); cudaMemcpy(&hor, gor, 4, cudaMemcpyDeviceToHost);
    for (i = 0, k = 0; i < 512; i++) { int v = (int)(mix(i) % 100000); if (v > k) k = v; }
    printf("atoms: %d %d %g %llu %d %x (max should be %d)\n", hi[0], hi[1], hf, hl >> 20, hi[2], hor, k);
    { int *w, hw[32 * NW]; cudaMalloc(&w, sizeof hw); cudaMemset(w, 0, sizeof hw); warp<<<1, 32>>>(w); cudaDeviceSynchronize();
      cudaMemcpy(hw, w, sizeof hw, cudaMemcpyDeviceToHost); total += warp_check(hw); }
    printf("%s\n", total ? "FAILED" : "all agree");
    return total != 0;
}
