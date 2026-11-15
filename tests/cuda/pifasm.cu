/* opt.c cheap_block in device code: an arm holding one MUFU inline asm (act_ab's SiLU through tanh.approx behind "if (!G) return v")
   is if-converted at the default MCC_IFC (MCC_NOIFASM: kept as a branch). Against the host: the G-off lanes return v unchanged, the
   G-on lanes h + h tanh(h) (tanh.approx: compared with a tolerance), with G varying across the warp. */
#include <stdio.h>
#include <math.h>
#include <cuda_runtime.h>
__device__ __forceinline__ float act(float v, float a, float b, bool G) {
    if (!G) return v;
    v = fmaf(v, a, b);
    const float h = 0.5f * v;
    float th; asm("tanh.approx.f32 %0, %1;" : "=f"(th) : "f"(h));
    return fmaf(h, th, h);
}
__global__ void k_act(float *out, const float *in, unsigned gmask) {
    int t = threadIdx.x;
    float s = 0.f;
#pragma unroll
    for (int k = 0; k < 4; k++) s += act(in[t] + k, 0.5f + 0.25f * k, -0.125f * k, (gmask >> ((t + k) & 31)) & 1u);
    out[t] = s;
}
int main(void) {
    enum { N = 64 };
    float *out, *in;
    int bad = 0;
    cudaMallocManaged(&out, N * 4); cudaMallocManaged(&in, N * 4);
    for (int t = 0; t < N; t++) in[t] = (t - 32) * 0.15625f;
    const unsigned gm = 0x5a3c96f1u;
    k_act<<<1, N>>>(out, in, gm);
    printf("%d\n", (int)cudaDeviceSynchronize());
    for (int t = 0; t < N; t++) {
        float s = 0.f;
        for (int k = 0; k < 4; k++) {
            float v = in[t] + k;
            if ((gm >> ((t + k) & 31)) & 1u) { v = v * (0.5f + 0.25f * k) - 0.125f * k; const float h = 0.5f * v; v = h + h * tanhf(h); }
            s += v;
        }
        bad += fabsf(out[t] - s) > 2e-3f * (1.f + fabsf(s));
    }
    printf("%d\n", bad);
    return 0;
}
