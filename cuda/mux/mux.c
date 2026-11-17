/* One CUDA run-time library that holds two: mcuda, which runs the kernels' PTX on the CPU (cuda/), and mcudahw,
   which gives it to NVIDIA's driver (cuda/hw). mkcuda.sh puts both into libcudart.a for x86-64 Linux, every
   global name of the first with mcpu_ before it and of the second with mgpu_ (cuda/mux/symprefix.c), and makes
   of each public name (cuda*, cublas*, cu*, __mcuda_*) a few instructions that jump through a slot (stubs.S,
   written by mkcuda.sh; first.S). The slots are filled at the first call of any of them, here:
     the GPU's run time, when the driver (libcuda.so.1) can be loaded and finds a device;
     the CPU's otherwise.
   MCUDA_BACKEND=cpu or gpu in the environment decides instead (gpu: without a driver or a device every call
   then fails as it does on a machine without one); MCUDA_VERBOSE=1 says which it is.
   A driver that is there and fails (it does not load: a name our C library lacks, thread-local storage; or
   its cuInit gives an error) is not a machine without one: the CPU's run time is taken, but with a line on
   standard error that says why, always; and under MCUDA_BACKEND=gpu the program stops there. */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct __mcuda_module;
extern void *__mcuda_mux_cpu[], *__mcuda_mux_gpu[], *__mcuda_mux_slot[];
extern const int __mcuda_mux_n;
void mcpu___mcuda_register(struct __mcuda_module *m);
void mgpu___mcuda_register(struct __mcuda_module *m);
int mgpu_cudaGetDeviceCount(int *n);
const char *mgpu_hw_why(void);

void __mcuda_mux_init(void);
void __mcuda_mux_missing(const char *name);
const char *mcuda_backend(void);
void __mcuda_register(struct __mcuda_module *m);

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int chosen; /* 0: not yet, 1: the CPU, 2: the GPU */

/* (first.S calls this with the arguments of the call that came first saved) */
void __mcuda_mux_init(void)
{
    const char *e, *v, *why;
    int gpu, n = 0, i, r = 0;
    pthread_mutex_lock(&lock);
    if (!chosen) {
        e = getenv("MCUDA_BACKEND");
        if (e && !strcmp(e, "cpu"))
            gpu = 0;
        else {
            r = mgpu_cudaGetDeviceCount(&n);
            gpu = r == 0 && n > 0;
            why = mgpu_hw_why();
            if (e && !strcmp(e, "gpu")) {
                if (!gpu && why[0]) {
                    fprintf(stderr, "mcuda: %s; MCUDA_BACKEND=gpu, so this is the end (MCUDA_BACKEND=cpu runs it on the CPU)\n", why);
                    exit(1);
                }
                gpu = 1;
            } else if (!gpu && why[0])
                fprintf(stderr, "mcuda: %s; the CPU run time is used instead (MCUDA_BACKEND=gpu makes this an error, "
                                "MCUDA_BACKEND=cpu chooses the CPU without asking the driver)\n", why);
        }
        v = getenv("MCUDA_VERBOSE");
        if (v && v[0] && v[0] != '0') {
            if (e && (!strcmp(e, "cpu") || !strcmp(e, "gpu")))
                fprintf(stderr, "mcuda: the %s run time (MCUDA_BACKEND)\n", gpu ? "GPU" : "CPU");
            else if (gpu)
                fprintf(stderr, "mcuda: the GPU run time (the driver has %d device%s)\n", n, n == 1 ? "" : "s");
            else
                fprintf(stderr, "mcuda: the CPU run time (no driver or no device: error %d)\n", r);
        }
        for (i = 0; i < __mcuda_mux_n; i++)
            __mcuda_mux_slot[i] = gpu ? __mcuda_mux_gpu[i] : __mcuda_mux_cpu[i];
        chosen = gpu ? 2 : 1;
    }
    pthread_mutex_unlock(&lock);
}

/* "cpu" or "gpu": the run time in use (the choice is made now if no call has made it) */
const char *mcuda_backend(void)
{
    if (!chosen)
        __mcuda_mux_init();
    return chosen == 2 ? "gpu" : "cpu";
}

/* A unit's device code announces itself before main: to both, so that nothing is chosen (and no driver
   loaded) by a program that then never uses the device. */
void __mcuda_register(struct __mcuda_module *m)
{
    mcpu___mcuda_register(m);
    mgpu___mcuda_register(m);
}

/* a function that only one of the two has, called with the other chosen */
void __mcuda_mux_missing(const char *name)
{
    fprintf(stderr, "mcuda: %s is not in the %s run time (MCUDA_BACKEND chooses the other)\n", name, chosen == 2 ? "GPU" : "CPU");
    abort();
}
