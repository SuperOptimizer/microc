/* mcudahw: the driver, opened at run time; the device's primary context; the per-thread state. */
#include "hw.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct HwDriver hw;
CUdevice hw_dev;
CUhandle hw_ctx;

typedef struct {
    int current; /* the context was made this thread's */
    int err;     /* the last error */
} Tls;

static pthread_key_t key;
static pthread_once_t key_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t mod_lock = PTHREAD_MUTEX_INITIALIZER;
static int state; /* 0: not tried, 1: open, 2: failed with init_err */
static int init_err;

static void key_make(void) { pthread_key_create(&key, free); }

static Tls *tls(void)
{
    Tls *t;
    pthread_once(&key_once, key_make);
    t = (Tls *)pthread_getspecific(key);
    if (!t) {
        t = (Tls *)calloc(1, sizeof *t);
        pthread_setspecific(key, t);
    }
    return t;
}

void hw_err_set(int e)
{
    static int verbose = -1;
    Tls *t = tls();
    if (t)
        t->err = e;
    if (verbose < 0) {
        const char *v = getenv("MCUDA_VERBOSE");
        verbose = v && v[0] && v[0] != '0';
    }
    if (verbose && e)
        fprintf(stderr, "mcuda: error %d\n", e);
}

int hw_err_get(int clear)
{
    Tls *t = tls();
    int e = t ? t->err : 0;
    if (t && clear)
        t->err = 0;
    return e;
}

void hw_lock(void) { pthread_mutex_lock(&mod_lock); }
void hw_unlock(void) { pthread_mutex_unlock(&mod_lock); }

void *hw_entry(const char *name) { return hw.lib ? dlsym(hw.lib, name) : 0; }

#define HW_NAME(m, name, args) name,
static const char *const names[] = {HW_DRIVER(HW_NAME) 0};

static int open_driver(void)
{
    void *lib, **slot = (void **)(void *)&hw;
    const char *path = getenv("MCUDA_LIBCUDA");
    int i, n, r;
    lib = dlopen(path && path[0] ? path : "libcuda.so.1", RTLD_NOW | RTLD_GLOBAL);
    if (!lib)
        return 35; /* cudaErrorInsufficientDriver */
    for (i = 0; names[i]; i++) {
        void *f = dlsym(lib, names[i]);
        if (!f) {
            fprintf(stderr, "mcuda: the driver has no %s\n", names[i]);
            return 35;
        }
        slot[i] = f; /* (the members are all pointers to functions, in the order of the names) */
    }
    hw.lib = lib;
    r = hw.Init(0);
    if (r)
        return r == 100 ? 100 : r == 999 ? 999 : r;
    n = 0;
    r = hw.DeviceGetCount(&n);
    if (r || n < 1)
        return 100; /* cudaErrorNoDevice */
    r = hw.DeviceGet(&hw_dev, 0);
    if (!r)
        r = hw.PrimaryCtxRetain(&hw_ctx, hw_dev);
    return r;
}

int hw_enter(void)
{
    Tls *t = tls();
    int r;
    if (t && t->current)
        return 0;
    if (state != 1) {
        pthread_mutex_lock(&init_lock);
        if (state == 0) {
            init_err = open_driver();
            state = init_err ? 2 : 1;
        }
        pthread_mutex_unlock(&init_lock);
        if (state != 1)
            return init_err;
    }
    r = hw.CtxSetCurrent(hw_ctx);
    if (!r && t)
        t->current = 1;
    if (!r) {
        static int asked;
        const char *v = getenv("MCUDA_PRECOMPILE");
        if (v && v[0] && v[0] != '0' && !__sync_lock_test_and_set(&asked, 1))
            hw_precompile(atoi(v) > 0 ? atoi(v) : 1); /* (does not come back) */
    }
    return r;
}
