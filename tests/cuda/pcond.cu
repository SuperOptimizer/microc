/* parse.c cond_expr in device code: a record chosen by ?: is copied in each arm (not chosen by its address). Against
   the host: a float2 from shared memory or made in place (lp_s2.cu:166's form), a three-field record, nested ?:, the
   result changed afterwards (the chosen object stays as it was), and a const reference bound to ?: of two objects. */
#include <stdio.h>
#include <cuda_runtime.h>
typedef struct { int a; float b; short c; } rec3;

static float host_one(int t, const float2 *bank, const float2 *tab, const rec3 *rs) {
    float s = 0.f;
    for (int k = 0; k < 8; k++) {
        const float2 ab = t + k >= 20 ? bank[k] : make_float2(tab[k].x, tab[k].y);
        s += ab.x * 3.f + ab.y;
        rec3 r = (t & k) ? rs[k] : (k & 1) ? rs[7 - k] : rec3{k, -1.5f, (short)t};
        r.a += 1;
        s += (float)r.a + r.b + (float)r.c;
        const float2 &q = (t ^ k) & 2 ? bank[k] : tab[k];
        s += q.x - q.y;
    }
    return s + (float)rs[3].a;
}

__global__ void k_cond(float *out, const float2 *gtab, const rec3 *grs) {
    __shared__ float2 bank[8];
    __shared__ rec3 rs[8];
    int t = threadIdx.x;
    if (t < 8) { bank[t] = make_float2(0.5f * t, -t); rs[t] = grs[t]; }
    __syncthreads();
    float s = 0.f;
#pragma unroll
    for (int k = 0; k < 8; k++) {
        const float2 ab = t + k >= 20 ? bank[k] : make_float2(gtab[k].x, gtab[k].y);
        s += ab.x * 3.f + ab.y;
        rec3 r = (t & k) ? rs[k] : (k & 1) ? rs[7 - k] : rec3{k, -1.5f, (short)t};
        r.a += 1;
        s += (float)r.a + r.b + (float)r.c;
        const float2 &q = (t ^ k) & 2 ? bank[k] : gtab[k];
        s += q.x - q.y;
    }
    out[t] = s + (float)rs[3].a;
}
int main(void) {
    enum { N = 64 };
    float *out;
    float2 *tab, bank[8];
    rec3 *rs;
    int bad = 0;
    double sum = 0;
    cudaMallocManaged(&out, N * 4); cudaMallocManaged(&tab, 8 * sizeof(float2)); cudaMallocManaged(&rs, 8 * sizeof(rec3));
    for (int k = 0; k < 8; k++) { tab[k] = make_float2(k * 0.25f, 2.f - k); rs[k] = rec3{10 * k, 0.75f * k, (short)(-k)}; bank[k] = make_float2(0.5f * k, -k); }
    k_cond<<<1, N>>>(out, tab, rs);
    printf("%d\n", (int)cudaDeviceSynchronize());
    for (int t = 0; t < N; t++) {
        bad += out[t] != host_one(t, bank, tab, rs);
        sum += out[t];
    }
    printf("%.2f %d\n", sum, bad);
    return 0;
}
