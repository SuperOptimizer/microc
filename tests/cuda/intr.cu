/* The device's functions that programs call by name. The ones with one right answer (rounding, conversions,
   halves, bits, minima) are run over special and random operands and a sum of all the results is printed for
   each, any NaN counted as one value. The ones that are approximations (exp, log, pow, ...) are held to a few
   units in the last place of the right answer (the device's own fast forms, __expf and __logf, to less). Then what the threads of a warp tell each other, and atomics. */
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <cuda_bf16.h>

enum { N = 512 };

__host__ __device__ static inline unsigned U(float f) { unsigned u; if (f != f) return 0x7fc00000u; memcpy(&u, &f, 4); return u; }
__host__ __device__ static inline unsigned D(double f) { unsigned long long u; if (f != f) return 0x7ff80000u; memcpy(&u, &f, 8); return (unsigned)(u ^ u >> 32); }
__host__ __device__ static inline unsigned L(long long v) { return (unsigned)((unsigned long long)v ^ (unsigned long long)v >> 32); }
__device__ static inline unsigned H(__half h) { float f = __half2float(h); return f != f ? 0x7e00u : __half_as_ushort(h); }
__device__ static inline unsigned H2(__half2 h) { return H(h.x) | H(h.y) << 16; }
__device__ static inline unsigned B(__nv_bfloat16 h) { float f = __bfloat162float(h); return f != f ? 0x7fc0u : (unsigned)__bfloat16_as_ushort(h); }
__device__ static inline unsigned B2(__nv_bfloat162 h) { return B(h.x) | B(h.y) << 16; }
__device__ static inline float fin(float f) { return f != f || f > 1e9f || f < -1e9f ? 0.25f : f; }   /* something a conversion to an int holds */

/* x, y, z: floats; a, b: their bits; i: the thread */
#define EXACT(X) \
    X(fmaxf, U(fmaxf(x, y))) X(fminf, U(fminf(x, y))) X(fabsf, U(fabsf(x))) X(fmaf, U(fmaf(x, y, z))) X(sqrtf, U(sqrtf(fabsf(x)))) \
    X(__fmul_rn, U(__fmul_rn(x, y))) X(__fadd_rn, U(__fadd_rn(x, y))) X(__fsub_rn, U(__fsub_rn(x, y))) X(__fdiv_rn, U(__fdiv_rn(x, y))) \
    X(__fmaf_rn, U(__fmaf_rn(x, y, z))) X(__fsqrt_rn, U(__fsqrt_rn(fabsf(y)))) \
    X(mul_add, U(x * y + z)) X(div, U(x / y)) \
    X(floorf, U(floorf(x))) X(ceilf, U(ceilf(x))) X(rintf, U(rintf(x))) X(truncf, U(truncf(x))) X(roundf, U(roundf(x))) \
    X(__float2int_rn, (unsigned)__float2int_rn(x)) X(__float2int_rz, (unsigned)__float2int_rz(x)) X(__float2uint_rn, __float2uint_rn(x)) \
    X(lrintf, L(lrintf(fin(x)))) X(lroundf, L(lroundf(fin(x)))) X(cast_int, (unsigned)(int)fin(x)) X(cast_long, L((long long)fin(x))) \
    X(int_to_float, U((float)(int)a) ^ U((float)b) ^ D((double)(int)a)) \
    X(ldexpf, U(ldexpf(x, i % 40 - 20))) X(frexpf, U(frexpf(fin(x), &e)) + (unsigned)e * 3) X(copysignf, U(copysignf(x, y))) \
    X(ilogbf, (unsigned)ilogbf(fin(x) == 0 ? 1.5f : fin(x))) X(isinf_isnan, (unsigned)(isinf(x) != 0) + 2 * (unsigned)(isnan(x) != 0) + 4 * (unsigned)(isfinite(y) != 0)) \
    X(__float2half_rn, H(__float2half_rn(x))) X(__float2half, H(__float2half(y))) X(__half2float, U(__half2float(__ushort_as_half((unsigned short)a)))) \
    X(__floats2half2_rn, H2(__floats2half2_rn(x, y))) X(__float2half2_rn, H2(__float2half2_rn(z))) \
    X(__half22float2, U(__half22float2(*(__half2 *)&a).x) ^ U(__half22float2(*(__half2 *)&a).y) * 3) \
    X(__low2float, U(__low2float(*(__half2 *)&b)) + U(__high2float(*(__half2 *)&b)) * 5) \
    X(__hfma2, H2(__hfma2(__floats2half2_rn(fin(x), 0.5f), __floats2half2_rn(1.5f, fin(y)), __floats2half2_rn(fin(z), -3.0f)))) \
    X(__float2bfloat16, B(__float2bfloat16(x))) X(__float2bfloat16_rn, B(__float2bfloat16_rn(y))) \
    X(__bfloat162float, U(__bfloat162float(__ushort_as_bfloat16((unsigned short)(b >> 7))))) \
    X(__floats2bfloat162_rn, B2(__floats2bfloat162_rn(x, z))) \
    X(__bfloat1622float2, U(__bfloat1622float2(*(__nv_bfloat162 *)&a).x) ^ U(__bfloat1622float2(*(__nv_bfloat162 *)&a).y) * 3) \
    X(__funnelshift_r, __funnelshift_r(a, b, (unsigned)i)) X(__funnelshift_l, __funnelshift_l(a, b, (unsigned)i * 3)) \
    X(__byte_perm, __byte_perm(a, b, a ^ (b >> 3))) X(__popc, (unsigned)__popc(a)) X(__popcll, (unsigned)__popcll((unsigned long long)a << 32 | b)) \
    X(__ffs, (unsigned)__ffs((int)(a << (i % 32)))) X(__clz, (unsigned)__clz((int)(a >> (i % 33 % 32)))) X(__brev, __brev(a)) \
    X(__mulhi, (unsigned)__mulhi((int)a, (int)b)) X(__umulhi, __umulhi(a, b)) \
    X(min_max, (unsigned)min((int)a, (int)b) ^ max(a, b) ^ U(fminf(x, 0.0f)) ^ U(max(x, y))) \
    X(__double2ll_rn, L(__double2ll_rn((double)fin(x) * 1000.25))) \
    X(__dmul_rn, D(__dmul_rn((double)x, 1.0 / 3.0 + y))) X(__dadd_rn, D(__dadd_rn((double)x, (double)y * 1e-9))) \
    X(__ddiv_rn, D(__ddiv_rn((double)x, (double)y))) X(fma, D(fma((double)x, (double)y, (double)z))) \
    X(sqrt_floor, D(sqrt(fabs((double)x))) ^ D(floor((double)x * 0.37)) ^ D(fmax((double)x, (double)y)) ^ D(fmin((double)x, (double)z)))

/* x in [-8, 8), p in (0, 8) */
#define NEAR(X) \
    X(expf, expf(x), exp(x)) X(__expf, __expf(x), exp(x)) X(exp2f, exp2f(x), exp2(x)) X(logf, logf(p), log(p)) X(__logf, __logf(p), log(p)) \
    X(log2f, log2f(p), log2(p)) X(log1pf, log1pf(p - 0.9f), log1p((double)(p - 0.9f))) X(powf, powf(p, x), pow(p, x)) \
    X(atan2f, atan2f(x, p - 4.0f), atan2(x, (double)(p - 4.0f))) X(erff, erff(x * 0.5f), erf((double)(x * 0.5f))) \
    X(rsqrtf, rsqrtf(p), 1.0 / sqrt(p)) X(hypotf, hypotf(x, p), hypot(x, p)) X(tanhf, tanhf(x), tanh(x)) \
    X(sinf, sinf(x), sin(x)) X(cosf, cosf(x), cos(x)) X(cospif, cospif(x * 0.25f), cos(3.14159265358979323846 * (double)(x * 0.25f))) \
    X(exp, (float)exp((double)x), exp(x)) X(log, (float)log((double)p), log(p))

enum {
#define X(n, e) E_##n,
    EXACT(X)
#undef X
    NE
};
enum {
#define X(n, e, r) A_##n,
    NEAR(X)
#undef X
    A_sincosf_s, A_sincosf_c, A_sincospif_s, A_sincospif_c, NA
};

__global__ void k_exact(const float *in, unsigned *out)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x, k = 0, e = 0;
    float x = __ldg(in + i), y = in[(i * 7 + 3) % N], z = in[(i * 13 + 5) % N];
    unsigned a = __float_as_uint(x), b = __float_as_uint(y);
#define X(n, ex) out[k++ * N + i] = (ex);
    EXACT(X)
#undef X
}

__global__ void k_near(float *out)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x, k = 0;
    float x = (float)(i - N / 2) * (16.0f / N) + 0.001f, p = (float)(i + 1) * (8.0f / (N + 1)), s, c;
#define X(n, ex, r) out[k++ * N + i] = (ex);
    NEAR(X)
#undef X
    sincosf(x, &s, &c); out[k++ * N + i] = s; out[k++ * N + i] = c;
    sincospif(x * 0.25f, &s, &c); out[k++ * N + i] = s; out[k++ * N + i] = c;
}

/* 64 threads: two warps */
__global__ void k_warp(const int *in, int *out)
{
    int t = threadIdx.x, l = t & 31, v = in[blockIdx.x * 64 + t], k = 0, r, any;
    float f = (float)v * 0.5f;
    int *o = out + (blockIdx.x * 64 + t) * 16;
    r = v; for (int d = 16; d > 0; d >>= 1) r += __shfl_xor_sync(0xffffffffu, r, d);
    o[k++] = r;
    r = v; for (int d = 16; d > 0; d >>= 1) r = max(r, __shfl_down_sync(0xffffffffu, r, d));
    o[k++] = r;
    o[k++] = __shfl_sync(0xffffffffu, v, (l * 5 + 3) & 31);
    o[k++] = __shfl_up_sync(0xffffffffu, v, 3);
    o[k++] = __shfl_down_sync(0xffffffffu, v, 30);
    o[k++] = __shfl_xor_sync(0xffffffffu, v, 3, 8);
    o[k++] = __shfl_sync(0xffffffffu, v, 9, 4);
    o[k++] = (int)__ballot_sync(0xffffffffu, v & 1);
    o[k++] = (int)(__shfl_xor_sync(0xffffffffu, f, 1) * 4.0f);
    o[k++] = (int)__shfl_down_sync(0xffffffffu, (double)v + 0.5, 1) + (int)__shfl_sync(0xffffffffu, (unsigned long long)v << 33, 0) ;
    o[k++] = __reduce_max_sync(0xffffffffu, v) - __reduce_min_sync(0xffffffffu, v) + __reduce_add_sync(0xffffffffu, v);
    o[k++] = (int)(__reduce_xor_sync(0xffffffffu, (unsigned)v) ^ __reduce_or_sync(0xffffffffu, (unsigned)v) ^ __reduce_and_sync(0xffffffffu, (unsigned)v | 0xf0));
    __syncwarp();
    any = __syncthreads_or(v == in[blockIdx.x * 64 + 40]);
    o[k++] = any + 2 * __syncthreads_or(v == 1000000) + 4 * __syncthreads_and(v > -1000) + 8 * __syncthreads_count(v & 1);
    o[k++] = (int)__match_any_sync(0xffffffffu, v & 3) ^ l;
    o[k++] = (int)__activemask();
    o[k++] = 0;
}

__global__ void k_atom(int *ia, unsigned *ua, float *fa, double *da, unsigned long long *la)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    atomicAdd(&ia[i % 7], i);          atomicSub(&ia[7 + i % 3], i);       atomicMax(&ia[10 + i % 5], (i * 37) % 1001);
    atomicMin(&ia[15 + i % 5], (i * 53) % 1009 - 500);                     atomicAdd(&ua[i % 4], 0x10000000u + i);
    atomicOr(&ua[4 + i % 3], 1u << (i % 29));                              atomicAnd(&ua[7 + i % 2], ~(1u << (i % 31)));
    atomicXor(&ua[9], (unsigned)i * 2654435761u);                          atomicMax(&ua[10], (unsigned)i * 2654435761u);
    atomicAdd(&fa[i % 5], (float)(i % 16));                                atomicAdd(&da[i % 3], (double)i * 0.5);
    atomicAdd(&la[i % 2], (unsigned long long)i << 30);                    atomicMax(&la[2], (unsigned long long)i * 0x100000001ull);
    if (atomicCAS(&ia[20], 0, i + 1) == 0) atomicAdd(&ia[21], 1);
    atomicExch(&ia[22], 77);
}

static float hin[N], hnear[NA * N];
static unsigned hout[NE * N];
static int hw[2 * 64], hwo[2 * 64 * 16];

int main(void)
{
    static const char *const en[] = {
#define X(n, e) #n,
        EXACT(X)
#undef X
    };
    static const char *const an[] = {
#define X(n, e, r) #n,
        NEAR(X)
#undef X
        "sincosf sin", "sincosf cos", "sincospif sin", "sincospif cos"
    };
    static const unsigned special[] = {0, 0x80000000u, 0x7f800000u, 0xff800000u, 0x7fc00000u, 1, 0x80000001u, 0x007fffffu, 0x00800000u,
        0x7f7fffffu, 0xff7fffffu, 0x3f800000u, 0xbf800000u, 0x3f000000u, 0xbf000000u, 0x3fc00000u, 0xbfc00000u, 0x40200000u, 0xc0200000u,
        0x4f000000u, 0xcf000000u, 0x4f800000u, 0x5f000000u, 0xdf000000u, 0x477fe000u, 0x477ff000u, 0x33000000u, 0x387fc000u, 0x00000400u, 0x4b000001u};
    float *din, *dnear, *fa; unsigned *dout, *ua; int *dw, *dwo, *ia; double *da; unsigned long long *la;
    unsigned s = 99; int ns = (int)(sizeof special / sizeof *special);
    int hia[23]; unsigned hua[11]; float hfa[7]; double hda[3]; unsigned long long hla[3];
    for (int i = 0; i < N; i++) {
        unsigned u;
        s = s * 1664525u + 1013904223u;
        if (i < ns) u = special[i];
        else if (i % 3 == 0) u = s;                                              /* anything */
        else if (i % 3 == 1) u = (s & 0x807fffffu) | (120 + (s >> 23) % 16) << 23;  /* around 1 */
        else { float f = (float)((int)(s >> 8) % 4001 - 2000) * 0.25f; memcpy(&u, &f, 4); }   /* quarters: ties */
        memcpy(&hin[i], &u, 4);
    }
    for (int i = 0; i < 128; i++) { s = s * 1664525u + 1013904223u; hw[i] = (int)(s >> 12) % 2001 - 1000; }
    cudaMalloc(&din, sizeof hin); cudaMalloc(&dout, sizeof hout); cudaMalloc(&dnear, sizeof hnear); cudaMalloc(&dw, sizeof hw); cudaMalloc(&dwo, sizeof hwo);
    cudaMalloc(&ia, sizeof hia); cudaMalloc(&ua, sizeof hua); cudaMalloc(&fa, sizeof hfa); cudaMalloc(&da, sizeof hda); cudaMalloc(&la, sizeof hla);
    cudaMemcpy(din, hin, sizeof hin, cudaMemcpyHostToDevice); cudaMemcpy(dw, hw, sizeof hw, cudaMemcpyHostToDevice);
    cudaMemset(ia, 0, sizeof hia); cudaMemset(ua, 0, sizeof hua); cudaMemset(fa, 0, sizeof hfa); cudaMemset(da, 0, sizeof hda); cudaMemset(la, 0, sizeof hla);
    for (int i = 0; i < 5; i++) hia[i] = 100000;
    cudaMemcpy(ia + 15, hia, 5 * sizeof(int), cudaMemcpyHostToDevice);
    hua[0] = hua[1] = 0xffffffffu; cudaMemcpy(ua + 7, hua, 2 * sizeof(unsigned), cudaMemcpyHostToDevice);
    k_exact<<<N / 128, 128>>>(din, dout);
    k_near<<<N / 128, 128>>>(dnear);
    k_warp<<<2, 64>>>(dw, dwo);
    k_atom<<<10, 100>>>(ia, ua, fa, da, la);
    if (cudaDeviceSynchronize() != cudaSuccess) { printf("failed: %s\n", cudaGetErrorString(cudaGetLastError())); return 1; }
    cudaMemcpy(hout, dout, sizeof hout, cudaMemcpyDeviceToHost); cudaMemcpy(hnear, dnear, sizeof hnear, cudaMemcpyDeviceToHost);
    cudaMemcpy(hwo, dwo, sizeof hwo, cudaMemcpyDeviceToHost);
    cudaMemcpy(hia, ia, sizeof hia, cudaMemcpyDeviceToHost); cudaMemcpy(hua, ua, sizeof hua, cudaMemcpyDeviceToHost);
    cudaMemcpy(hfa, fa, sizeof hfa, cudaMemcpyDeviceToHost); cudaMemcpy(hda, da, sizeof hda, cudaMemcpyDeviceToHost);
    cudaMemcpy(hla, la, sizeof hla, cudaMemcpyDeviceToHost);
    for (int k = 0; k < NE; k++) {
        unsigned h = 2166136261u;
        for (int i = 0; i < N; i++) h = (h ^ hout[k * N + i]) * 16777619u;
        printf("%-22s %08x\n", en[k], h);
        if (getenv("INTR_DUMP") && !strcmp(getenv("INTR_DUMP"), en[k]))
            for (int i = 0; i < N; i++) printf("  %3d %08x\n", i, hout[k * N + i]);
    }
    for (int k = 0; k < NA; k++) {
        double worst = 0; int wi = 0;
        for (int i = 0; i < N; i++) {
            double x = (double)((float)(i - N / 2) * (16.0f / N) + 0.001f), p = (double)((float)(i + 1) * (8.0f / (N + 1))), want = 0, got = hnear[k * N + i], ulp, e;
            int j = 0, ex;
            (void)p;
#define X(n, ex, r) if (k == j++) want = (r);
            NEAR(X)
#undef X
            if (k == A_sincosf_s) want = sin(x); if (k == A_sincosf_c) want = cos(x);
            if (k == A_sincospif_s) want = sin(3.14159265358979323846 * (double)((float)x * 0.25f));
            if (k == A_sincospif_c) want = cos(3.14159265358979323846 * (double)((float)x * 0.25f));
            frexp(want, &ex); ulp = ldexp(1.0, ex - 24);
            if (ulp < 1e-30) ulp = 1e-30;
            if (an[k][0] == '_') ulp = 4 * (ulp > 1.2e-7 ? ulp : 1.2e-7);   /* the fast ones: of a result near 1 */
            e = fabs(got - want) / ulp;
            if (!(e <= worst)) { worst = e; wi = i; }
        }
        if (worst <= 4.0) printf("%-22s near\n", an[k]);
        else printf("%-22s off by %.1f at %d: %.9g\n", an[k], worst, wi, (double)hnear[k * N + wi]);
    }
    for (int k = 0; k < 16; k++) {
        unsigned h = 2166136261u;
        for (int i = 0; i < 128; i++) h = (h ^ (unsigned)hwo[i * 16 + k]) * 16777619u;
        printf("warp %2d %08x\n", k, h);
    }
    printf("int "); for (int i = 0; i < 23; i++) printf(" %d", i == 20 ? hia[20] != 0 : hia[i]); printf("\n");
    printf("unsigned "); for (int i = 0; i < 11; i++) printf(" %x", hua[i]); printf("\n");
    printf("float "); for (int i = 0; i < 5; i++) printf(" %g", hfa[i]); printf("\n");
    printf("double  %g %g %g\nlong  %llx %llx %llx\n", hda[0], hda[1], hda[2], hla[0], hla[1], hla[2]);
    return 0;
}
