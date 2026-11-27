/* A kernel that calls a device function nothing defines must stop the program, not leave its output unwritten behind a latched
   launch error (11-27: ufsm's four_tab_k called sincospi, the device library had no double sinpi; the test went on and failed on
   garbage). The kernel runs in a child: it must not come back from the launch and the wait as if nothing happened. On the GPU's run
   time (mcudahw) the child aborts at the first launch ("mcuda: fatal: kernel ... cannot be loaded"); on the CPU's the call faults. */
#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

extern "C" __device__ int mcuda_test_nothing_defines_this(int);

__global__ void undef_k(int *p) { p[threadIdx.x] = mcuda_test_nothing_defines_this(p[threadIdx.x]); }

int main(void)
{
    int st = 0;
    pid_t pid = fork();
    if (pid == 0) {
        int *d = 0;
        cudaMalloc((void **)&d, 128);
        undef_k<<<1, 32>>>(d);
        cudaGetLastError();               /* (a program that latches and goes on) */
        cudaDeviceSynchronize();
        _exit(0);                         /* came back: the failure was silent */
    }
    waitpid(pid, &st, 0);
    printf(WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "undefined device function: SILENT\n" : "undefined device function: stopped\n");
    return 0;
}
