/* What the x86-64 translator does itself or calls the run time for (cuda/jitx64.c: float to integer and to an
   integral float in every rounding, division, the atomic operations, library calls) at the values where the
   processor's instruction and PTX part: NaNs, infinities, zeros of both signs, subnormals, halves, the limits of
   int and long. Every result's bits go into a sum per kernel; the sums expected are the interpreter's
   (MCUDA_JIT=0), which the translated code has to match bit for bit. */
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <cuda_runtime.h>

#define NV 48
#define MIX(h, v) ((h) = ((h) ^ (unsigned long long)(v)) * 0x100000001b3ull + 0x9e3779b97f4a7c15ull)

__device__ static unsigned fbits(float f) { unsigned u; memcpy(&u, &f, 4); return u; }
__device__ static unsigned long long dbits(double d) { unsigned long long u; memcpy(&u, &d, 8); return u; }

__global__ void toint(const float *f, const double *d, unsigned long long *out)
{
    int i = threadIdx.x;
    unsigned long long h = 1;
    MIX(h, (unsigned)(int)lrintf(f[i]));
    MIX(h, (unsigned long long)lrintf(f[i]));
    MIX(h, (unsigned long long)llrint(d[i]));
    MIX(h, (unsigned)(int)lrint(d[i]));
    MIX(h, (unsigned)(int)f[i]);
    MIX(h, (unsigned long long)(long long)d[i]);
    MIX(h, (unsigned)(int)lroundf(f[i]));
    MIX(h, (unsigned)(unsigned)f[i]);
    out[i] = h;
}
__global__ void tofloat(const float *f, const double *d, unsigned long long *out)
{
    int i = threadIdx.x;
    unsigned long long h = 2;
    MIX(h, fbits(floorf(f[i])));
    MIX(h, fbits(ceilf(f[i])));
    MIX(h, fbits(truncf(f[i])));
    MIX(h, fbits(rintf(f[i])));
    MIX(h, fbits(nearbyintf(f[i])));
    MIX(h, fbits(roundf(f[i])));
    MIX(h, dbits(floor(d[i])));
    MIX(h, dbits(ceil(d[i])));
    MIX(h, dbits(trunc(d[i])));
    MIX(h, dbits(rint(d[i])));
    MIX(h, dbits(round(d[i])));
    out[i] = h;
}
__global__ void divide(const float *f, const double *d, unsigned long long *out)
{
    int i = threadIdx.x, j = (i * 7 + 3) % NV;
    unsigned long long h = 3;
    float q = f[i] / f[j];
    double p = d[i] / d[j];
    /* (a NaN's sign and payload are the device's own affair: only that it is one) */
    MIX(h, q != q ? 0x7fc00000u : fbits(q));
    MIX(h, p != p ? 0x7ff8000000000000ull : dbits(p));
    MIX(h, fbits(fminf(f[i], f[j]) == fminf(f[i], f[j]) ? fminf(f[i], f[j]) : 0.0f));
    MIX(h, fbits(fmaxf(f[i], f[j]) == fmaxf(f[i], f[j]) ? fmaxf(f[i], f[j]) : 0.0f));
    out[i] = h;
}
/* every thread on the same few words: the totals do not depend on the order, the returned values do not go in */
__global__ void atoms(int *si, unsigned *ui, unsigned long long *ul, float *fl, int *mn, int *mx, unsigned *bits, unsigned long long *won)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    atomicAdd(si, i - 100);
    atomicSub(ui, (unsigned)i * 3u);
    atomicAdd(ul, 0x100000001ull * (unsigned)i);
    atomicAdd(fl, 0.25f); /* (quarters: exact in any order) */
    atomicMin(mn, 1000 - i * 5);
    atomicMax(mx, i * i - 300);
    atomicOr(&bits[0], 1u << (i & 31));
    atomicAnd(&bits[1], ~(1u << (i % 29)));
    atomicXor(&bits[2], 0x9e3779b9u * (unsigned)(i + 1));
    atomicMin(&bits[3], 5000u - (unsigned)i);
    atomicMax(&bits[4], (unsigned)i << 20);
    if (atomicCAS(&bits[5], 0u, (unsigned)i + 1) == 0u)
        atomicAdd(won, 1ull); /* one thread wins */
    atomicExch(&bits[6], 77u);
}
__global__ void calls(const float *f, unsigned char *buf, unsigned long long *out)
{
    int i = threadIdx.x, k;
    unsigned long long h = 5;
    unsigned char tmp[16];
    float e = expf(f[i] * 0.001f), s = sqrtf(fabsf(f[i])), l = logf(fabsf(f[i]) + 1.0f);
    memset(tmp, i, sizeof tmp);
    memcpy(tmp + 3, buf + (i & 7), 9);
    for (k = 0; k < 16; k++)
        MIX(h, tmp[k]);
    MIX(h, e == e ? fbits(e) >> 3 : 1); /* (the last bits of exp and log are the library's) */
    MIX(h, s == s ? fbits(s) : 2);
    MIX(h, l == l ? fbits(l) >> 3 : 3);
    MIX(h, isnan(f[i]) + 2 * isinf(f[i]));
    out[i] = h;
}

static unsigned long long fold(const unsigned long long *o)
{
    unsigned long long h = 7;
    int i;
    for (i = 0; i < NV; i++)
        MIX(h, o[i]);
    return h;
}

int main(void)
{
    static const float edge[NV] = {
        0.0f, -0.0f, 0.5f, -0.5f, 1.5f, -1.5f, 2.5f, -2.5f, 0.49999997f, -0.49999997f, 0.50000006f, 1.0f, -1.0f, 3.7f, -3.7f,
        1e-45f, -1e-45f, 1e-39f, 1.17549435e-38f, 8388607.5f, 8388608.0f, -8388608.5f, 16777216.0f, 2147483520.0f, 2147483648.0f,
        -2147483648.0f, -2147483904.0f, 4294967040.0f, 4294967296.0f, 9223371487098961920.0f, 9223372036854775808.0f,
        -9223372036854775808.0f, -9223373136366403584.0f, 1.8446744e19f, 3.4028235e38f, -3.4028235e38f, 1e30f, -1e30f,
        255.5f, 256.5f, -255.5f, 1e9f, 123456.789f, -0.75f, 0.75f, 7.0f, 0.0f, 0.0f };
    float hf[NV], *f, *fl, hfl;
    double hd[NV], *d;
    unsigned long long ho[NV], *o, *ul, *won, hul, hwon;
    unsigned char hbuf[32], *buf;
    int *si, *mn, *mx, hsi, hmn, hmx, k;
    unsigned *ui, *bits, hui, hbits[8];
    memcpy(hf, edge, sizeof hf);
    hf[NV - 2] = INFINITY; hf[NV - 1] = -INFINITY; hf[NV - 3] = NAN;
    for (k = 0; k < NV; k++)
        hd[k] = (double)hf[k];
    hd[5] = 4503599627370496.5; hd[6] = -4503599627370495.5; hd[7] = 9223372036854775808.0; hd[8] = -9223372036854777856.0;
    hd[9] = 2147483647.5; hd[10] = -2147483648.5; hd[11] = 2147483648.5; hd[12] = 4.9e-324; hd[13] = 1.7976931348623157e308;
    for (k = 0; k < 32; k++)
        hbuf[k] = (unsigned char)(k * 11 + 1);
    cudaMalloc(&f, sizeof hf); cudaMalloc(&d, sizeof hd); cudaMalloc(&o, sizeof ho); cudaMalloc(&buf, sizeof hbuf);
    cudaMemcpy(f, hf, sizeof hf, cudaMemcpyHostToDevice);
    cudaMemcpy(d, hd, sizeof hd, cudaMemcpyHostToDevice);
    cudaMemcpy(buf, hbuf, sizeof hbuf, cudaMemcpyHostToDevice);
    toint<<<1, NV>>>(f, d, o);
    cudaMemcpy(ho, o, sizeof ho, cudaMemcpyDeviceToHost);
    printf("toint %016llx\n", fold(ho));
    tofloat<<<1, NV>>>(f, d, o);
    cudaMemcpy(ho, o, sizeof ho, cudaMemcpyDeviceToHost);
    printf("tofloat %016llx\n", fold(ho));
    divide<<<1, NV>>>(f, d, o);
    cudaMemcpy(ho, o, sizeof ho, cudaMemcpyDeviceToHost);
    printf("divide %016llx\n", fold(ho));
    calls<<<1, NV>>>(f, buf, o);
    cudaMemcpy(ho, o, sizeof ho, cudaMemcpyDeviceToHost);
    printf("calls %016llx\n", fold(ho));
    cudaMalloc(&si, 4); cudaMalloc(&ui, 4); cudaMalloc(&ul, 8); cudaMalloc(&fl, 4); cudaMalloc(&mn, 4); cudaMalloc(&mx, 4);
    cudaMalloc(&bits, sizeof hbits); cudaMalloc(&won, 8);
    hsi = 5; hui = 7; hul = 9; hfl = 1.0f; hmn = 100000; hmx = -100000; hwon = 0;
    memset(hbits, 0, sizeof hbits);
    hbits[1] = 0xffffffffu; hbits[3] = 0xffffffffu;
    cudaMemcpy(si, &hsi, 4, cudaMemcpyHostToDevice); cudaMemcpy(ui, &hui, 4, cudaMemcpyHostToDevice);
    cudaMemcpy(ul, &hul, 8, cudaMemcpyHostToDevice); cudaMemcpy(fl, &hfl, 4, cudaMemcpyHostToDevice);
    cudaMemcpy(mn, &hmn, 4, cudaMemcpyHostToDevice); cudaMemcpy(mx, &hmx, 4, cudaMemcpyHostToDevice);
    cudaMemcpy(bits, hbits, sizeof hbits, cudaMemcpyHostToDevice); cudaMemcpy(won, &hwon, 8, cudaMemcpyHostToDevice);
    atoms<<<8, 96>>>(si, ui, ul, fl, mn, mx, bits, won);
    cudaMemcpy(&hsi, si, 4, cudaMemcpyDeviceToHost); cudaMemcpy(&hui, ui, 4, cudaMemcpyDeviceToHost);
    cudaMemcpy(&hul, ul, 8, cudaMemcpyDeviceToHost); cudaMemcpy(&hfl, fl, 4, cudaMemcpyDeviceToHost);
    cudaMemcpy(&hmn, mn, 4, cudaMemcpyDeviceToHost); cudaMemcpy(&hmx, mx, 4, cudaMemcpyDeviceToHost);
    cudaMemcpy(hbits, bits, sizeof hbits, cudaMemcpyDeviceToHost); cudaMemcpy(&hwon, won, 8, cudaMemcpyDeviceToHost);
    printf("atoms %d %u %llu %.2f %d %d %08x %08x %08x %u %u %d %u %llu\n", hsi, hui, hul, hfl, hmn, hmx, hbits[0], hbits[1], hbits[2],
           hbits[3], hbits[4], hbits[5] != 0, hbits[6], hwon);
    return 0;
}
