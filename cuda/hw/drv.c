/* mcudahw: the driver, opened at run time; the devices' primary contexts; the per-thread state. */
#include "hw.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct HwDriver hw;
int hw_ndev;
CUdevice hw_devs[HW_MAXDEV];
CUhandle hw_ctxs[HW_MAXDEV];

typedef struct {
    int current; /* 1 + the device whose context was made this thread's, 0: none yet */
    int dev;     /* the thread's device (cudaSetDevice) */
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

/* Why the driver could not be used, when that is a failure and not an absence: it is there and does not
   load, or loads and does not start. "" when it was opened, or is not on this machine, or finds no device. */
static char why[600];
const char *hw_why(void) { return why; }

static int open_driver(void)
{
    void *lib, **slot = (void **)(void *)&hw;
    const char *path = getenv("MCUDA_LIBCUDA"), *name, *msg;
    int i, n, r;
    size_t ml, nl;
    name = path && path[0] ? path : "libcuda.so.1";
    lib = dlopen(name, RTLD_NOW | RTLD_GLOBAL);
    if (!lib) {
        /* no such library ("...: libcuda.so.1", "libcuda.so.1: cannot open shared object file..."): absent */
        msg = dlerror();
        if (!msg)
            msg = "";
        ml = strlen(msg);
        nl = strlen(name);
        if (!((strncmp(msg, "no such shared object", 21) == 0 && ml >= nl && strcmp(msg + ml - nl, name) == 0) ||
              (strncmp(msg, name, nl) == 0 && strstr(msg, "cannot open shared object file"))))
            snprintf(why, sizeof why, "the driver (%s) is here but cannot be loaded: %s", name, msg);
        return 35; /* cudaErrorInsufficientDriver */
    }
    for (i = 0; names[i]; i++) {
        void *f = dlsym(lib, names[i]);
        if (!f) {
            snprintf(why, sizeof why, "the driver (%s) has no %s", name, names[i]);
            return 35;
        }
        slot[i] = f; /* (the members are all pointers to functions, in the order of the names) */
    }
    hw.lib = lib;
    r = hw.Init(0);
    if (r) {
        if (r != 100)
            snprintf(why, sizeof why, "the driver (%s) does not start: cuInit gives error %d", name, r);
        return r == 100 ? 100 : r == 999 ? 999 : r;
    }
    n = 0;
    r = hw.DeviceGetCount(&n);
    if (r || n < 1)
        return 100; /* cudaErrorNoDevice */
    hw_ndev = n > HW_MAXDEV ? HW_MAXDEV : n;
    for (i = 0; !r && i < hw_ndev; i++)
        r = hw.DeviceGet(&hw_devs[i], i);
    if (!r)
        r = hw.PrimaryCtxRetain(&hw_ctxs[0], hw_devs[0]);
    return r;
}

static int opened(void)
{
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
    return 0;
}

int hw_cur(void)
{
    Tls *t = tls();
    return t ? t->dev : 0;
}

CUhandle hw_ctx_of(int d)
{
    CUhandle c;
    if (opened() || d < 0 || d >= hw_ndev)
        return 0;
    pthread_mutex_lock(&init_lock);
    if (!hw_ctxs[d] && hw.PrimaryCtxRetain(&c, hw_devs[d]) == 0)
        hw_ctxs[d] = c;
    c = hw_ctxs[d];
    pthread_mutex_unlock(&init_lock);
    return c;
}

int hw_set_device(int d)
{
    Tls *t = tls();
    int r = opened();
    if (r)
        return r;
    if (d < 0 || d >= hw_ndev)
        return 101; /* cudaErrorInvalidDevice */
    if (!hw_ctx_of(d))
        return 101;
    if (t) {
        t->dev = d;
        t->current = 0; /* (hw_enter makes it current) */
    }
    return hw_enter();
}

int hw_enter(void)
{
    Tls *t = tls();
    int r, d = t ? t->dev : 0;
    if (t && t->current == d + 1)
        return 0;
    r = opened();
    if (r)
        return r;
    if (!hw_ctxs[d] && !hw_ctx_of(d))
        return 101;
    r = hw.CtxSetCurrent(hw_ctxs[d]);
    if (!r && t)
        t->current = d + 1;
    if (!r) {
        static int asked;
        const char *v = getenv("MCUDA_PRECOMPILE");
        if (v && v[0] && v[0] != '0' && !__sync_lock_test_and_set(&asked, 1))
            hw_precompile(atoi(v) > 0 ? atoi(v) : 1); /* (does not come back) */
    }
    return r;
}
