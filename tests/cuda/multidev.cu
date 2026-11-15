/* nvcc: -arch=sm_120 */
/* Several devices: cudaSetDevice / cudaGetDevice, a device global and a __constant__ per device (each starts from
   the image, then is the device's own), kernels and copies on each, cudaPointerGetAttributes' device, peer copies,
   a stream and an event on device 1, an invalid ordinal. With one device, device 1 is device 0 again and the
   checks are the same; what the program prints does not depend on the number of devices. MULTIDEV_MIN=N: fewer
   than N devices is a failure (a run time that shows one device of two passes otherwise). */
#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>

__constant__ int cmul = 3;
__device__ int gcount = 100;

__global__ void fill(int *p, int n)
{
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i < n)
        p[i] = i * cmul;
    if (i == 0)
        atomicAdd(&gcount, 1);
}

static int bad;
#define CK(x) do { cudaError_t e_ = (x); if (e_ != cudaSuccess) { printf("line %d: %s\n", __LINE__, cudaGetErrorString(e_)); bad++; } } while (0)

int main(void)
{
    enum { N = 1000 };
    int n = 0, d, cur = -1, h[N], i, k, c, g;
    int *buf[2] = {0, 0};
    cudaStream_t s;
    cudaEvent_t ev;
    struct cudaPointerAttributes pa;
    CK(cudaGetDeviceCount(&n));
    if (getenv("MULTIDEV_MIN") && n < atoi(getenv("MULTIDEV_MIN"))) { printf("%d devices\n", n); bad++; }
    if (cudaSetDevice(n) != cudaErrorInvalidDevice) { printf("device %d is not invalid\n", n); bad++; }
    cudaGetLastError();
    if (cudaSetDevice(-1) != cudaErrorInvalidDevice) { printf("device -1 is not invalid\n"); bad++; }
    cudaGetLastError();
    for (k = 0; k < 2; k++) {
        d = k < n ? k : 0;
        CK(cudaSetDevice(d));
        CK(cudaGetDevice(&cur));
        if (cur != d) { printf("current %d, not %d\n", cur, d); bad++; }
        CK(cudaMalloc((void **)&buf[k], N * sizeof(int)));
        CK(cudaPointerGetAttributes(&pa, buf[k]));
        if (pa.device != d) { printf("pointer on %d, not %d\n", pa.device, d); bad++; }
        if (k == 0 || n > 1) { /* (one device: device "1" is 0, whose constant is set) */
            CK(cudaMemcpyFromSymbol(&c, cmul, sizeof c));
            if (c != 3) { printf("device %d: cmul %d at first\n", d, c); bad++; }
        }
        c = 5 + k;
        CK(cudaMemcpyToSymbol(cmul, &c, sizeof c));
        fill<<<(N + 255) / 256, 256>>>(buf[k], N);
        CK(cudaGetLastError());
        CK(cudaMemcpy(h, buf[k], sizeof h, cudaMemcpyDeviceToHost));
        for (i = 0; i < N; i++)
            if (h[i] != i * (5 + k)) { printf("device %d: [%d] %d\n", d, i, h[i]); bad++; break; }
    }
    /* each device kept its own constant and global */
    CK(cudaSetDevice(0));
    CK(cudaMemcpyFromSymbol(&c, cmul, sizeof c));
    CK(cudaMemcpyFromSymbol(&g, gcount, sizeof g));
    if (c != (n > 1 ? 5 : 6) || g != (n > 1 ? 101 : 102)) { printf("device 0: cmul %d gcount %d\n", c, g); bad++; }
    if (n > 1) {
        CK(cudaSetDevice(1));
        CK(cudaMemcpyFromSymbol(&c, cmul, sizeof c));
        CK(cudaMemcpyFromSymbol(&g, gcount, sizeof g));
        if (c != 6 || g != 101) { printf("device 1: cmul %d gcount %d\n", c, g); bad++; }
    }
    /* a peer copy, then device 1's stream and event */
    {
        int p = 0, q = n > 1 ? 1 : 0;
        CK(cudaSetDevice(q));
        CK(cudaMemcpyPeer(buf[1], q, buf[0], p, N * sizeof(int)));
        CK(cudaStreamCreateWithFlags(&s, cudaStreamNonBlocking));
        CK(cudaEventCreateWithFlags(&ev, cudaEventDisableTiming));
        CK(cudaMemcpyPeerAsync(buf[1] + 1, q, buf[0], p, (N - 1) * sizeof(int), s));
        CK(cudaEventRecord(ev, s));
        CK(cudaEventSynchronize(ev));
        CK(cudaMemcpy(h, buf[1], sizeof h, cudaMemcpyDeviceToHost));
        for (i = 0; i < N; i++)
            if (h[i] != (i ? (i - 1) * 5 : 0)) { printf("peer copy: [%d] %d\n", i, h[i]); bad++; break; }
        CK(cudaStreamDestroy(s));
        CK(cudaEventDestroy(ev));
    }
    CK(cudaFree(buf[1]));
    CK(cudaSetDevice(0));
    CK(cudaFree(buf[0]));
    printf("multidev: %s\n", bad ? "FAIL" : "ok");
    return bad != 0;
}
