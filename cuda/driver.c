/* The cu* driver API subset on the PTX engine: devices, contexts (bookkeeping), modules loaded from PTX
   text, launches, memory, and CUDA's virtual memory management done with mmap. The driver API does not
   touch the run time's "last error". All tables are guarded by MC_LOCK_RT. */
#define MCUDA_INTERNAL
#include "mc.h"
#include <mcuda.h>
#include <cuda_runtime.h>
#include <cuda.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Both C libraries (the hosted Darwin one and the Linux one) have this raw system call; the hosted one has
   no syscall(). It returns the result or the negated errno. */
long __syscall(long number, long a, long b, long c, long d, long e, long f);

#define MAGIC_CTX 0x43545830u /* "CTX0" */
#define MAGIC_MOD 0x4d4f4455u /* "MODU" */

struct CUctx_st {
    u32 magic;
    int primary;
};

struct CUmod_st {
    u32 magic;
    McModule *m;
    char *text; /* our copy of the PTX: the engine keeps pointers into it */
};

static int inited;
static struct CUctx_st primary_ctx = {MAGIC_CTX, 1};
static CUcontext cur_ctx; /* per host thread once there are threads */

static int ctx_ok(CUcontext c) { return c && c->magic == MAGIC_CTX; }

static CUresult state(int need_ctx)
{
    int i, c;
    mcuda_lock(MC_LOCK_RT);
    i = inited;
    c = cur_ctx != 0;
    mcuda_unlock(MC_LOCK_RT);
    if (!i)
        return CUDA_ERROR_NOT_INITIALIZED;
    if (need_ctx && !c)
        return CUDA_ERROR_INVALID_CONTEXT;
    return CUDA_SUCCESS;
}

#define NEED_INIT()             \
    do {                        \
        CUresult e_ = state(0); \
        if (e_)                 \
            return e_;          \
    } while (0)
#define NEED_CTX()              \
    do {                        \
        CUresult e_ = state(1); \
        if (e_)                 \
            return e_;          \
    } while (0)

CUresult cuInit(unsigned int flags)
{
    if (flags)
        return CUDA_ERROR_INVALID_VALUE;
    if (!mcuda_dev_count())
        return CUDA_ERROR_NO_DEVICE;
    mcuda_lock(MC_LOCK_RT);
    inited = 1;
    mcuda_unlock(MC_LOCK_RT);
    return CUDA_SUCCESS;
}

CUresult cuDriverGetVersion(int *v)
{
    if (!v)
        return CUDA_ERROR_INVALID_VALUE;
    *v = CUDA_VERSION;
    return CUDA_SUCCESS;
}

/* CUresult and cudaError_t share their numbers; so do the texts. */
CUresult cuGetErrorString(CUresult e, const char **s)
{
    if (!s)
        return CUDA_ERROR_INVALID_VALUE;
    *s = cudaGetErrorString((cudaError_t)e);
    if (!strcmp(*s, "unrecognized error code")) {
        *s = 0;
        return CUDA_ERROR_INVALID_VALUE;
    }
    return CUDA_SUCCESS;
}

static const struct {
    int code;
    const char *name;
} cunames[] = {
    {0, "CUDA_SUCCESS"},
    {1, "CUDA_ERROR_INVALID_VALUE"},
    {2, "CUDA_ERROR_OUT_OF_MEMORY"},
    {3, "CUDA_ERROR_NOT_INITIALIZED"},
    {4, "CUDA_ERROR_DEINITIALIZED"},
    {100, "CUDA_ERROR_NO_DEVICE"},
    {101, "CUDA_ERROR_INVALID_DEVICE"},
    {200, "CUDA_ERROR_INVALID_IMAGE"},
    {201, "CUDA_ERROR_INVALID_CONTEXT"},
    {205, "CUDA_ERROR_MAP_FAILED"},
    {206, "CUDA_ERROR_UNMAP_FAILED"},
    {208, "CUDA_ERROR_ALREADY_MAPPED"},
    {211, "CUDA_ERROR_NOT_MAPPED"},
    {218, "CUDA_ERROR_INVALID_PTX"},
    {300, "CUDA_ERROR_INVALID_SOURCE"},
    {304, "CUDA_ERROR_OPERATING_SYSTEM"},
    {400, "CUDA_ERROR_INVALID_HANDLE"},
    {401, "CUDA_ERROR_ILLEGAL_STATE"},
    {500, "CUDA_ERROR_NOT_FOUND"},
    {600, "CUDA_ERROR_NOT_READY"},
    {700, "CUDA_ERROR_ILLEGAL_ADDRESS"},
    {701, "CUDA_ERROR_LAUNCH_OUT_OF_RESOURCES"},
    {719, "CUDA_ERROR_LAUNCH_FAILED"},
    {800, "CUDA_ERROR_NOT_PERMITTED"},
    {801, "CUDA_ERROR_NOT_SUPPORTED"},
    {999, "CUDA_ERROR_UNKNOWN"}
};

CUresult cuGetErrorName(CUresult e, const char **s)
{
    unsigned i;
    if (!s)
        return CUDA_ERROR_INVALID_VALUE;
    for (i = 0; i < sizeof cunames / sizeof cunames[0]; i++)
        if (cunames[i].code == (int)e) {
            *s = cunames[i].name;
            return CUDA_SUCCESS;
        }
    *s = 0;
    return CUDA_ERROR_INVALID_VALUE;
}

/* ---- devices ---- */

CUresult cuDeviceGetCount(int *count)
{
    if (!count)
        return CUDA_ERROR_INVALID_VALUE;
    NEED_INIT();
    *count = mcuda_dev_count();
    return CUDA_SUCCESS;
}

CUresult cuDeviceGet(CUdevice *dev, int ordinal)
{
    if (!dev)
        return CUDA_ERROR_INVALID_VALUE;
    NEED_INIT();
    if (ordinal != 0 || !mcuda_dev_count())
        return CUDA_ERROR_INVALID_DEVICE;
    *dev = 0;
    return CUDA_SUCCESS;
}

CUresult cuDeviceGetName(char *name, int len, CUdevice dev)
{
    if (!name || len <= 0)
        return CUDA_ERROR_INVALID_VALUE;
    NEED_INIT();
    if (dev != 0)
        return CUDA_ERROR_INVALID_DEVICE;
    strncpy(name, MC_DEV_NAME, (size_t)len);
    name[len - 1] = 0;
    return CUDA_SUCCESS;
}

CUresult cuDeviceGetUuid(CUuuid *uuid, CUdevice dev)
{
    struct cudaDeviceProp *p;
    if (!uuid)
        return CUDA_ERROR_INVALID_VALUE;
    NEED_INIT();
    if (dev != 0)
        return CUDA_ERROR_INVALID_DEVICE;
    p = (struct cudaDeviceProp *)malloc(sizeof *p);
    if (!p)
        return CUDA_ERROR_OUT_OF_MEMORY;
    if (cudaGetDeviceProperties(p, 0) == cudaSuccess)
        memcpy(uuid->bytes, p->uuid.bytes, 16);
    free(p);
    return CUDA_SUCCESS;
}

CUresult cuDeviceTotalMem(size_t *bytes, CUdevice dev)
{
    unsigned long fr, total;
    if (!bytes)
        return CUDA_ERROR_INVALID_VALUE;
    NEED_INIT();
    if (dev != 0)
        return CUDA_ERROR_INVALID_DEVICE;
    mcuda_mem_info(&fr, &total);
    *bytes = total;
    return CUDA_SUCCESS;
}

CUresult cuDeviceGetAttribute(int *pi, CUdevice_attribute a, CUdevice dev)
{
    if (!pi)
        return CUDA_ERROR_INVALID_VALUE;
    NEED_INIT();
    if (dev != 0)
        return CUDA_ERROR_INVALID_DEVICE;
    return mcuda_dev_attr((int)a, pi) ? CUDA_ERROR_INVALID_VALUE : CUDA_SUCCESS;
}

CUresult cuDeviceComputeCapability(int *major, int *minor, CUdevice dev)
{
    NEED_INIT();
    if (dev != 0)
        return CUDA_ERROR_INVALID_DEVICE;
    if (major)
        *major = MC_DEV_SM_MAJOR;
    if (minor)
        *minor = MC_DEV_SM_MINOR;
    return CUDA_SUCCESS;
}

/* ---- contexts: which one is current, nothing more ---- */

CUresult cuCtxCreate_v2(CUcontext *pctx, unsigned int flags, CUdevice dev)
{
    CUcontext c;
    (void)flags;
    if (!pctx)
        return CUDA_ERROR_INVALID_VALUE;
    NEED_INIT();
    if (dev != 0)
        return CUDA_ERROR_INVALID_DEVICE;
    c = (CUcontext)malloc(sizeof *c);
    if (!c)
        return CUDA_ERROR_OUT_OF_MEMORY;
    c->magic = MAGIC_CTX;
    c->primary = 0;
    mcuda_lock(MC_LOCK_RT);
    cur_ctx = c;
    mcuda_unlock(MC_LOCK_RT);
    *pctx = c;
    return CUDA_SUCCESS;
}

CUresult cuCtxCreate(CUcontext *pctx, unsigned int flags, CUdevice dev) { return cuCtxCreate_v2(pctx, flags, dev); }

CUresult cuCtxDestroy(CUcontext c)
{
    NEED_INIT();
    if (!ctx_ok(c) || c->primary)
        return CUDA_ERROR_INVALID_VALUE;
    mcuda_lock(MC_LOCK_RT);
    if (cur_ctx == c)
        cur_ctx = 0;
    c->magic = 0;
    mcuda_unlock(MC_LOCK_RT);
    free(c);
    return CUDA_SUCCESS;
}

CUresult cuCtxGetCurrent(CUcontext *pctx)
{
    if (!pctx)
        return CUDA_ERROR_INVALID_VALUE;
    NEED_INIT();
    mcuda_lock(MC_LOCK_RT);
    *pctx = cur_ctx;
    mcuda_unlock(MC_LOCK_RT);
    return CUDA_SUCCESS;
}

CUresult cuCtxSetCurrent(CUcontext c)
{
    NEED_INIT();
    if (c && !ctx_ok(c))
        return CUDA_ERROR_INVALID_CONTEXT;
    mcuda_lock(MC_LOCK_RT);
    cur_ctx = c;
    mcuda_unlock(MC_LOCK_RT);
    return CUDA_SUCCESS;
}

/* There is no stack of contexts: push sets the current one, pop takes it away. */
CUresult cuCtxPushCurrent(CUcontext c)
{
    if (!c)
        return CUDA_ERROR_INVALID_CONTEXT;
    return cuCtxSetCurrent(c);
}

CUresult cuCtxPopCurrent(CUcontext *pctx)
{
    NEED_CTX();
    mcuda_lock(MC_LOCK_RT);
    if (pctx)
        *pctx = cur_ctx;
    cur_ctx = 0;
    mcuda_unlock(MC_LOCK_RT);
    return CUDA_SUCCESS;
}

CUresult cuCtxGetDevice(CUdevice *dev)
{
    if (!dev)
        return CUDA_ERROR_INVALID_VALUE;
    NEED_CTX();
    *dev = 0;
    return CUDA_SUCCESS;
}

CUresult cuCtxSynchronize(void)
{
    NEED_CTX();
    return CUDA_SUCCESS;
}

CUresult cuDevicePrimaryCtxRetain(CUcontext *pctx, CUdevice dev)
{
    if (!pctx)
        return CUDA_ERROR_INVALID_VALUE;
    NEED_INIT();
    if (dev != 0)
        return CUDA_ERROR_INVALID_DEVICE;
    *pctx = &primary_ctx;
    return CUDA_SUCCESS;
}

CUresult cuDevicePrimaryCtxRelease(CUdevice dev)
{
    NEED_INIT();
    return dev != 0 ? CUDA_ERROR_INVALID_DEVICE : CUDA_SUCCESS;
}

/* ---- modules ---- */

static int mod_ok(CUmodule m) { return m && m->magic == MAGIC_MOD; }

static void put_log(unsigned n, CUjit_option *opt, void **val, CUjit_option which_buf, CUjit_option which_size, const char *msg)
{
    char *buf = 0;
    u64 size = 0;
    unsigned i;
    for (i = 0; i < n; i++) {
        if (opt[i] == which_buf)
            buf = (char *)val[i];
        if (opt[i] == which_size)
            size = (u64)val[i]; /* the size travels in the pointer itself */
    }
    if (buf && size) {
        strncpy(buf, msg, size);
        buf[size - 1] = 0;
    }
}

CUresult cuModuleLoadDataEx(CUmodule *pmod, const void *image, unsigned int n, CUjit_option *opt, void **val)
{
    const u8 *b = (const u8 *)image;
    CUmodule mod;
    u64 len;
    if (!pmod || !image || (n && (!opt || !val)))
        return CUDA_ERROR_INVALID_VALUE;
    *pmod = 0;
    NEED_CTX();
    put_log(n, opt, val, CU_JIT_INFO_LOG_BUFFER, CU_JIT_INFO_LOG_BUFFER_SIZE_BYTES, "");
    put_log(n, opt, val, CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES, "");
    /* an ELF cubin or a fatbin: only PTX text runs here */
    if ((b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F') || (b[0] == 0x50 && b[1] == 0xed && b[2] == 0x55 && b[3] == 0xba)) {
        put_log(n, opt, val, CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES, "mcuda: the image is not PTX text");
        return CUDA_ERROR_INVALID_SOURCE;
    }
    mod = (CUmodule)malloc(sizeof *mod);
    len = strlen((const char *)image);
    if (!mod)
        return CUDA_ERROR_OUT_OF_MEMORY;
    mod->text = (char *)malloc(len + 1);
    if (!mod->text) {
        free(mod);
        return CUDA_ERROR_OUT_OF_MEMORY;
    }
    memcpy(mod->text, image, len + 1);
    mcuda_lock(MC_LOCK_MODULE);
    mod->m = mc_module_load(mod->text, "cuModuleLoadData");
    if (!mod->m)
        put_log(n, opt, val, CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES, mc_error());
    mcuda_unlock(MC_LOCK_MODULE);
    if (!mod->m) {
        free(mod->text);
        free(mod);
        return CUDA_ERROR_INVALID_PTX;
    }
    mod->magic = MAGIC_MOD;
    *pmod = mod;
    return CUDA_SUCCESS;
}

CUresult cuModuleLoadData(CUmodule *pmod, const void *image) { return cuModuleLoadDataEx(pmod, image, 0, 0, 0); }

CUresult cuModuleUnload(CUmodule mod)
{
    NEED_CTX();
    if (!mod_ok(mod))
        return CUDA_ERROR_INVALID_HANDLE;
    mod->magic = 0;
    mcuda_lock(MC_LOCK_MODULE);
    mc_module_free(mod->m);
    mcuda_unlock(MC_LOCK_MODULE);
    free(mod->text);
    free(mod);
    return CUDA_SUCCESS;
}

/* A CUfunction is the engine's McFunc. */
CUresult cuModuleGetFunction(CUfunction *pf, CUmodule mod, const char *name)
{
    McFunc *f;
    if (!pf || !name)
        return CUDA_ERROR_INVALID_VALUE;
    *pf = 0;
    NEED_CTX();
    if (!mod_ok(mod))
        return CUDA_ERROR_INVALID_HANDLE;
    f = mc_module_func(mod->m, name);
    if (!f)
        return CUDA_ERROR_NOT_FOUND;
    *pf = (CUfunction)f;
    return CUDA_SUCCESS;
}

CUresult cuModuleGetGlobal(CUdeviceptr *dptr, size_t *bytes, CUmodule mod, const char *name)
{
    void *p;
    u64 size = 0;
    if (!name || (!dptr && !bytes))
        return CUDA_ERROR_INVALID_VALUE;
    NEED_CTX();
    if (!mod_ok(mod))
        return CUDA_ERROR_INVALID_HANDLE;
    p = mc_module_global(mod->m, name, &size);
    if (!p)
        return CUDA_ERROR_NOT_FOUND;
    if (dptr)
        *dptr = (CUdeviceptr)(u64)p;
    if (bytes)
        *bytes = size;
    return CUDA_SUCCESS;
}

CUresult cuLaunchKernel(CUfunction cf, unsigned int gx, unsigned int gy, unsigned int gz, unsigned int bx, unsigned int by,
                        unsigned int bz, unsigned int shared, CUstream stream, void **params, void **extra)
{
    McFunc *f = (McFunc *)cf;
    unsigned grid[3], block[3];
    const void *buf = 0;
    u64 bufsize = 0;
    int have_size = 0, e;
    NEED_CTX();
    if (!f)
        return CUDA_ERROR_INVALID_HANDLE;
    if (!mcuda_stream_ok(stream))
        return CUDA_ERROR_INVALID_HANDLE;
    grid[0] = gx;
    grid[1] = gy;
    grid[2] = gz;
    block[0] = bx;
    block[1] = by;
    block[2] = bz;
    if (mcuda_launch_check(f, grid, block, shared))
        return CUDA_ERROR_INVALID_VALUE;
    if (params)
        return (CUresult)mc_launch(f, grid, block, shared, params);
    if (extra) {
        for (; *extra != CU_LAUNCH_PARAM_END; extra += 2) {
            if (*extra == CU_LAUNCH_PARAM_BUFFER_POINTER)
                buf = extra[1];
            else if (*extra == CU_LAUNCH_PARAM_BUFFER_SIZE) {
                if (!extra[1])
                    return CUDA_ERROR_INVALID_VALUE;
                bufsize = *(const size_t *)extra[1];
                have_size = 1;
            } else
                return CUDA_ERROR_INVALID_VALUE;
        }
    }
    if (mc_func_nparams(f) == 0)
        return (CUresult)mc_launch(f, grid, block, shared, 0);
    if (!buf || !have_size || bufsize < mc_func_param_bytes(f))
        return CUDA_ERROR_INVALID_VALUE;
    e = mc_launch_buf(f, grid, block, shared, buf);
    return (CUresult)e;
}

/* ---- memory ---- */

CUresult cuMemAlloc(CUdeviceptr *dptr, size_t n)
{
    void *p = 0;
    int e;
    if (!dptr || !n)
        return CUDA_ERROR_INVALID_VALUE;
    *dptr = 0;
    NEED_CTX();
    e = mcuda_mem_alloc(&p, n, MCUDA_MEM_DEVICE);
    *dptr = (CUdeviceptr)(u64)p;
    return (CUresult)e;
}

CUresult cuMemFree(CUdeviceptr dptr)
{
    NEED_CTX();
    if (!dptr || mcuda_mem_free((void *)(u64)dptr, 1 << MCUDA_MEM_DEVICE | 1 << MCUDA_MEM_MANAGED))
        return CUDA_ERROR_INVALID_VALUE;
    return CUDA_SUCCESS;
}

CUresult cuMemGetInfo(size_t *fr, size_t *total)
{
    unsigned long f, t;
    NEED_CTX();
    mcuda_mem_info(&f, &t);
    if (fr)
        *fr = f;
    if (total)
        *total = t;
    return CUDA_SUCCESS;
}

static CUresult dcopy(void *dst, const void *src, size_t n)
{
    NEED_CTX();
    if (!n)
        return CUDA_SUCCESS;
    if (!dst || !src)
        return CUDA_ERROR_INVALID_VALUE;
    memmove(dst, src, n);
    return CUDA_SUCCESS;
}

CUresult cuMemcpyHtoD(CUdeviceptr dst, const void *src, size_t n) { return dcopy((void *)(u64)dst, src, n); }
CUresult cuMemcpyDtoH(void *dst, CUdeviceptr src, size_t n) { return dcopy(dst, (const void *)(u64)src, n); }
CUresult cuMemcpyDtoD(CUdeviceptr dst, CUdeviceptr src, size_t n) { return dcopy((void *)(u64)dst, (const void *)(u64)src, n); }

CUresult cuMemsetD8(CUdeviceptr dst, unsigned char v, size_t n)
{
    NEED_CTX();
    if (n && !dst)
        return CUDA_ERROR_INVALID_VALUE;
    if (n)
        memset((void *)(u64)dst, v, n);
    return CUDA_SUCCESS;
}

CUresult cuMemsetD32(CUdeviceptr dst, unsigned int v, size_t n)
{
    u32 *p = (u32 *)(u64)dst;
    size_t i;
    NEED_CTX();
    if (n && (!dst || (dst & 3)))
        return CUDA_ERROR_INVALID_VALUE;
    for (i = 0; i < n; i++)
        p[i] = v;
    return CUDA_SUCCESS;
}

/* ---- virtual memory management ----
   A reservation is an inaccessible anonymous mapping. A physical allocation is an unlinked shared-memory
   object (a file descriptor); mapping it puts a shared mapping of that object over a piece of a
   reservation, so the same allocation mapped twice shows the same bytes, as on the GPU. Unmapping puts
   the inaccessible anonymous pages back. */

#define GRAN ((u64)2 << 20) /* the granularity NVIDIA's driver reports */

typedef struct {
    int used;     /* the slot is live: the handle is its index + 1 */
    int released; /* cuMemRelease was called; the slot lives on until the last mapping goes */
    int fd;       /* -1 once released */
    int nmaps;
    u64 size;
} Phys;

typedef struct Resv {
    u8 *base;
    u64 size;
    struct Resv *next;
} Resv;

typedef struct Map {
    u8 *addr;
    u64 size;
    int phys; /* index */
    int prot; /* CU_MEM_ACCESS_FLAGS_* */
    struct Map *next;
} Map;

static Phys *phys;
static int nphys;
static Resv *resvs;
static Map *maps;

/* A file descriptor for `size` bytes of shareable memory that has no name any more, or -1. */
static int backing_fd(u64 size)
{
    static unsigned serial;
    char name[64];
    int fd;
#ifdef __linux__
    (void)name;
    (void)serial;
    fd = (int)__syscall(279, (long)"mcuda", 1, 0, 0, 0, 0); /* memfd_create(name, MFD_CLOEXEC) */
#else
    sprintf(name, "/mcuda.%d.%u", (int)getpid(), serial++);
    fd = (int)__syscall(266, (long)name, O_RDWR | O_CREAT | O_EXCL, 0600, 0, 0, 0); /* shm_open */
    if (fd >= 0)
        __syscall(267, (long)name, 0, 0, 0, 0, 0); /* shm_unlink */
#endif
    if (fd < 0) { /* neither: an unlinked temporary file */
        const char *dir = getenv("TMPDIR");
        char *path;
#ifdef __linux__
        if (!dir && access("/dev/shm", W_OK) == 0)
            dir = "/dev/shm";
#endif
        if (!dir || !*dir)
            dir = "/tmp";
        path = (char *)malloc(strlen(dir) + 32);
        if (!path)
            return -1;
        sprintf(path, "%s/mcuda.XXXXXX", dir);
        fd = mkstemp(path);
        if (fd >= 0)
            unlink(path);
        free(path);
    }
    if (fd >= 0 && ftruncate(fd, (off_t)size)) {
        close(fd);
        fd = -1;
    }
    return fd;
}

static Resv *resv_of(const u8 *p, u64 size)
{
    Resv *r;
    for (r = resvs; r; r = r->next)
        if (p >= r->base && size <= r->size && (u64)(p - r->base) <= r->size - size)
            return r;
    return 0;
}

/* Bytes of [p, p + size) that are mapped; *whole is cleared if a mapping sticks out of the range. */
static u64 covered(const u8 *p, u64 size, int *whole)
{
    Map *m;
    u64 n = 0;
    for (m = maps; m; m = m->next) {
        const u8 *lo = m->addr > p ? m->addr : p;
        const u8 *hi = m->addr + m->size < p + size ? m->addr + m->size : p + size;
        if (lo < hi) {
            n += (u64)(hi - lo);
            if (whole && (u64)(hi - lo) != m->size)
                *whole = 0;
        }
    }
    return n;
}

/* Call with the lock held. Returns the bytes to give back with mcuda_mem_charge once the lock is released
   (mcuda_mem_charge takes it itself). */
static u64 phys_drop(int i)
{
    if (phys[i].fd >= 0)
        close(phys[i].fd);
    phys[i].fd = -1;
    phys[i].used = 0;
    return phys[i].size;
}

/* Take every mapping inside [p, p + size) away; with `restore` the reservation's pages come back.
   Call with the lock held. Returns the bytes of physical memory that this freed. */
static u64 unmap_range(u8 *p, u64 size, int restore)
{
    Map **pm = &maps, *m;
    u64 freed = 0;
    while ((m = *pm) != 0) {
        if (m->addr >= p && m->addr + m->size <= p + size) {
            *pm = m->next;
            if (restore)
                mmap(m->addr, m->size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
            if (--phys[m->phys].nmaps == 0 && phys[m->phys].released)
                freed += phys_drop(m->phys);
            free(m);
        } else
            pm = &m->next;
    }
    return freed;
}

CUresult cuMemGetAllocationGranularity(size_t *gran, const CUmemAllocationProp *prop, CUmemAllocationGranularity_flags option)
{
    if (!gran || !prop || (option != CU_MEM_ALLOC_GRANULARITY_MINIMUM && option != CU_MEM_ALLOC_GRANULARITY_RECOMMENDED))
        return CUDA_ERROR_INVALID_VALUE;
    NEED_CTX();
    if (prop->type != CU_MEM_ALLOCATION_TYPE_PINNED || prop->location.type != CU_MEM_LOCATION_TYPE_DEVICE)
        return CUDA_ERROR_NOT_SUPPORTED;
    if (prop->location.id != 0)
        return CUDA_ERROR_INVALID_DEVICE;
    *gran = GRAN;
    return CUDA_SUCCESS;
}

CUresult cuMemAddressReserve(CUdeviceptr *ptr, size_t size, size_t alignment, CUdeviceptr addr, unsigned long long flags)
{
    u64 page = (u64)sysconf(_SC_PAGESIZE), extra, head;
    u8 *raw, *base;
    Resv *r;
    if (!ptr || !size || size % GRAN || flags || (alignment & (alignment - 1)))
        return CUDA_ERROR_INVALID_VALUE;
    *ptr = 0;
    NEED_CTX();
    if ((long)page <= 0)
        page = 16384;
    if (!alignment)
        alignment = GRAN;
    extra = alignment > page ? alignment : 0;
    if (size + extra < size)
        return CUDA_ERROR_INVALID_VALUE;
    r = (Resv *)malloc(sizeof *r);
    if (!r)
        return CUDA_ERROR_OUT_OF_MEMORY;
    /* addr is a hint, as in the driver */
    raw = (u8 *)mmap((void *)(u64)addr, size + extra, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (raw == (u8 *)MAP_FAILED) {
        free(r);
        return CUDA_ERROR_OUT_OF_MEMORY;
    }
    base = raw;
    if (extra) {
        base = (u8 *)(((u64)raw + alignment - 1) & ~((u64)alignment - 1));
        head = (u64)(base - raw);
        if (head)
            munmap(raw, head);
        if (extra - head)
            munmap(base + size, extra - head);
    }
    if (mcuda_mem_track(base, size, MCUDA_MEM_DEVICE)) {
        munmap(base, size);
        free(r);
        return CUDA_ERROR_OUT_OF_MEMORY;
    }
    r->base = base;
    r->size = size;
    mcuda_lock(MC_LOCK_RT);
    r->next = resvs;
    resvs = r;
    mcuda_unlock(MC_LOCK_RT);
    *ptr = (CUdeviceptr)(u64)base;
    return CUDA_SUCCESS;
}

CUresult cuMemAddressFree(CUdeviceptr ptr, size_t size)
{
    u8 *p = (u8 *)(u64)ptr;
    Resv **pr, *r;
    u64 freed = 0;
    NEED_CTX();
    mcuda_lock(MC_LOCK_RT);
    for (pr = &resvs; (r = *pr) != 0; pr = &r->next)
        if (r->base == p && r->size == size)
            break;
    if (r) {
        *pr = r->next;
        freed = unmap_range(p, size, 0);
    }
    mcuda_unlock(MC_LOCK_RT);
    if (!r)
        return CUDA_ERROR_INVALID_VALUE;
    mcuda_mem_charge(-(long)freed);
    mcuda_mem_untrack(p);
    munmap(p, size);
    free(r);
    return CUDA_SUCCESS;
}

CUresult cuMemCreate(CUmemGenericAllocationHandle *handle, size_t size, const CUmemAllocationProp *prop, unsigned long long flags)
{
    int i, fd;
    if (!handle || !prop || !size || size % GRAN || flags)
        return CUDA_ERROR_INVALID_VALUE;
    *handle = 0;
    NEED_CTX();
    if (prop->type != CU_MEM_ALLOCATION_TYPE_PINNED || prop->location.type != CU_MEM_LOCATION_TYPE_DEVICE)
        return CUDA_ERROR_NOT_SUPPORTED;
    if (prop->location.id != 0)
        return CUDA_ERROR_INVALID_DEVICE;
    if (mcuda_mem_charge((long)size))
        return CUDA_ERROR_OUT_OF_MEMORY;
    fd = backing_fd(size);
    if (fd < 0) {
        mcuda_mem_charge(-(long)size);
        return CUDA_ERROR_OUT_OF_MEMORY;
    }
    mcuda_lock(MC_LOCK_RT);
    for (i = 0; i < nphys && phys[i].used; i++)
        ;
    if (i == nphys) {
        int cap = nphys ? nphys * 2 : 64, k;
        Phys *t = (Phys *)realloc(phys, (size_t)cap * sizeof *t);
        if (!t) {
            mcuda_unlock(MC_LOCK_RT);
            close(fd);
            mcuda_mem_charge(-(long)size);
            return CUDA_ERROR_OUT_OF_MEMORY;
        }
        for (k = nphys; k < cap; k++) {
            t[k].used = 0;
            t[k].fd = -1;
        }
        phys = t;
        nphys = cap;
    }
    phys[i].used = 1;
    phys[i].released = 0;
    phys[i].fd = fd;
    phys[i].nmaps = 0;
    phys[i].size = size;
    mcuda_unlock(MC_LOCK_RT);
    *handle = (CUmemGenericAllocationHandle)(i + 1);
    return CUDA_SUCCESS;
}

/* Call with the lock held: the slot of a handle that may still be mapped, or -1. */
static int phys_of(CUmemGenericAllocationHandle h)
{
    if (!h || h > (CUmemGenericAllocationHandle)nphys || !phys[h - 1].used || phys[h - 1].released)
        return -1;
    return (int)(h - 1);
}

CUresult cuMemRelease(CUmemGenericAllocationHandle handle)
{
    int i;
    u64 freed = 0;
    NEED_CTX();
    mcuda_lock(MC_LOCK_RT);
    i = phys_of(handle);
    if (i >= 0) {
        phys[i].released = 1;
        close(phys[i].fd); /* live mappings keep the memory */
        phys[i].fd = -1;
        if (!phys[i].nmaps)
            freed = phys_drop(i);
    }
    mcuda_unlock(MC_LOCK_RT);
    mcuda_mem_charge(-(long)freed);
    return i < 0 ? CUDA_ERROR_INVALID_VALUE : CUDA_SUCCESS;
}

CUresult cuMemMap(CUdeviceptr ptr, size_t size, size_t offset, CUmemGenericAllocationHandle handle, unsigned long long flags)
{
    u8 *p = (u8 *)(u64)ptr;
    Map *m;
    int i;
    CUresult e = CUDA_SUCCESS;
    if (!ptr || !size || offset || flags || (u64)ptr % GRAN)
        return CUDA_ERROR_INVALID_VALUE;
    NEED_CTX();
    m = (Map *)malloc(sizeof *m);
    if (!m)
        return CUDA_ERROR_OUT_OF_MEMORY;
    mcuda_lock(MC_LOCK_RT);
    i = phys_of(handle);
    if (i < 0 || phys[i].size != size || !resv_of(p, size) || covered(p, size, 0))
        e = CUDA_ERROR_INVALID_VALUE;
    else if (mmap(p, size, PROT_NONE, MAP_SHARED | MAP_FIXED, phys[i].fd, 0) != (void *)p) {
        /* the reservation's pages may be gone from a part of the range: put them back */
        mmap(p, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0);
        e = CUDA_ERROR_OUT_OF_MEMORY;
    } else {
        m->addr = p;
        m->size = size;
        m->phys = i;
        m->prot = CU_MEM_ACCESS_FLAGS_PROT_NONE;
        m->next = maps;
        maps = m;
        phys[i].nmaps++;
    }
    mcuda_unlock(MC_LOCK_RT);
    if (e)
        free(m);
    return e;
}

CUresult cuMemUnmap(CUdeviceptr ptr, size_t size)
{
    u8 *p = (u8 *)(u64)ptr;
    int whole = 1;
    u64 freed = 0;
    CUresult e = CUDA_SUCCESS;
    if (!ptr || !size)
        return CUDA_ERROR_INVALID_VALUE;
    NEED_CTX();
    mcuda_lock(MC_LOCK_RT);
    if (!resv_of(p, size) || covered(p, size, &whole) != size || !whole)
        e = CUDA_ERROR_INVALID_VALUE;
    else
        freed = unmap_range(p, size, 1);
    mcuda_unlock(MC_LOCK_RT);
    mcuda_mem_charge(-(long)freed);
    return e;
}

CUresult cuMemSetAccess(CUdeviceptr ptr, size_t size, const CUmemAccessDesc *desc, size_t count)
{
    u8 *p = (u8 *)(u64)ptr;
    Map *m;
    int prot;
    CUresult e = CUDA_SUCCESS;
    if (!ptr || !size || !desc || count != 1)
        return CUDA_ERROR_INVALID_VALUE;
    NEED_CTX();
    if (desc->location.type != CU_MEM_LOCATION_TYPE_DEVICE)
        return CUDA_ERROR_NOT_SUPPORTED;
    if (desc->location.id != 0)
        return CUDA_ERROR_INVALID_DEVICE;
    if (desc->flags == CU_MEM_ACCESS_FLAGS_PROT_NONE)
        prot = PROT_NONE;
    else if (desc->flags == CU_MEM_ACCESS_FLAGS_PROT_READ)
        prot = PROT_READ;
    else if (desc->flags == CU_MEM_ACCESS_FLAGS_PROT_READWRITE)
        prot = PROT_READ | PROT_WRITE;
    else
        return CUDA_ERROR_INVALID_VALUE;
    mcuda_lock(MC_LOCK_RT);
    if (!resv_of(p, size) || covered(p, size, 0) != size) /* every byte must be mapped */
        e = CUDA_ERROR_INVALID_VALUE;
    else if (mprotect(p, size, prot))
        e = CUDA_ERROR_UNKNOWN;
    else
        for (m = maps; m; m = m->next)
            if (m->addr >= p && m->addr + m->size <= p + size)
                m->prot = (int)desc->flags;
    mcuda_unlock(MC_LOCK_RT);
    return e;
}

CUresult cuMemGetAccess(unsigned long long *flags, const CUmemLocation *loc, CUdeviceptr ptr)
{
    const u8 *p = (const u8 *)(u64)ptr;
    Map *m;
    CUresult e = CUDA_ERROR_INVALID_VALUE;
    if (!flags || !loc)
        return CUDA_ERROR_INVALID_VALUE;
    NEED_CTX();
    mcuda_lock(MC_LOCK_RT);
    for (m = maps; m; m = m->next)
        if (p >= m->addr && p < m->addr + m->size) {
            *flags = (unsigned long long)m->prot;
            e = CUDA_SUCCESS;
        }
    mcuda_unlock(MC_LOCK_RT);
    return e;
}

/* ---- streams: the run time's ---- */

CUresult cuStreamCreate(CUstream *ps, unsigned int flags)
{
    cudaError_t e;
    NEED_CTX();
    e = cudaStreamCreateWithFlags(ps, flags);
    if (e)
        cudaGetLastError(); /* the driver API leaves no run-time error behind */
    return (CUresult)e;
}

CUresult cuStreamDestroy(CUstream s)
{
    NEED_CTX();
    if ((u64)s <= 2 || !mcuda_stream_ok(s))
        return CUDA_ERROR_INVALID_HANDLE;
    cudaStreamDestroy(s);
    return CUDA_SUCCESS;
}

CUresult cuStreamSynchronize(CUstream s)
{
    NEED_CTX();
    return mcuda_stream_ok(s) ? CUDA_SUCCESS : CUDA_ERROR_INVALID_HANDLE;
}

CUresult cuStreamQuery(CUstream s)
{
    NEED_CTX();
    return mcuda_stream_ok(s) ? CUDA_SUCCESS : CUDA_ERROR_INVALID_HANDLE;
}

/* ---- cudaGetDriverEntryPointByVersion ---- */

typedef void (*Fn)(void);

static const struct {
    const char *name;
    Fn fn;
} entries[] = {
    {"cuInit", (Fn)cuInit},
    {"cuDriverGetVersion", (Fn)cuDriverGetVersion},
    {"cuGetErrorString", (Fn)cuGetErrorString},
    {"cuGetErrorName", (Fn)cuGetErrorName},
    {"cuDeviceGet", (Fn)cuDeviceGet},
    {"cuDeviceGetCount", (Fn)cuDeviceGetCount},
    {"cuDeviceGetName", (Fn)cuDeviceGetName},
    {"cuDeviceGetUuid", (Fn)cuDeviceGetUuid},
    {"cuDeviceTotalMem", (Fn)cuDeviceTotalMem},
    {"cuDeviceGetAttribute", (Fn)cuDeviceGetAttribute},
    {"cuDeviceComputeCapability", (Fn)cuDeviceComputeCapability},
    {"cuCtxCreate", (Fn)cuCtxCreate},
    {"cuCtxDestroy", (Fn)cuCtxDestroy},
    {"cuCtxGetCurrent", (Fn)cuCtxGetCurrent},
    {"cuCtxSetCurrent", (Fn)cuCtxSetCurrent},
    {"cuCtxPushCurrent", (Fn)cuCtxPushCurrent},
    {"cuCtxPopCurrent", (Fn)cuCtxPopCurrent},
    {"cuCtxGetDevice", (Fn)cuCtxGetDevice},
    {"cuCtxSynchronize", (Fn)cuCtxSynchronize},
    {"cuDevicePrimaryCtxRetain", (Fn)cuDevicePrimaryCtxRetain},
    {"cuDevicePrimaryCtxRelease", (Fn)cuDevicePrimaryCtxRelease},
    {"cuModuleLoadData", (Fn)cuModuleLoadData},
    {"cuModuleLoadDataEx", (Fn)cuModuleLoadDataEx},
    {"cuModuleUnload", (Fn)cuModuleUnload},
    {"cuModuleGetFunction", (Fn)cuModuleGetFunction},
    {"cuModuleGetGlobal", (Fn)cuModuleGetGlobal},
    {"cuLaunchKernel", (Fn)cuLaunchKernel},
    {"cuMemAlloc", (Fn)cuMemAlloc},
    {"cuMemFree", (Fn)cuMemFree},
    {"cuMemGetInfo", (Fn)cuMemGetInfo},
    {"cuMemcpyHtoD", (Fn)cuMemcpyHtoD},
    {"cuMemcpyDtoH", (Fn)cuMemcpyDtoH},
    {"cuMemcpyDtoD", (Fn)cuMemcpyDtoD},
    {"cuMemsetD8", (Fn)cuMemsetD8},
    {"cuMemsetD32", (Fn)cuMemsetD32},
    {"cuMemGetAllocationGranularity", (Fn)cuMemGetAllocationGranularity},
    {"cuMemAddressReserve", (Fn)cuMemAddressReserve},
    {"cuMemAddressFree", (Fn)cuMemAddressFree},
    {"cuMemCreate", (Fn)cuMemCreate},
    {"cuMemRelease", (Fn)cuMemRelease},
    {"cuMemMap", (Fn)cuMemMap},
    {"cuMemUnmap", (Fn)cuMemUnmap},
    {"cuMemSetAccess", (Fn)cuMemSetAccess},
    {"cuMemGetAccess", (Fn)cuMemGetAccess},
    {"cuStreamCreate", (Fn)cuStreamCreate},
    {"cuStreamDestroy", (Fn)cuStreamDestroy},
    {"cuStreamSynchronize", (Fn)cuStreamSynchronize},
    {"cuStreamQuery", (Fn)cuStreamQuery}
};

/* The run time hands out driver functions after it has initialised the driver and made the device's
   primary context current, as NVIDIA's does; so the functions work without cuInit and cuCtxCreate. */
void *mcuda_driver_entry(const char *name)
{
    unsigned i;
    u64 len = strlen(name);
    /* NVIDIA's names carry version suffixes (cuMemAlloc_v2): accept them */
    if (len > 3 && name[len - 3] == '_' && name[len - 2] == 'v' && name[len - 1] >= '2' && name[len - 1] <= '4')
        len -= 3;
    for (i = 0; i < sizeof entries / sizeof entries[0]; i++)
        if (strlen(entries[i].name) == len && !strncmp(entries[i].name, name, len)) {
            mcuda_lock(MC_LOCK_RT);
            inited = 1;
            if (!cur_ctx)
                cur_ctx = &primary_ctx;
            mcuda_unlock(MC_LOCK_RT);
            return (void *)entries[i].fn;
        }
    return 0;
}
