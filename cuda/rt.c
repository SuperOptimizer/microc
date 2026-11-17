/* The cuda* run-time API on the PTX engine: one device that is the host CPU. Device memory is host memory,
   every call is synchronous, streams and events are bookkeeping. Also the two entry points that compiled
   CUDA code calls (__mcuda_launch, __mcuda_symbol). All tables are guarded by MC_LOCK_RT. */
#define MCUDA_INTERNAL
#include "mc.h"
#include <mcuda.h>
#include <mcuda_pack.h>
#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/sysinfo.h>
#else
#include <sys/sysctl.h>
#endif

#define DEV_TOTAL ((u64)24 << 30) /* an RTX 3090 has 24 GiB */
#define ALIGN_DEVICE 256
#define ALIGN_HOST 4096

/* ---- the last error: per host thread once there are threads ---- */

static int last_error;

static void err_record(int e)
{
    mcuda_lock(MC_LOCK_ERR);
    last_error = e;
    mcuda_unlock(MC_LOCK_ERR);
}

static int err_read(int clear)
{
    int e;
    mcuda_lock(MC_LOCK_ERR);
    e = last_error;
    if (clear)
        last_error = 0;
    mcuda_unlock(MC_LOCK_ERR);
    return e;
}

/* Every API function returns through here. */
static cudaError_t ret(int e)
{
    if (e)
        err_record(e);
    return (cudaError_t)e;
}

cudaError_t cudaGetLastError(void) { return (cudaError_t)err_read(1); }
cudaError_t cudaPeekAtLastError(void) { return (cudaError_t)err_read(0); }

#include "errtab.h"

/* ---- the device ---- */

/* 1 unless CUDA_VISIBLE_DEVICES is set and does not list device 0. */
static int dev_count(void)
{
    static int count = -1;
    int n;
    mcuda_lock(MC_LOCK_RT);
    if (count < 0) {
        const char *e = getenv("CUDA_VISIBLE_DEVICES");
        count = 1;
        if (e) {
            count = 0;
            while (*e) {
                const char *s = e;
                while (*e && *e != ',')
                    e++;
                if (e - s == 1 && *s == '0')
                    count = 1;
                if (*e)
                    e++;
            }
        }
    }
    n = count;
    mcuda_unlock(MC_LOCK_RT);
    return n;
}

int mcuda_dev_count(void) { return dev_count(); }

#define NEED_DEVICE()                      \
    do {                                   \
        if (!dev_count())                  \
            return ret(cudaErrorNoDevice); \
    } while (0)

static u64 host_ram(void)
{
    static u64 ram;
    if (!ram) {
#ifdef __linux__
        struct sysinfo si;
        if (sysinfo(&si) == 0)
            ram = (u64)si.totalram * (si.mem_unit ? si.mem_unit : 1);
#else
        int mib[2];
        u64 v = 0;
        size_t len = sizeof v;
        mib[0] = CTL_HW;
        mib[1] = 24; /* HW_MEMSIZE */
        if (sysctl(mib, 2, &v, &len, 0, 0) == 0 && len == sizeof v)
            ram = v;
#endif
        if (!ram)
            ram = (u64)8 << 30; /* unknown: a modest machine */
    }
    return ram;
}

/* ---- the allocation table: every live allocation, sorted by address ---- */

typedef struct {
    u8 *base;
    u64 size;
    void *raw;  /* what free() takes; 0 for a range somebody else owns */
    int type;   /* MCUDA_MEM_* */
    u32 flags;  /* the cudaHostAlloc flags */
} Alloc;

static Alloc *tab;
static u64 ntab, captab;
static u64 dev_used;  /* bytes of device and managed memory, with the physical memory of cuMemCreate */
static u64 host_used; /* bytes of cudaHostAlloc memory */

/* Call with the lock held. */
static u64 dev_total(void)
{
    u64 ram = host_ram();
    return ram < DEV_TOTAL ? ram : DEV_TOTAL;
}

static u64 dev_total_now(void)
{
    u64 t;
    mcuda_lock(MC_LOCK_RT);
    t = dev_total();
    mcuda_unlock(MC_LOCK_RT);
    return t;
}

/* The index of the last entry that starts at or before p, or -1. Call with the lock held. */
static long tab_floor(const u8 *p)
{
    long lo = 0, hi = (long)ntab - 1, r = -1;
    while (lo <= hi) {
        long mid = (lo + hi) / 2;
        if (tab[mid].base <= p) {
            r = mid;
            lo = mid + 1;
        } else
            hi = mid - 1;
    }
    return r;
}

static int tab_insert(u8 *base, u64 size, void *raw, int type, u32 flags)
{
    long i;
    if (ntab == captab) {
        u64 cap = captab ? captab * 2 : 256;
        Alloc *t = (Alloc *)realloc(tab, cap * sizeof *t);
        if (!t)
            return -1;
        tab = t;
        captab = cap;
    }
    i = tab_floor(base) + 1;
    memmove(tab + i + 1, tab + i, (ntab - (u64)i) * sizeof *tab);
    tab[i].base = base;
    tab[i].size = size;
    tab[i].raw = raw;
    tab[i].type = type;
    tab[i].flags = flags;
    ntab++;
    return 0;
}

static void tab_remove(long i)
{
    memmove(tab + i, tab + i + 1, (ntab - (u64)i - 1) * sizeof *tab);
    ntab--;
}

static int mem_alloc(void **out, u64 n, int type, u32 flags)
{
    u64 align = type == MCUDA_MEM_DEVICE ? ALIGN_DEVICE : ALIGN_HOST;
    void *raw;
    u8 *base;
    int full;
    *out = 0;
    if (n > ((u64)1 << 46))
        return cudaErrorMemoryAllocation;
    mcuda_lock(MC_LOCK_RT);
    full = type != MCUDA_MEM_HOST && (dev_used > dev_total() || n > dev_total() - dev_used);
    mcuda_unlock(MC_LOCK_RT);
    if (full)
        return cudaErrorMemoryAllocation;
    raw = malloc(n + align);
    if (!raw)
        return cudaErrorMemoryAllocation;
    base = (u8 *)(((u64)raw + align - 1) & ~(align - 1));
    mcuda_lock(MC_LOCK_RT);
    if (tab_insert(base, n, raw, type, flags)) {
        mcuda_unlock(MC_LOCK_RT);
        free(raw);
        return cudaErrorMemoryAllocation;
    }
    if (type == MCUDA_MEM_HOST)
        host_used += n;
    else
        dev_used += n;
    mcuda_unlock(MC_LOCK_RT);
    *out = base;
    return 0;
}

int mcuda_mem_alloc(void **out, unsigned long n, int type) { return mem_alloc(out, n, type, 0); }

int mcuda_mem_free(void *p, int types)
{
    long i;
    void *raw = 0;
    mcuda_lock(MC_LOCK_RT);
    i = tab_floor((u8 *)p);
    if (i >= 0 && tab[i].base == (u8 *)p && tab[i].raw && (types >> tab[i].type & 1)) {
        raw = tab[i].raw;
        if (tab[i].type == MCUDA_MEM_HOST)
            host_used -= tab[i].size;
        else
            dev_used -= tab[i].size;
        tab_remove(i);
    }
    mcuda_unlock(MC_LOCK_RT);
    if (!raw)
        return -1;
    free(raw);
    return 0;
}

int mcuda_mem_track(void *p, unsigned long n, int type)
{
    long i;
    int r = -1;
    mcuda_lock(MC_LOCK_RT);
    i = tab_floor((u8 *)p + n - 1);
    if (!p || !n || (i >= 0 && tab[i].base + tab[i].size > (u8 *)p))
        r = -2; /* it overlaps something that is already there */
    else
        r = tab_insert((u8 *)p, n, 0, type, 0);
    mcuda_unlock(MC_LOCK_RT);
    return r;
}

int mcuda_mem_untrack(void *p)
{
    long i;
    int r = -1;
    mcuda_lock(MC_LOCK_RT);
    i = tab_floor((u8 *)p);
    if (i >= 0 && tab[i].base == (u8 *)p && !tab[i].raw) {
        tab_remove(i);
        r = 0;
    }
    mcuda_unlock(MC_LOCK_RT);
    return r;
}

int mcuda_mem_charge(long delta)
{
    int r = 0;
    mcuda_lock(MC_LOCK_RT);
    if (delta < 0)
        dev_used -= (u64)-delta < dev_used ? (u64)-delta : dev_used;
    else if (dev_used > dev_total() || (u64)delta > dev_total() - dev_used)
        r = cudaErrorMemoryAllocation;
    else
        dev_used += (u64)delta;
    mcuda_unlock(MC_LOCK_RT);
    return r;
}

static int mem_find(const void *p, void **base, unsigned long *size, u32 *flags)
{
    long i;
    int type = 0;
    mcuda_lock(MC_LOCK_RT);
    i = tab_floor((const u8 *)p);
    if (i >= 0 && (u64)((const u8 *)p - tab[i].base) < tab[i].size) {
        type = tab[i].type;
        if (base)
            *base = tab[i].base;
        if (size)
            *size = tab[i].size;
        if (flags)
            *flags = tab[i].flags;
    }
    mcuda_unlock(MC_LOCK_RT);
    return type;
}

int mcuda_mem_find(const void *p, void **base, unsigned long *size) { return mem_find(p, base, size, 0); }

/* total: 24 GiB or the host's memory if that is less. free: what is left of the total, and no more than the
   host can still give: on Linux its free memory; elsewhere (no such call in the hosted library) its memory
   less everything this library handed out. */
void mcuda_mem_info(unsigned long *free_bytes, unsigned long *total_bytes)
{
    u64 total, fr, avail, used_dev, used_host, ram;
    mcuda_lock(MC_LOCK_RT);
    total = dev_total();
    ram = host_ram();
    used_dev = dev_used;
    used_host = host_used;
    mcuda_unlock(MC_LOCK_RT);
    fr = used_dev < total ? total - used_dev : 0;
#ifdef __linux__
    {
        struct sysinfo si;
        (void)used_host;
        (void)ram;
        if (sysinfo(&si) == 0) {
            avail = ((u64)si.freeram + (u64)si.bufferram) * (si.mem_unit ? si.mem_unit : 1);
            if (avail < fr)
                fr = avail;
        }
    }
#else
    avail = used_dev + used_host < ram ? ram - used_dev - used_host : 0;
    if (avail < fr)
        fr = avail;
#endif
    if (free_bytes)
        *free_bytes = fr;
    if (total_bytes)
        *total_bytes = total;
}

/* ---- device queries ---- */

int mcuda_dev_attr(int attr, int *value)
{
    int v;
    switch (attr) {
    case cudaDevAttrMaxThreadsPerBlock: v = MC_DEV_MAX_THREADS; break;
    case cudaDevAttrMaxBlockDimX: v = 1024; break;
    case cudaDevAttrMaxBlockDimY: v = 1024; break;
    case cudaDevAttrMaxBlockDimZ: v = 64; break;
    case cudaDevAttrMaxGridDimX: v = 2147483647; break;
    case cudaDevAttrMaxGridDimY: v = 65535; break;
    case cudaDevAttrMaxGridDimZ: v = 65535; break;
    case cudaDevAttrMaxSharedMemoryPerBlock: v = 49152; break;
    case cudaDevAttrTotalConstantMemory: v = 65536; break;
    case cudaDevAttrWarpSize: v = 32; break;
    case cudaDevAttrMaxPitch: v = 2147483647; break;
    case cudaDevAttrMaxRegistersPerBlock: v = 65536; break;
    case cudaDevAttrClockRate: v = 1695000; break;
    case cudaDevAttrTextureAlignment: v = 512; break;
    case cudaDevAttrGpuOverlap: v = 1; break;
    case cudaDevAttrMultiProcessorCount: v = MC_DEV_NSM; break;
    case cudaDevAttrKernelExecTimeout: v = 0; break;
    case cudaDevAttrIntegrated: v = 0; break;
    case cudaDevAttrCanMapHostMemory: v = 1; break;
    case cudaDevAttrComputeMode: v = cudaComputeModeDefault; break;
    case cudaDevAttrSurfaceAlignment: v = 512; break;
    case cudaDevAttrConcurrentKernels: v = 1; break;
    case cudaDevAttrEccEnabled: v = 0; break;
    case cudaDevAttrPciBusId: v = 1; break;
    case cudaDevAttrPciDeviceId: v = 0; break;
    case cudaDevAttrTccDriver: v = 0; break;
    case cudaDevAttrMemoryClockRate: v = 9751000; break;
    case cudaDevAttrGlobalMemoryBusWidth: v = 384; break;
    case cudaDevAttrL2CacheSize: v = 6291456; break;
    case cudaDevAttrMaxThreadsPerMultiProcessor: v = 1536; break;
    case cudaDevAttrAsyncEngineCount: v = 2; break;
    case cudaDevAttrUnifiedAddressing: v = 1; break;
    case cudaDevAttrPciDomainId: v = 0; break;
    case cudaDevAttrTexturePitchAlignment: v = 32; break;
    case cudaDevAttrComputeCapabilityMajor: v = MC_DEV_SM_MAJOR; break;
    case cudaDevAttrComputeCapabilityMinor: v = MC_DEV_SM_MINOR; break;
    case cudaDevAttrStreamPrioritiesSupported: v = 1; break;
    case cudaDevAttrGlobalL1CacheSupported: v = 1; break;
    case cudaDevAttrLocalL1CacheSupported: v = 1; break;
    case cudaDevAttrMaxSharedMemoryPerMultiprocessor: v = (int)MC_DEV_MAX_SHARED; break;
    case cudaDevAttrMaxRegistersPerMultiprocessor: v = 65536; break;
    case cudaDevAttrManagedMemory: v = 1; break;
    case cudaDevAttrIsMultiGpuBoard: v = 0; break;
    case cudaDevAttrMultiGpuBoardGroupID: v = 0; break;
    case cudaDevAttrHostNativeAtomicSupported: v = 0; break;
    case cudaDevAttrSingleToDoublePrecisionPerfRatio: v = 32; break;
    case cudaDevAttrPageableMemoryAccess: v = 0; break;
    case cudaDevAttrConcurrentManagedAccess: v = 1; break;
    case cudaDevAttrComputePreemptionSupported: v = 1; break;
    case cudaDevAttrCanUseHostPointerForRegisteredMem: v = 1; break;
    case cudaDevAttrCooperativeLaunch: v = 1; break;
    case cudaDevAttrCooperativeMultiDeviceLaunch: v = 0; break;
    case cudaDevAttrMaxSharedMemoryPerBlockOptin: v = (int)MC_DEV_MAX_SHARED - 1024; break;
    case cudaDevAttrCanFlushRemoteWrites: v = 0; break;
    case cudaDevAttrHostRegisterSupported: v = 1; break;
    case cudaDevAttrPageableMemoryAccessUsesHostPageTables: v = 0; break;
    case cudaDevAttrDirectManagedMemAccessFromHost: v = 0; break;
    case 102: v = 1; break; /* CU_DEVICE_ATTRIBUTE_VIRTUAL_MEMORY_MANAGEMENT_SUPPORTED */
    case cudaDevAttrMaxBlocksPerMultiprocessor: v = 16; break;
    case cudaDevAttrMaxPersistingL2CacheSize: v = 4128768; break;
    case cudaDevAttrMaxAccessPolicyWindowSize: v = 134213632; break;
    case cudaDevAttrReservedSharedMemoryPerBlock: v = 1024; break;
    case cudaDevAttrSparseCudaArraySupported: v = 0; break;
    case cudaDevAttrHostRegisterReadOnlySupported: v = 1; break;
    case cudaDevAttrTimelineSemaphoreInteropSupported: v = 0; break;
    case cudaDevAttrMemoryPoolsSupported: v = 0; break;
    case cudaDevAttrGPUDirectRDMASupported: v = 0; break;
    case cudaDevAttrClusterLaunch: v = 0; break;
    default: return -1;
    }
    *value = v;
    return 0;
}

static int attr(int a)
{
    int v = 0;
    mcuda_dev_attr(a, &v);
    return v;
}

cudaError_t cudaGetDeviceProperties(struct cudaDeviceProp *p, int device)
{
    static const char uuid[16] = {'m', 'c', 'u', 'd', 'a', '-', 'c', 'p', 'u', '-', '0', '0', '0', '0', '0', '0'};
    if (!p)
        return ret(cudaErrorInvalidValue);
    if (device != 0 || !dev_count())
        return ret(cudaErrorInvalidDevice);
    memset(p, 0, sizeof *p);
    strcpy(p->name, MC_DEV_NAME);
    memcpy(p->uuid.bytes, uuid, 16);
    p->totalGlobalMem = dev_total_now();
    p->sharedMemPerBlock = 49152;
    p->regsPerBlock = 65536;
    p->warpSize = 32;
    p->memPitch = 2147483647;
    p->maxThreadsPerBlock = MC_DEV_MAX_THREADS;
    p->maxThreadsDim[0] = 1024;
    p->maxThreadsDim[1] = 1024;
    p->maxThreadsDim[2] = 64;
    p->maxGridSize[0] = 2147483647;
    p->maxGridSize[1] = 65535;
    p->maxGridSize[2] = 65535;
    p->clockRate = attr(cudaDevAttrClockRate);
    p->totalConstMem = 65536;
    p->major = MC_DEV_SM_MAJOR;
    p->minor = MC_DEV_SM_MINOR;
    p->textureAlignment = 512;
    p->texturePitchAlignment = 32;
    p->deviceOverlap = 1;
    p->multiProcessorCount = MC_DEV_NSM;
    p->canMapHostMemory = 1;
    p->computeMode = cudaComputeModeDefault;
    /* no textures or surfaces: their limits stay 0 */
    p->surfaceAlignment = 512;
    p->concurrentKernels = 1;
    p->pciBusID = 1;
    p->asyncEngineCount = 2;
    p->unifiedAddressing = 1;
    p->memoryClockRate = attr(cudaDevAttrMemoryClockRate);
    p->l2CacheSize = attr(cudaDevAttrL2CacheSize);
    p->persistingL2CacheMaxSize = attr(cudaDevAttrMaxPersistingL2CacheSize);
    p->maxThreadsPerMultiProcessor = 1536;
    p->streamPrioritiesSupported = 1;
    p->globalL1CacheSupported = 1;
    p->localL1CacheSupported = 1;
    p->sharedMemPerMultiprocessor = MC_DEV_MAX_SHARED;
    p->regsPerMultiprocessor = 65536;
    p->managedMemory = 1;
    p->singleToDoublePrecisionPerfRatio = 32;
    p->concurrentManagedAccess = 1;
    p->computePreemptionSupported = 1;
    p->canUseHostPointerForRegisteredMem = 1;
    p->cooperativeLaunch = 1;
    p->sharedMemPerBlockOptin = MC_DEV_MAX_SHARED - 1024;
    p->maxBlocksPerMultiProcessor = 16;
    p->accessPolicyMaxWindowSize = attr(cudaDevAttrMaxAccessPolicyWindowSize);
    p->reservedSharedMemPerBlock = 1024;
    p->hostRegisterSupported = 1;
    p->hostRegisterReadOnlySupported = 1;
    return cudaSuccess;
}

cudaError_t cudaDeviceGetAttribute(int *value, enum cudaDeviceAttr a, int device)
{
    if (!value)
        return ret(cudaErrorInvalidValue);
    if (device != 0 || !dev_count())
        return ret(cudaErrorInvalidDevice);
    if (mcuda_dev_attr((int)a, value))
        return ret(cudaErrorInvalidValue);
    return cudaSuccess;
}

cudaError_t cudaGetDeviceCount(int *count)
{
    if (!count)
        return ret(cudaErrorInvalidValue);
    *count = dev_count();
    return ret(*count ? cudaSuccess : cudaErrorNoDevice);
}

cudaError_t cudaGetDevice(int *device)
{
    if (!device)
        return ret(cudaErrorInvalidValue);
    NEED_DEVICE();
    *device = 0;
    return cudaSuccess;
}

cudaError_t cudaSetDevice(int device)
{
    NEED_DEVICE();
    return ret(device == 0 ? cudaSuccess : cudaErrorInvalidDevice);
}

cudaError_t cudaInitDevice(int device, unsigned int deviceFlags, unsigned int flags)
{
    (void)deviceFlags;
    (void)flags;
    NEED_DEVICE();
    return ret(device == 0 ? cudaSuccess : cudaErrorInvalidDevice);
}

static unsigned int dev_flags;

cudaError_t cudaSetDeviceFlags(unsigned int flags)
{
    if (flags & ~(unsigned)cudaDeviceMask)
        return ret(cudaErrorInvalidValue);
    mcuda_lock(MC_LOCK_RT);
    dev_flags = flags;
    mcuda_unlock(MC_LOCK_RT);
    return cudaSuccess;
}

cudaError_t cudaGetDeviceFlags(unsigned int *flags)
{
    if (!flags)
        return ret(cudaErrorInvalidValue);
    mcuda_lock(MC_LOCK_RT);
    *flags = dev_flags;
    mcuda_unlock(MC_LOCK_RT);
    return cudaSuccess;
}

cudaError_t cudaDeviceSynchronize(void)
{
    NEED_DEVICE();
    return cudaSuccess;
}

cudaError_t cudaDeviceReset(void) { return cudaSuccess; }

/* There is no second device to be a peer. */
cudaError_t cudaDeviceCanAccessPeer(int *can, int device, int peer)
{
    if (!can)
        return ret(cudaErrorInvalidValue);
    *can = 0;
    if (device != 0 || peer != 0 || !dev_count())
        return ret(cudaErrorInvalidDevice);
    return cudaSuccess;
}

cudaError_t cudaDeviceEnablePeerAccess(int peer, unsigned int flags)
{
    if (flags)
        return ret(cudaErrorInvalidValue);
    (void)peer;
    return ret(cudaErrorInvalidDevice);
}

cudaError_t cudaDeviceDisablePeerAccess(int peer)
{
    (void)peer;
    return ret(cudaErrorPeerAccessNotEnabled);
}

/* Lower numbers are higher priorities. */
#define PRIO_LEAST 0
#define PRIO_GREATEST (-5)

cudaError_t cudaDeviceGetStreamPriorityRange(int *least, int *greatest)
{
    if (least)
        *least = PRIO_LEAST;
    if (greatest)
        *greatest = PRIO_GREATEST;
    return cudaSuccess;
}

cudaError_t cudaDriverGetVersion(int *v)
{
    if (!v)
        return ret(cudaErrorInvalidValue);
    *v = CUDA_VERSION;
    return cudaSuccess;
}

cudaError_t cudaRuntimeGetVersion(int *v)
{
    if (!v)
        return ret(cudaErrorInvalidValue);
    *v = CUDART_VERSION;
    return cudaSuccess;
}

cudaError_t cudaGetDriverEntryPointByVersion(const char *proc, void **fn, unsigned int version,
                                             enum cudaGetDriverEntryPointFlags flags,
                                             cudaDriverEntryPointQueryResult *status)
{
    void *f;
    (void)flags;
    if (!proc || !fn)
        return ret(cudaErrorInvalidValue);
    *fn = 0;
    NEED_DEVICE();
    if (version > CUDA_VERSION) {
        if (status)
            *status = cudaDriverEntryPointVersionNotSufficent;
        return ret(cudaErrorNotSupported);
    }
    f = mcuda_driver_entry(proc);
    if (!f) {
        if (status)
            *status = cudaDriverEntryPointSymbolNotFound;
        return ret(cudaErrorSymbolNotFound);
    }
    *fn = f;
    if (status)
        *status = cudaDriverEntryPointSuccess;
    return cudaSuccess;
}

cudaError_t cudaFuncSetAttribute(const void *func, enum cudaFuncAttribute a, int value)
{
    (void)func;
    (void)a;
    (void)value;
    return cudaSuccess;
}

cudaError_t cudaFuncGetAttributes(struct cudaFuncAttributes *attr, const void *func)
{
    (void)func;
    if (!attr)
        return ret(cudaErrorInvalidValue);
    memset(attr, 0, sizeof *attr);
    return cudaSuccess;
}

cudaError_t cudaFuncSetCacheConfig(const void *func, enum cudaFuncCache c)
{
    (void)func;
    (void)c;
    return cudaSuccess;
}

/* What fits on one multiprocessor by threads (1536), by shared memory (102400 bytes) and by the limit of
   16 blocks; the kernel's registers are unknown and not counted. */
cudaError_t cudaOccupancyMaxActiveBlocksPerMultiprocessorWithFlags(int *n, const void *func, int block_size,
                                                                   size_t smem, unsigned int flags)
{
    u64 b;
    (void)func;
    (void)flags;
    if (!n || block_size <= 0)
        return ret(cudaErrorInvalidValue);
    b = 1536 / (u64)block_size;
    if (block_size > MC_DEV_MAX_THREADS || smem > MC_DEV_MAX_SHARED)
        b = 0;
    else if (smem && MC_DEV_MAX_SHARED / smem < b)
        b = MC_DEV_MAX_SHARED / smem;
    if (b > 16)
        b = 16;
    *n = (int)b;
    return cudaSuccess;
}

cudaError_t cudaOccupancyMaxActiveBlocksPerMultiprocessor(int *n, const void *func, int block_size, size_t smem)
{
    return cudaOccupancyMaxActiveBlocksPerMultiprocessorWithFlags(n, func, block_size, smem, 0);
}

/* ---- memory ---- */

cudaError_t cudaMalloc(void **p, size_t n)
{
    if (!p)
        return ret(cudaErrorInvalidValue);
    *p = 0;
    NEED_DEVICE();
    if (!n)
        return cudaSuccess;
    return ret(mem_alloc(p, n, MCUDA_MEM_DEVICE, 0));
}

cudaError_t cudaMallocManaged(void **p, size_t n, unsigned int flags)
{
    if (!p || !n || (flags != cudaMemAttachGlobal && flags != cudaMemAttachHost))
        return ret(cudaErrorInvalidValue);
    *p = 0;
    NEED_DEVICE();
    return ret(mem_alloc(p, n, MCUDA_MEM_MANAGED, 0));
}

cudaError_t cudaFree(void *p)
{
    if (!p)
        return cudaSuccess;
    if (mcuda_mem_free(p, 1 << MCUDA_MEM_DEVICE | 1 << MCUDA_MEM_MANAGED))
        return ret(cudaErrorInvalidValue);
    return cudaSuccess;
}

cudaError_t cudaHostAlloc(void **p, size_t n, unsigned int flags)
{
    if (!p)
        return ret(cudaErrorInvalidValue);
    *p = 0;
    if (flags & ~(unsigned)(cudaHostAllocPortable | cudaHostAllocMapped | cudaHostAllocWriteCombined))
        return ret(cudaErrorInvalidValue);
    NEED_DEVICE();
    if (!n)
        return cudaSuccess;
    return ret(mem_alloc(p, n, MCUDA_MEM_HOST, flags));
}

cudaError_t cudaMallocHost(void **p, size_t n) { return cudaHostAlloc(p, n, cudaHostAllocDefault); }

cudaError_t cudaFreeHost(void *p)
{
    if (!p)
        return cudaSuccess;
    if (mcuda_mem_free(p, 1 << MCUDA_MEM_HOST))
        return ret(cudaErrorInvalidValue);
    return cudaSuccess;
}

/* With unified addressing every pinned host buffer is visible to the device at its own address. */
cudaError_t cudaHostGetDevicePointer(void **dev, void *host, unsigned int flags)
{
    if (!dev || flags)
        return ret(cudaErrorInvalidValue);
    *dev = 0;
    if (mem_find(host, 0, 0, 0) != MCUDA_MEM_HOST)
        return ret(cudaErrorInvalidValue);
    *dev = host;
    return cudaSuccess;
}

cudaError_t cudaHostGetFlags(unsigned int *flags, void *host)
{
    u32 f = 0;
    if (!flags || mem_find(host, 0, 0, &f) != MCUDA_MEM_HOST)
        return ret(cudaErrorInvalidValue);
    *flags = f | cudaHostAllocMapped;
    return cudaSuccess;
}

cudaError_t cudaHostRegister(void *p, size_t n, unsigned int flags)
{
    int r;
    (void)flags;
    if (!p || !n)
        return ret(cudaErrorInvalidValue);
    r = mcuda_mem_track(p, n, MCUDA_MEM_HOST);
    if (r == -2)
        return ret(cudaErrorHostMemoryAlreadyRegistered);
    return ret(r ? cudaErrorMemoryAllocation : cudaSuccess);
}

cudaError_t cudaHostUnregister(void *p)
{
    if (mem_find(p, 0, 0, 0) != MCUDA_MEM_HOST || mcuda_mem_untrack(p))
        return ret(cudaErrorHostMemoryNotRegistered);
    return cudaSuccess;
}

cudaError_t cudaMemGetInfo(size_t *fr, size_t *total)
{
    unsigned long f, t;
    NEED_DEVICE();
    mcuda_mem_info(&f, &t);
    if (fr)
        *fr = f;
    if (total)
        *total = t;
    return cudaSuccess;
}

/* Memory that this library did not hand out (the stack, malloc, a device global) is unregistered. */
cudaError_t cudaPointerGetAttributes(struct cudaPointerAttributes *a, const void *p)
{
    int type;
    if (!a || !p)
        return ret(cudaErrorInvalidValue);
    memset(a, 0, sizeof *a);
    type = mem_find(p, 0, 0, 0);
    a->type = (enum cudaMemoryType)type;
    a->device = 0;
    a->devicePointer = type ? (void *)p : 0;
    a->hostPointer = type == MCUDA_MEM_DEVICE ? 0 : (void *)p;
    return cudaSuccess;
}

static int copy(void *dst, const void *src, size_t n, enum cudaMemcpyKind kind)
{
    if ((unsigned)kind > (unsigned)cudaMemcpyDefault)
        return cudaErrorInvalidMemcpyDirection;
    if (!n)
        return 0;
    if (!dst || !src)
        return cudaErrorInvalidValue;
    memmove(dst, src, n);
    return 0;
}

static int copy2d(void *dst, size_t dpitch, const void *src, size_t spitch, size_t w, size_t h, enum cudaMemcpyKind kind)
{
    size_t y;
    if ((unsigned)kind > (unsigned)cudaMemcpyDefault)
        return cudaErrorInvalidMemcpyDirection;
    if (!w || !h)
        return 0;
    if (!dst || !src)
        return cudaErrorInvalidValue;
    if (w > dpitch || w > spitch)
        return cudaErrorInvalidPitchValue;
    for (y = 0; y < h; y++)
        memmove((u8 *)dst + y * dpitch, (const u8 *)src + y * spitch, w);
    return 0;
}

#define NEED_STREAM(s)                                  \
    do {                                                \
        if (!mcuda_stream_ok(s))                        \
            return ret(cudaErrorInvalidResourceHandle); \
    } while (0)

cudaError_t cudaMemcpy(void *dst, const void *src, size_t n, enum cudaMemcpyKind kind) { return ret(copy(dst, src, n, kind)); }

cudaError_t cudaMemcpyAsync(void *dst, const void *src, size_t n, enum cudaMemcpyKind kind, cudaStream_t s)
{
    NEED_STREAM(s);
    return ret(copy(dst, src, n, kind));
}

cudaError_t cudaMemcpyWithAttributesAsync(void *dst, const void *src, size_t n, struct cudaMemcpyAttributes *a, cudaStream_t s)
{
    if (!a || a->srcAccessOrder < cudaMemcpySrcAccessOrderStream || a->srcAccessOrder > cudaMemcpySrcAccessOrderAny)
        return ret(cudaErrorInvalidValue);
    NEED_STREAM(s);
    return ret(copy(dst, src, n, cudaMemcpyDefault));
}

cudaError_t cudaMemcpyPeer(void *dst, int ddev, const void *src, int sdev, size_t n)
{
    if (ddev != 0 || sdev != 0)
        return ret(cudaErrorInvalidDevice);
    return ret(copy(dst, src, n, cudaMemcpyDeviceToDevice));
}

cudaError_t cudaMemcpyPeerAsync(void *dst, int ddev, const void *src, int sdev, size_t n, cudaStream_t s)
{
    NEED_STREAM(s);
    return cudaMemcpyPeer(dst, ddev, src, sdev, n);
}

cudaError_t cudaMemcpy2D(void *dst, size_t dpitch, const void *src, size_t spitch, size_t w, size_t h, enum cudaMemcpyKind kind)
{
    return ret(copy2d(dst, dpitch, src, spitch, w, h, kind));
}

cudaError_t cudaMemcpy2DAsync(void *dst, size_t dpitch, const void *src, size_t spitch, size_t w, size_t h,
                              enum cudaMemcpyKind kind, cudaStream_t s)
{
    NEED_STREAM(s);
    return ret(copy2d(dst, dpitch, src, spitch, w, h, kind));
}

/* Between two pitched pointers: extent.width and pos.x are in bytes; a slice is pitch * ysize bytes. */
cudaError_t cudaMemcpy3D(const struct cudaMemcpy3DParms *p)
{
    size_t z;
    const u8 *s;
    u8 *d;
    int e;
    if (!p)
        return ret(cudaErrorInvalidValue);
    if (p->srcArray || p->dstArray)
        return ret(cudaErrorNotSupported);
    if (!p->extent.width || !p->extent.height || !p->extent.depth)
        return ret((unsigned)p->kind > (unsigned)cudaMemcpyDefault ? cudaErrorInvalidMemcpyDirection : cudaSuccess);
    if (!p->srcPtr.ptr || !p->dstPtr.ptr)
        return ret(cudaErrorInvalidValue);
    if (p->srcPos.x + p->extent.width > p->srcPtr.pitch || p->dstPos.x + p->extent.width > p->dstPtr.pitch)
        return ret(cudaErrorInvalidPitchValue);
    if ((p->extent.depth > 1 || p->srcPos.z || p->dstPos.z) &&
        (p->srcPos.y + p->extent.height > p->srcPtr.ysize || p->dstPos.y + p->extent.height > p->dstPtr.ysize))
        return ret(cudaErrorInvalidValue);
    for (z = 0; z < p->extent.depth; z++) {
        s = (const u8 *)p->srcPtr.ptr + ((p->srcPos.z + z) * p->srcPtr.ysize + p->srcPos.y) * p->srcPtr.pitch + p->srcPos.x;
        d = (u8 *)p->dstPtr.ptr + ((p->dstPos.z + z) * p->dstPtr.ysize + p->dstPos.y) * p->dstPtr.pitch + p->dstPos.x;
        e = copy2d(d, p->dstPtr.pitch, s, p->srcPtr.pitch, p->extent.width, p->extent.height, p->kind);
        if (e)
            return ret(e);
    }
    return cudaSuccess;
}

cudaError_t cudaMemcpy3DAsync(const struct cudaMemcpy3DParms *p, cudaStream_t s)
{
    NEED_STREAM(s);
    return cudaMemcpy3D(p);
}

cudaError_t cudaMemcpyToSymbol(const void *sym, const void *src, size_t n, size_t off, enum cudaMemcpyKind kind)
{
    if (!sym)
        return ret(cudaErrorInvalidSymbol);
    if (kind != cudaMemcpyHostToDevice && kind != cudaMemcpyDeviceToDevice && kind != cudaMemcpyDefault)
        return ret(cudaErrorInvalidMemcpyDirection);
    return ret(copy((u8 *)sym + off, src, n, kind));
}

cudaError_t cudaMemcpyToSymbolAsync(const void *sym, const void *src, size_t n, size_t off, enum cudaMemcpyKind kind, cudaStream_t s)
{
    NEED_STREAM(s);
    return cudaMemcpyToSymbol(sym, src, n, off, kind);
}

cudaError_t cudaMemcpyFromSymbol(void *dst, const void *sym, size_t n, size_t off, enum cudaMemcpyKind kind)
{
    if (!sym)
        return ret(cudaErrorInvalidSymbol);
    if (kind != cudaMemcpyDeviceToHost && kind != cudaMemcpyDeviceToDevice && kind != cudaMemcpyDefault)
        return ret(cudaErrorInvalidMemcpyDirection);
    return ret(copy(dst, (const u8 *)sym + off, n, kind));
}

cudaError_t cudaMemcpyFromSymbolAsync(void *dst, const void *sym, size_t n, size_t off, enum cudaMemcpyKind kind, cudaStream_t s)
{
    NEED_STREAM(s);
    return cudaMemcpyFromSymbol(dst, sym, n, off, kind);
}

/* what the templates of mcuda_device.h call (a reference to the symbol cannot go to the functions above) */
int __mcuda_to_symbol(const void *sym, const void *src, unsigned long n, unsigned long off, int kind)
{
    return (int)cudaMemcpyToSymbol(sym, src, n, off, (enum cudaMemcpyKind)kind);
}

int __mcuda_from_symbol(void *dst, const void *sym, unsigned long n, unsigned long off, int kind)
{
    return (int)cudaMemcpyFromSymbol(dst, sym, n, off, (enum cudaMemcpyKind)kind);
}


cudaError_t cudaMemset(void *p, int value, size_t n)
{
    if (!n)
        return cudaSuccess;
    if (!p)
        return ret(cudaErrorInvalidValue);
    memset(p, value, n);
    return cudaSuccess;
}

cudaError_t cudaMemsetAsync(void *p, int value, size_t n, cudaStream_t s)
{
    NEED_STREAM(s);
    return cudaMemset(p, value, n);
}

cudaError_t cudaMemset2D(void *p, size_t pitch, int value, size_t w, size_t h)
{
    size_t y;
    if (!w || !h)
        return cudaSuccess;
    if (!p)
        return ret(cudaErrorInvalidValue);
    if (w > pitch)
        return ret(cudaErrorInvalidPitchValue);
    for (y = 0; y < h; y++)
        memset((u8 *)p + y * pitch, value, w);
    return cudaSuccess;
}

cudaError_t cudaMemset2DAsync(void *p, size_t pitch, int value, size_t w, size_t h, cudaStream_t s)
{
    NEED_STREAM(s);
    return cudaMemset2D(p, pitch, value, w, h);
}

/* ---- streams and events ----
   Handles point at small records that are recycled through a free list and never returned to the heap, so a
   stale handle is recognised (by its magic number) instead of read from freed memory. */

#define MAGIC_STREAM 0x5354524du /* "STRM" */
#define MAGIC_EVENT 0x45564e54u  /* "EVNT" */

struct CUstream_st {
    u32 magic;
    u32 flags;
    int priority;
    struct CUstream_st *next;
};

struct CUevent_st {
    u32 magic;
    u32 flags;
    int recorded;
    struct timespec t;
    struct CUevent_st *next;
};

static struct CUstream_st *free_streams;
static struct CUevent_st *free_events;

int mcuda_stream_ok(void *s)
{
    if ((u64)s <= 2) /* 0, cudaStreamLegacy, cudaStreamPerThread */
        return 1;
    if ((u64)s < 4096 || ((u64)s & 7))
        return 0;
    return ((struct CUstream_st *)s)->magic == MAGIC_STREAM;
}

static int event_ok(cudaEvent_t e) { return (u64)e >= 4096 && !((u64)e & 7) && e->magic == MAGIC_EVENT; }

cudaError_t cudaStreamCreateWithPriority(cudaStream_t *ps, unsigned int flags, int priority)
{
    struct CUstream_st *s;
    if (!ps || (flags & ~(unsigned)cudaStreamNonBlocking))
        return ret(cudaErrorInvalidValue);
    *ps = 0;
    NEED_DEVICE();
    mcuda_lock(MC_LOCK_RT);
    s = free_streams;
    if (s)
        free_streams = s->next;
    mcuda_unlock(MC_LOCK_RT);
    if (!s)
        s = (struct CUstream_st *)malloc(sizeof *s);
    if (!s)
        return ret(cudaErrorMemoryAllocation);
    if (priority > PRIO_LEAST) /* out of range: clamped, as the real one does */
        priority = PRIO_LEAST;
    if (priority < PRIO_GREATEST)
        priority = PRIO_GREATEST;
    s->flags = flags;
    s->priority = priority;
    s->next = 0;
    s->magic = MAGIC_STREAM;
    *ps = s;
    return cudaSuccess;
}

cudaError_t cudaStreamCreateWithFlags(cudaStream_t *ps, unsigned int flags) { return cudaStreamCreateWithPriority(ps, flags, PRIO_LEAST); }
cudaError_t cudaStreamCreate(cudaStream_t *ps) { return cudaStreamCreateWithPriority(ps, cudaStreamDefault, PRIO_LEAST); }

cudaError_t cudaStreamDestroy(cudaStream_t s)
{
    if ((u64)s <= 2 || !mcuda_stream_ok(s)) /* the default streams cannot be destroyed */
        return ret(cudaErrorInvalidResourceHandle);
    mcuda_lock(MC_LOCK_RT);
    s->magic = 0;
    s->next = free_streams;
    free_streams = s;
    mcuda_unlock(MC_LOCK_RT);
    return cudaSuccess;
}

cudaError_t cudaStreamGetPriority(cudaStream_t s, int *priority)
{
    NEED_STREAM(s);
    if (!priority)
        return ret(cudaErrorInvalidValue);
    *priority = (u64)s <= 2 ? PRIO_LEAST : s->priority;
    return cudaSuccess;
}

cudaError_t cudaStreamGetFlags(cudaStream_t s, unsigned int *flags)
{
    NEED_STREAM(s);
    if (!flags)
        return ret(cudaErrorInvalidValue);
    *flags = (u64)s <= 2 ? cudaStreamDefault : s->flags;
    return cudaSuccess;
}

cudaError_t cudaStreamSynchronize(cudaStream_t s)
{
    NEED_STREAM(s);
    return cudaSuccess;
}

cudaError_t cudaStreamQuery(cudaStream_t s)
{
    NEED_STREAM(s);
    return cudaSuccess;
}

cudaError_t cudaStreamWaitEvent(cudaStream_t s, cudaEvent_t e, unsigned int flags)
{
    NEED_STREAM(s);
    if (!event_ok(e))
        return ret(cudaErrorInvalidResourceHandle);
    if (flags & ~(unsigned)cudaEventWaitExternal)
        return ret(cudaErrorInvalidValue);
    return cudaSuccess;
}

cudaError_t cudaEventCreateWithFlags(cudaEvent_t *pe, unsigned int flags)
{
    struct CUevent_st *e;
    if (!pe || (flags & ~(unsigned)(cudaEventBlockingSync | cudaEventDisableTiming | cudaEventInterprocess)))
        return ret(cudaErrorInvalidValue);
    *pe = 0;
    if ((flags & cudaEventInterprocess) && !(flags & cudaEventDisableTiming))
        return ret(cudaErrorInvalidValue);
    NEED_DEVICE();
    mcuda_lock(MC_LOCK_RT);
    e = free_events;
    if (e)
        free_events = e->next;
    mcuda_unlock(MC_LOCK_RT);
    if (!e)
        e = (struct CUevent_st *)malloc(sizeof *e);
    if (!e)
        return ret(cudaErrorMemoryAllocation);
    e->flags = flags;
    e->recorded = 0;
    e->t.tv_sec = 0;
    e->t.tv_nsec = 0;
    e->next = 0;
    e->magic = MAGIC_EVENT;
    *pe = e;
    return cudaSuccess;
}

cudaError_t cudaEventCreate(cudaEvent_t *pe) { return cudaEventCreateWithFlags(pe, cudaEventDefault); }

cudaError_t cudaEventDestroy(cudaEvent_t e)
{
    if (!event_ok(e))
        return ret(cudaErrorInvalidResourceHandle);
    mcuda_lock(MC_LOCK_RT);
    e->magic = 0;
    e->next = free_events;
    free_events = e;
    mcuda_unlock(MC_LOCK_RT);
    return cudaSuccess;
}

cudaError_t cudaEventRecord(cudaEvent_t e, cudaStream_t s)
{
    struct timespec t;
    if (!event_ok(e))
        return ret(cudaErrorInvalidResourceHandle);
    NEED_STREAM(s);
    t.tv_sec = 0;
    t.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &t);
    mcuda_lock(MC_LOCK_RT);
    e->t = t;
    e->recorded = 1;
    mcuda_unlock(MC_LOCK_RT);
    return cudaSuccess;
}

/* An event is complete as soon as it is recorded, and one that was never recorded counts as complete too. */
cudaError_t cudaEventQuery(cudaEvent_t e) { return ret(event_ok(e) ? cudaSuccess : cudaErrorInvalidResourceHandle); }
cudaError_t cudaEventSynchronize(cudaEvent_t e) { return ret(event_ok(e) ? cudaSuccess : cudaErrorInvalidResourceHandle); }

cudaError_t cudaEventElapsedTime(float *ms, cudaEvent_t a, cudaEvent_t b)
{
    double d;
    int ok;
    if (!ms)
        return ret(cudaErrorInvalidValue);
    if (!event_ok(a) || !event_ok(b))
        return ret(cudaErrorInvalidResourceHandle);
    mcuda_lock(MC_LOCK_RT);
    ok = a->recorded && b->recorded && !((a->flags | b->flags) & cudaEventDisableTiming);
    d = (double)(b->t.tv_sec - a->t.tv_sec) * 1e3 + (double)(b->t.tv_nsec - a->t.tv_nsec) * 1e-6;
    mcuda_unlock(MC_LOCK_RT);
    if (!ok)
        return ret(cudaErrorInvalidResourceHandle);
    *ms = (float)d;
    return cudaSuccess;
}

/* ---- launches ---- */

int mcuda_launch_check(void *func, const unsigned grid[3], const unsigned block[3], unsigned long shared_bytes)
{
    McFunc *f = (McFunc *)func;
    u32 maxntid[3], reqntid[3];
    u64 sshared = 0, threads;
    int i;
    for (i = 0; i < 3; i++) {
        maxntid[i] = 0;
        reqntid[i] = 0;
        if (!grid[i] || !block[i])
            return cudaErrorInvalidValue; /* what the driver says today, not cudaErrorInvalidConfiguration */
    }
    if (block[0] > 1024 || block[1] > 1024 || block[2] > 64 || grid[0] > 2147483647u || grid[1] > 65535 || grid[2] > 65535)
        return cudaErrorInvalidValue; /* what the driver says today, not cudaErrorInvalidConfiguration */
    threads = (u64)block[0] * block[1] * block[2];
    if (threads > MC_DEV_MAX_THREADS)
        return cudaErrorInvalidValue; /* what the driver says today, not cudaErrorInvalidConfiguration */
    mc_func_limits(f, maxntid, reqntid, &sshared);
    if (maxntid[0] && threads > (u64)maxntid[0] * (maxntid[1] ? maxntid[1] : 1) * (maxntid[2] ? maxntid[2] : 1))
        return cudaErrorInvalidValue; /* what the driver says today, not cudaErrorInvalidConfiguration */
    if (reqntid[0])
        for (i = 0; i < 3; i++)
            if (block[i] != (reqntid[i] ? reqntid[i] : 1))
                return cudaErrorInvalidValue; /* what the driver says today, not cudaErrorInvalidConfiguration */
    if (shared_bytes > MC_DEV_MAX_SHARED || sshared > MC_DEV_MAX_SHARED - shared_bytes)
        return cudaErrorInvalidValue;
    return 0;
}

/* Modules whose PTX did not parse: they are not parsed (and complained about) again. */
#define NBROKEN 64
static struct __mcuda_module *broken[NBROKEN];
static int nbroken;

static McModule *module_get(struct __mcuda_module *m)
{
    McModule *h;
    int i;
    mcuda_lock(MC_LOCK_MODULE);
    h = (McModule *)m->handle;
    for (i = 0; !h && i < nbroken; i++)
        if (broken[i] == m) {
            mcuda_unlock(MC_LOCK_MODULE);
            return 0;
        }
    if (!h && m->ptx && mcuda_ptx_plain(&m->ptx)) /* (packed by mcc: unpacked here, once) */
        fprintf(stderr, "mcuda: %s: damaged packed PTX\n", m->name ? m->name : "module");
    if (!h && m->ptx) {
        h = mc_module_load(m->ptx, m->name);
        if (h) {
            const struct __mcuda_sym *y;
            for (y = m->syms; y && y->name; y++)
                mc_module_bind(h, y->name, y->addr); /* a name the PTX does not mention is no error */
            m->handle = h;
        } else {
            fprintf(stderr, "mcuda: %s: %s\n", m->name ? m->name : "module", mc_error());
            if (nbroken < NBROKEN)
                broken[nbroken++] = m;
        }
    }
    mcuda_unlock(MC_LOCK_MODULE);
    return h;
}

/* Modules the program announced (__mcuda_register) and that are loaded when a name is found in none of the
   loaded ones: a kernel or a device function or variable that another translation unit defines. */
static struct __mcuda_module **registered;
static int nregistered, capregistered, nreg_loaded;

void __mcuda_register(struct __mcuda_module *m)
{
    if (!m)
        return;
    mcuda_lock(MC_LOCK_MODULE);
    if (nregistered == capregistered) {
        int cap = capregistered ? 2 * capregistered : 64;
        struct __mcuda_module **v = (struct __mcuda_module **)realloc(registered, (size_t)cap * sizeof *v);
        if (!v) {
            mcuda_unlock(MC_LOCK_MODULE);
            return;
        }
        registered = v;
        capregistered = cap;
    }
    registered[nregistered++] = m;
    mcuda_unlock(MC_LOCK_MODULE);
}

#ifdef __linux__
/* mcc puts the address of every translation unit's module record into the section mcuda_modules */
extern struct __mcuda_module *__start_mcuda_modules[] __attribute__((weak));
extern struct __mcuda_module *__stop_mcuda_modules[] __attribute__((weak));
#endif

static int load_registered(void)
{
    int n = 0;
#ifdef __linux__
    static int walked;
    if (!walked) {
        struct __mcuda_module **p = __start_mcuda_modules;
        walked = 1;
        for (; p && p < __stop_mcuda_modules; p++)
            __mcuda_register(*p);
    }
#endif
    mcuda_lock(MC_LOCK_MODULE);
    while (nreg_loaded < nregistered) {
        struct __mcuda_module *m = registered[nreg_loaded++];
        if (!m->handle && module_get(m))
            n++;
    }
    mcuda_unlock(MC_LOCK_MODULE);
    return n;
}

int __mcuda_launch(struct __mcuda_module *m, const char *kernel, const unsigned grid[3], const unsigned block[3],
                   unsigned long shared_bytes, void *stream, void **args)
{
    McModule *h;
    McFunc *f;
    int e;
    if (!m || !kernel || !grid || !block)
        return ret(cudaErrorInvalidValue);
    NEED_DEVICE();
    h = module_get(m);
    if (!h)
        return ret(cudaErrorInvalidPtx);
    mc_more_modules = load_registered;
    f = mc_module_func(h, kernel);
    if (!mc_func_defined(f))
        f = mc_find_kernel(kernel); /* defined by another translation unit */
    if (!f) {
        fprintf(stderr, "mcuda: %s: the kernel %s is defined in no module that is loaded or registered\n",
                m->name ? m->name : "module", kernel);
        return ret(cudaErrorInvalidDeviceFunction);
    }
    NEED_STREAM(stream);
    e = mcuda_launch_check(f, grid, block, shared_bytes);
    if (e)
        return ret(e);
    if (!args && mc_func_nparams(f) > 0)
        return ret(cudaErrorInvalidValue);
    return ret(mc_launch(f, grid, block, shared_bytes, args));
}

void *__mcuda_symbol(struct __mcuda_module *m, const char *name)
{
    McModule *h;
    u64 size = 0;
    const struct __mcuda_sym *y;
    if (!m || !name)
        return 0;
    for (y = m->syms; y && y->name; y++)
        if (strcmp(y->name, name) == 0)
            return y->addr;
    h = module_get(m);
    return h ? mc_module_global(h, name, &size) : 0;
}
