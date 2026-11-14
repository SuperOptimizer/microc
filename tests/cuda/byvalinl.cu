#include <cuda_runtime.h>
#include <stdio.h>
/* a structure passed by value after the register arguments are used up (in memory on x86-64), to an
   inlined function and to one that is not; copied on from a local to memory narrower than its members */
struct q4 { unsigned x, y, z, w; };
struct w2 { unsigned long long a, b; };
__device__ __forceinline__ void put4(unsigned *o, int p, int bm, int c, int h, q4 v) { *(q4 *)(o + p * bm + c + h) = v; }
__device__ __noinline__ unsigned sum4(unsigned *o, int p, int bm, int c, int h, q4 v) { v.x += (unsigned)(p + bm + c + h); return v.x + v.y + v.z + v.w + o[0]; }
__global__ void k(unsigned *o, const q4 *in, const w2 *in2, int n) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    q4 v = in[i];
    v.z ^= 5u;
    put4(o, i, 16, 1, 1, v);
    w2 t = in2[i];
    t.a += 1;
    *(w2 *)(void *)(o + i * 16 + 6) = t;
    o[i * 16] = sum4(o + i * 16 + 2, i, 16, 1, 1, v);
    o[i * 16 + 1] = (unsigned)(t.a >> 32) + (unsigned)t.b;
}
int main() {
    unsigned *o; q4 *in; w2 *in2; unsigned long long h = 0; int bad = 0;
    cudaMallocManaged(&o, 8 * 16 * sizeof *o); cudaMallocManaged(&in, 8 * sizeof *in); cudaMallocManaged(&in2, 8 * sizeof *in2);
    for (int i = 0; i < 8; i++) { in[i].x = i; in[i].y = 10 * i; in[i].z = 100 * i; in[i].w = 1000 * i; in2[i].a = 0xffffffffull + i; in2[i].b = 7 * i; }
    for (int i = 0; i < 8 * 16; i++) o[i] = 0;
    k<<<1, 8>>>(o, in, in2, 8); cudaDeviceSynchronize();
    for (int i = 0; i < 8; i++) {
        unsigned *r = o + i * 16, z = (100u * i) ^ 5u;
        unsigned long long a = 0xffffffffull + i + 1;
        bad += r[2] != (unsigned)i; bad += r[3] != 10u * i; bad += r[4] != z; bad += r[5] != 1000u * i;
        bad += r[6] != (unsigned)a; bad += r[7] != (unsigned)(a >> 32); bad += r[8] != 7u * i; bad += r[9] != 0;
        bad += r[0] != (unsigned)i + (unsigned)(i + 18) + 10u * i + z + 1000u * i + (unsigned)i;
        bad += r[1] != (unsigned)(a >> 32) + 7u * i;
    }
    for (int i = 0; i < 8 * 16; i++) h = h * 131 + o[i];
    printf("%d %llu\n", bad, h % 1000000007ull);
    return 0;
}
