/* cudaSetDeviceFlags on the card (mcudahw): the scheduling flag reaches the primary context (cuCtxGetFlags says it), and a wait
   for a ~0.3 s kernel under cudaDeviceScheduleBlockingSync costs the waiting thread almost no CPU, where the default spins.
   Prints SKIP without a GPU. */
#include <cuda_runtime.h>
#include <stdio.h>
#include <time.h>

__device__ static unsigned long long clk64(void)
{
    unsigned long long t;
    asm volatile("mov.u64 %0, %%clock64;" : "=l"(t));
    return t;
}
__global__ void spin_k(unsigned long long cycles, unsigned *out)
{
    unsigned long long t0 = clk64();
    while (clk64() - t0 < cycles)
        ;
    if (threadIdx.x == 0) *out = 1;
}

static double cpu_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}
static double wall_now(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + 1e-9 * t.tv_nsec;
}

/* the CPU share of one synchronous wait for a kernel of about 0.3 s */
static double wait_share(unsigned *d, unsigned long long cyc)
{
    double c0, w0;
    spin_k<<<1, 32>>>(cyc, d);
    c0 = cpu_now();
    w0 = wall_now();
    cudaDeviceSynchronize();
    return (cpu_now() - c0) / (wall_now() - w0);
}

int main(void)
{
    int n = 0, clk = 0;
    unsigned *d = 0, fl = 99;
    unsigned long long cyc;
    int (*get_flags)(unsigned *) = 0;
    double spin, blk;
    if (cudaGetDeviceCount(&n) != cudaSuccess || n < 1) { printf("SKIP: no GPU\n"); return 0; }
    cudaDeviceGetAttribute(&clk, cudaDevAttrClockRate, 0); /* kHz */
    cyc = (unsigned long long)clk * 1000ull * 3 / 10;
    if (cudaMalloc((void **)&d, 4) != cudaSuccess) { printf("FAIL: cudaMalloc\n"); return 1; }
    wait_share(d, cyc / 30); /* (the kernel loaded and compiled first) */
    spin = wait_share(d, cyc);
    if (cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync) != cudaSuccess) { printf("FAIL: cudaSetDeviceFlags\n"); return 1; }
    cudaGetDriverEntryPointByVersion("cuCtxGetFlags", (void **)&get_flags, 12000, cudaEnableDefault, 0);
    if (get_flags) get_flags(&fl);
    blk = wait_share(d, cyc);
    printf("wait CPU share: default %.2f, blocking sync %.2f; context flags %u\n", spin, blk, fl);
    if (get_flags && (fl & 7) != 4) { printf("FAIL: the context's scheduling flags are %u, not blocking sync (4)\n", fl & 7); return 1; }
    if (blk > 0.2) { printf("FAIL: the blocking wait still burns %.2f of a CPU\n", blk); return 1; }
    printf("ok\n");
    return 0;
}
