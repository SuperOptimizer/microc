/* opt.c hoist_to: a value worked out the same way in sibling blocks (the arms of an unrolled per-channel condition)
   is worked out once in the block before them. Against the same code on the host: arms taken and not taken per
   thread, values whose operands are only known inside the arm (stay), one inside a loop (stays in the loop). */
#include <stdio.h>
#include <cuda_runtime.h>

static float host_body(int t, int a, int b, int c, float s, float o, unsigned m) {
    float acc = 0.f;
    int ia = 0;
#pragma unroll
    for (int q = 0; q < 12; q++) {
        if (m >> q & 1) {
            int base = a * b + c;                  /* the same in every arm */
            float sc = s * (float)base + o;        /* and this */
            int own = t * q + base;                /* differs per arm */
            ia += own % 7 + base / 3;
            acc += sc * (float)(own & 15);
        } else if (m >> (q + 12) & 1) {
            int base = a * b + c;
            ia -= base ^ q;
        }
    }
#pragma unroll 1
    for (int k = 0; k < (t & 7); k++) {
        if (k & 1) ia += a * c + k; else ia -= a * c - k;   /* (in a loop: stays there) */
    }
    return (float)ia + acc;
}
__device__ __forceinline__ float dev_body(int t, int a, int b, int c, float s, float o, unsigned m) {
    float acc = 0.f;
    int ia = 0;
#pragma unroll
    for (int q = 0; q < 12; q++) {
        if (m >> q & 1) {
            int base = a * b + c;                  /* the same in every arm */
            float sc = s * (float)base + o;        /* and this */
            int own = t * q + base;                /* differs per arm */
            ia += own % 7 + base / 3;
            acc += sc * (float)(own & 15);
        } else if (m >> (q + 12) & 1) {
            int base = a * b + c;
            ia -= base ^ q;
        }
    }
#pragma unroll 1
    for (int k = 0; k < (t & 7); k++) {
        if (k & 1) ia += a * c + k; else ia -= a * c - k;   /* (in a loop: stays there) */
    }
    return (float)ia + acc;
}
__global__ void k_hoist(float *out, int a, int b, int c, float s, float o, const unsigned *mask) {
    int t = threadIdx.x;
    out[t] = dev_body(t, a, b, c, s, o, mask[t]);
}
int main(void) {
    enum { N = 64 };
    float *out;
    unsigned *mask;
    int bad = 0, a = 7, b = -3, c = 11;
    float s = 0.25f, o = -1.5f;
    double sum = 0;
    cudaMallocManaged(&out, N * 4); cudaMallocManaged(&mask, N * 4);
    for (int t = 0; t < N; t++) mask[t] = (unsigned)(t * 2654435761u) >> 3;
    for (int r = 0; r < 2; r++) {
        k_hoist<<<1, N>>>(out, a, b, c, s, o, mask);
        printf("%d\n", (int)cudaDeviceSynchronize());
        for (int t = 0; t < N; t++) {
            bad += out[t] != host_body(t, a, b, c, s, o, mask[t]);
            sum += out[t];
        }
        a = -5; b = 9; s = 3.f;
    }
    printf("%.1f %d\n", sum, bad);
    return 0;
}
