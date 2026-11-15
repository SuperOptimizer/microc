/* nvcc: -arch=sm_120 */
/* __constant__ memory: two kernels (in mcudahw two modules of the driver's, each with a copy of the constants of its own)
   read one __constant__ array and a __constant__ struct with an initializer; cudaMemcpyToSymbol between launches (and
   cudaMemcpyToSymbolAsync on a stream, a part of the array at an offset) must reach both kernels' copies, those loaded
   before the copy and those loaded after; cudaMemcpyFromSymbol reads it back; a kernel that takes the address of a
   constant (it escapes into memory) reads it through a generic pointer. What it prints is the same on NVIDIA's run time. */
#include <cuda_runtime.h>
#include <stdio.h>

struct Par { float scale; int add; };
__constant__ float cw[32];
__constant__ Par cp = {2.0f, 7};

__global__ void ka(float *o, int n)
{
    int i = threadIdx.x;
    float s = 0.f;
#pragma unroll
    for (int k = 0; k < 8; k++)
        s += cw[k] * (float)(i + k);
    if (i < n)
        o[i] = s * cp.scale + (float)cp.add + cw[(i + 8) & 31];
}

__global__ void kb(float *o, int n)
{
    int i = threadIdx.x;
    if (i < n)
        o[i] = cw[i & 31] - cp.scale;
}

__global__ void kc(const float **pp) { *pp = &cw[5]; }
__global__ void kd(const float *const *pp, float *o) { o[0] = **pp + cp.scale; }

static int bad;
#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { printf("line %d: %s\n", __LINE__, cudaGetErrorString(e_)); bad++; } } while (0)

static void show(const char *what, float *d, int n)
{
    float h[32];
    double sum = 0;
    CK(cudaMemcpy(h, d, n * sizeof(float), cudaMemcpyDeviceToHost));
    for (int i = 0; i < n; i++) sum += (double)h[i] * (i + 1);
    if (n == 1)
        printf("%s: %.3f\n", what, h[0]);
    else
        printf("%s: %.3f %.3f %.3f %.3f sum %.3f\n", what, h[0], h[1], h[2], h[n - 1], sum);
}

int main(void)
{
    float h[32], *o;
    const float **pp;
    Par p;
    cudaStream_t s;
    CK(cudaMalloc((void **)&o, 32 * sizeof(float)));
    CK(cudaMalloc((void **)&pp, sizeof *pp));
    CK(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking));
    for (int i = 0; i < 32; i++) h[i] = 0.5f * i - 3.f;
    CK(cudaMemcpyToSymbol(cw, h, sizeof h));
    ka<<<1, 32>>>(o, 32); show("ka 1", o, 32);   /* ka loaded after the first copy */
    for (int i = 0; i < 32; i++) h[i] = 1.f + 0.25f * i;
    CK(cudaMemcpyToSymbol(cw, h, sizeof h));
    ka<<<1, 32>>>(o, 32); show("ka 2", o, 32);   /* ka's copy, loaded before this copy */
    kb<<<1, 32>>>(o, 32); show("kb 2", o, 32);   /* kb loaded after it */
    p.scale = -1.5f; p.add = 3;
    CK(cudaMemcpyToSymbol(cp, &p, sizeof p));
    for (int i = 0; i < 4; i++) h[i] = 100.f + i;
    CK(cudaMemcpyToSymbolAsync(cw, h, 4 * sizeof(float), 8 * sizeof(float), cudaMemcpyHostToDevice, s));
    ka<<<1, 32, 0, s>>>(o, 32); CK(cudaStreamSynchronize(s)); show("ka 3", o, 32);
    kb<<<1, 32, 0, s>>>(o, 32); CK(cudaStreamSynchronize(s)); show("kb 3", o, 32);
    kc<<<1, 1>>>(pp); kd<<<1, 1>>>(pp, o); show("kd", o, 1);
    CK(cudaMemcpyFromSymbol(h, cw, sizeof h));
    CK(cudaMemcpyFromSymbol(&p, cp, sizeof p));
    printf("back: %.2f %.2f %.2f %.2f / %.2f %d\n", h[0], h[8], h[11], h[31], p.scale, p.add);
    CK(cudaGetLastError());
    printf("constmem: %s\n", bad ? "FAIL" : "ok");
    return bad != 0;
}
