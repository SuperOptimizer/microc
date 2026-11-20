#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <stdio.h>
#include <string.h>
/* copies of CUDA's vector types through pointers (one ld/st.v2 / v4 per object), groups of scalar loads and stores
   of one base at consecutive places (shared and global), a struct of no alignment of its own (packed: its copies stay
   scalar), constant tables read at known and unknown places, and device functions large enough to be inlined only
   by the device limit */
struct __attribute__((packed)) pk { char c; float2 v; short s; };
struct hold { char c; float4 v; uint2 u; __half2 h; };
__device__ static const float TW[4][3] = {{0.375f, 0.625f, 0.f}, {0.125f, 0.75f, 0.125f}, {0.f, 0.625f, 0.375f}, {1.f, -2.f, 3.5f}};
__device__ static const int TI[8] = {3, 1, 4, 1, 5, 9, 2, 6};

__device__ float mix(const float *a, int n, float s) {
    float t = 0.f;
    for (int k = 0; k < n; k++) {
        float x = a[k] * s + (float)k;
        t += x > 0.f ? x * 0.5f : -x * 0.25f;
        t = t > 1000.f ? t - 1000.f : t;
    }
    for (int k = 0; k < 3; k++)
        t += TW[k][k] * (float)TI[k + 2];
    return t;
}

template <int R> __device__ float taps(const float *a) {
    float t = 0.f;
#pragma unroll
    for (int k = 0; k < 3; k++)
        t += TW[R][k] * a[k];
    return t;
}

__global__ void k(const float4 *f4, float4 *of4, const uint4 *u4, uint4 *ou4, const int2 *i2, int2 *oi2, const __half2 *h2,
                  __half2 *oh2, const pk *p, pk *op, const hold *hd, hold *ohd, float *o, int n, int var) {
    __shared__ float s[256];
    __shared__ float4 s4[64];
    int i = threadIdx.x;
    if (i >= n)
        return;
    float4 a = f4[i];
    a.y += 1.f;
    of4[i] = a;
    uint4 b = u4[i];
    b.w ^= 0x5a5a5a5au;
    ou4[i + 1] = b;
    int2 c = i2[i];
    c.x -= c.y;
    oi2[i] = c;
    __half2 h = h2[i];
    oh2[n - 1 - i] = h;
    pk q = p[i];
    q.v.x *= 2.f;
    q.s = (short)(q.s + 1);
    op[i] = q;
    hold w = hd[i];
    w.u.y += 7u;
    ohd[i] = w;
    s[4 * i] = a.x;
    s[4 * i + 1] = a.y;
    s[4 * i + 2] = a.z;
    s[4 * i + 3] = a.w;
    s4[i] = make_float4(0.f, 1.f, 0.f, (float)i);
    __syncthreads();
    int j = (i + 1) % n;
    float4 r = s4[j];
    float t = s[4 * j] + s[4 * j + 1] * 2.f + s[4 * j + 2] * 3.f + s[4 * j + 3] * 4.f + r.w + r.y;
    t += mix(s + 4 * j, 4, 0.5f);
    t += taps<0>(s + 4 * j) + taps<1>(s + 4 * j) + taps<3>(s + 4 * j);
    t += TW[var & 3][(var >> 2) % 3] + (float)TI[(i + var) & 7];
    float *oo = o + 4 * i;
    oo[0] = t;
    oo[1] = 0.f;
    oo[2] = (float)i;
    oo[3] = t * 0.5f;
}

int main() {
    const int n = 32;
    float4 *f4, *of4; uint4 *u4, *ou4; int2 *i2, *oi2; __half2 *h2, *oh2; pk *p, *op; hold *hd, *ohd; float *o;
    cudaMallocManaged(&f4, n * sizeof *f4); cudaMallocManaged(&of4, n * sizeof *of4);
    cudaMallocManaged(&u4, n * sizeof *u4); cudaMallocManaged(&ou4, (n + 1) * sizeof *ou4);
    cudaMallocManaged(&i2, n * sizeof *i2); cudaMallocManaged(&oi2, n * sizeof *oi2);
    cudaMallocManaged(&h2, n * sizeof *h2); cudaMallocManaged(&oh2, n * sizeof *oh2);
    cudaMallocManaged(&p, n * sizeof *p); cudaMallocManaged(&op, n * sizeof *op);
    cudaMallocManaged(&hd, n * sizeof *hd); cudaMallocManaged(&ohd, n * sizeof *ohd);
    cudaMallocManaged(&o, 4 * n * sizeof *o);
    for (int i = 0; i < n; i++) {
        f4[i].x = 0.5f * i; f4[i].y = -1.f * i; f4[i].z = 3.f; f4[i].w = 0.25f * i * i;
        u4[i].x = 7u * i; u4[i].y = 0xdeadbeefu ^ i; u4[i].z = i << 20; u4[i].w = ~i;
        i2[i].x = 100 * i; i2[i].y = -3 * i;
        unsigned short hb[2] = {(unsigned short)(0x3c00 + i), (unsigned short)(0xbc00 + 2 * i)};
        memcpy(&h2[i], hb, 4);
        memset(&p[i], 0, sizeof p[i]); p[i].c = (char)i; p[i].v.x = 1.5f * i; p[i].v.y = -0.5f * i; p[i].s = (short)(1000 - i);
        memset(&hd[i], 0, sizeof hd[i]); hd[i].c = (char)(i + 1); hd[i].v.x = i; hd[i].v.y = 2 * i; hd[i].v.z = 3 * i; hd[i].v.w = 4 * i;
        hd[i].u.x = 11u * i; hd[i].u.y = 13u * i; memcpy(&hd[i].h, hb, 4);
    }
    memset(ou4, 0, (n + 1) * sizeof *ou4); memset(op, 0, n * sizeof *op); memset(ohd, 0, n * sizeof *ohd);
    k<<<1, n>>>(f4, of4, u4, ou4, i2, oi2, h2, oh2, p, op, hd, ohd, o, n, 6);
    cudaError_t e = cudaDeviceSynchronize();
    unsigned long long hsum = 0;
    const unsigned char *bufs[] = {(const unsigned char *)of4, (const unsigned char *)ou4, (const unsigned char *)oi2,
                                   (const unsigned char *)oh2, (const unsigned char *)op, (const unsigned char *)o};
    size_t lens[] = {n * sizeof *of4, (n + 1) * sizeof *ou4, n * sizeof *oi2, n * sizeof *oh2, n * sizeof *op, 4 * n * sizeof *o};
    for (int b = 0; b < 6; b++) {
        unsigned long long h = 1469598103934665603ull;
        for (size_t x = 0; x < lens[b]; x++) h = (h ^ bufs[b][x]) * 1099511628211ull;
        printf("%d %llu\n", b, h % 1000000007ull);
        hsum += h;
    }
    /* hold: its padding is not copied by every compiler; the members */
    unsigned long long h = 0;
    for (int i = 0; i < n; i++) {
        unsigned hh; memcpy(&hh, &ohd[i].h, 4);
        h = h * 131 + (unsigned)ohd[i].c; h = h * 131 + (unsigned)ohd[i].v.x + (unsigned)ohd[i].v.w; h = h * 131 + ohd[i].u.x + ohd[i].u.y; h = h * 131 + hh;
    }
    printf("hold %llu\n", h % 1000000007ull);
    printf("o0 %.4f o31 %.4f err %d\n", o[0], o[4 * 31], (int)e);
    return 0;
}
