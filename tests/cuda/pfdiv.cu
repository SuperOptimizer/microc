/* __fdividef (div.approx) and the fences (membar.cta / gl / sys), from mcuda_device.h: before they were implicit externs no module
   defined (ufsm tanh_silu's e / (1 + e), kc16_k's __threadfence_system). Against the host's division, to a few ulp. */
#include <stdio.h>
#include <math.h>
#include <cuda_runtime.h>
__global__ void k(float *o, const float *a, unsigned *flag) {
    int t = threadIdx.x;
    float e = __expf(a[t]);
    o[t] = __fdividef(e, 1.f + e) + 0.f * __frcp_rn(1.f + e);
    __threadfence_block();
    __threadfence();
    if (t == 0) { flag[0] = 7u; __threadfence_system(); }
}
int main(void) {
    enum { N = 64 };
    float *o, *a;
    unsigned *flag;
    int bad = 0;
    cudaMallocManaged(&o, N * 4); cudaMallocManaged(&a, N * 4); cudaMallocManaged(&flag, 4);
    for (int t = 0; t < N; t++) a[t] = (t - 32) * 0.25f;
    k<<<1, N>>>(o, a, flag);
    printf("%d\n", (int)cudaDeviceSynchronize());
    for (int t = 0; t < N; t++) {
        const double r = exp((double)a[t]) / (1.0 + exp((double)a[t]));
        bad += fabs(o[t] - r) > 1e-5 * (1.0 + fabs(r));
    }
    printf("%d %u\n", bad, flag[0]);
    return 0;
}
