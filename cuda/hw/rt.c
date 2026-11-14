/* mcudahw: the cuda* run-time API on NVIDIA's driver. A stream is the driver's stream, an event its event,
   device memory its device memory; the device is device 0 with its primary context. */
#include "hw.h"
#include <mcuda.h>
#include <cuda_runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned char u8;

/* Every API function returns through here. */
static cudaError_t ret(int e)
{
    if (e)
        hw_err_set(e);
    return (cudaError_t)e;
}

#define ENTER() \
    do { \
        int e_ = hw_enter(); \
        if (e_) \
            return ret(e_); \
    } while (0)
#define DP(p) ((CUdeviceptr)(size_t)(p))

cudaError_t cudaGetLastError(void) { return (cudaError_t)hw_err_get(1); }
cudaError_t cudaPeekAtLastError(void) { return (cudaError_t)hw_err_get(0); }

#include "../errtab.h"

/* ---- the device ---- */

cudaError_t cudaGetDeviceCount(int *count)
{
    int e;
    if (!count)
        return ret(cudaErrorInvalidValue);
    *count = 0;
    e = hw_enter();
    if (e)
        return ret(e == 100 ? cudaErrorNoDevice : e);
    *count = 1; /* (only device 0 is used) */
    return cudaSuccess;
}

cudaError_t cudaGetDevice(int *device)
{
    if (!device)
        return ret(cudaErrorInvalidValue);
    ENTER();
    *device = 0;
    return cudaSuccess;
}

cudaError_t cudaSetDevice(int device)
{
    ENTER();
    return ret(device == 0 ? cudaSuccess : cudaErrorInvalidDevice);
}

cudaError_t cudaInitDevice(int device, unsigned int deviceFlags, unsigned int flags)
{
    (void)deviceFlags;
    (void)flags;
    ENTER();
    return ret(device == 0 ? cudaSuccess : cudaErrorInvalidDevice);
}

static unsigned int dev_flags;

cudaError_t cudaSetDeviceFlags(unsigned int flags)
{
    if (flags & ~(unsigned)cudaDeviceMask)
        return ret(cudaErrorInvalidValue);
    dev_flags = flags;
    return cudaSuccess;
}

cudaError_t cudaGetDeviceFlags(unsigned int *flags)
{
    if (!flags)
        return ret(cudaErrorInvalidValue);
    *flags = dev_flags;
    return cudaSuccess;
}

cudaError_t cudaDeviceSynchronize(void)
{
    ENTER();
    return ret(hw.CtxSynchronize());
}

cudaError_t cudaDeviceReset(void)
{
    ENTER();
    return ret(hw.CtxSynchronize());
}

static int attr(int a)
{
    int v = 0;
    hw.DeviceGetAttribute(&v, a, hw_dev);
    return v;
}

cudaError_t cudaGetDeviceProperties(struct cudaDeviceProp *p, int device)
{
    size_t total = 0;
    if (!p)
        return ret(cudaErrorInvalidValue);
    ENTER();
    if (device != 0)
        return ret(cudaErrorInvalidDevice);
    memset(p, 0, sizeof *p);
    hw.DeviceGetName(p->name, (int)sizeof p->name - 1, hw_dev);
    hw.DeviceGetUuid(&p->uuid, hw_dev);
    hw.DeviceTotalMem(&total, hw_dev);
    p->totalGlobalMem = total;
    p->maxThreadsPerBlock = attr(1);
    p->maxThreadsDim[0] = attr(2);
    p->maxThreadsDim[1] = attr(3);
    p->maxThreadsDim[2] = attr(4);
    p->maxGridSize[0] = attr(5);
    p->maxGridSize[1] = attr(6);
    p->maxGridSize[2] = attr(7);
    p->sharedMemPerBlock = (size_t)attr(8);
    p->totalConstMem = (size_t)attr(9);
    p->warpSize = attr(10);
    p->memPitch = (size_t)attr(11);
    p->regsPerBlock = attr(12);
    p->clockRate = attr(13);
    p->textureAlignment = (size_t)attr(14);
    p->deviceOverlap = attr(15);
    p->multiProcessorCount = attr(16);
    p->kernelExecTimeoutEnabled = attr(17);
    p->integrated = attr(18);
    p->canMapHostMemory = attr(19);
    p->computeMode = attr(20);
    p->concurrentKernels = attr(31);
    p->ECCEnabled = attr(32);
    p->pciBusID = attr(33);
    p->pciDeviceID = attr(34);
    p->tccDriver = attr(35);
    p->memoryClockRate = attr(36);
    p->l2CacheSize = attr(38);
    p->maxThreadsPerMultiProcessor = attr(39);
    p->asyncEngineCount = attr(40);
    p->unifiedAddressing = attr(41);
    p->pciDomainID = attr(50);
    p->texturePitchAlignment = (size_t)attr(51);
    p->major = attr(75);
    p->minor = attr(76);
    p->streamPrioritiesSupported = attr(78);
    p->globalL1CacheSupported = attr(79);
    p->localL1CacheSupported = attr(80);
    p->sharedMemPerMultiprocessor = (size_t)attr(81);
    p->regsPerMultiprocessor = attr(82);
    p->managedMemory = attr(83);
    p->isMultiGpuBoard = attr(84);
    p->multiGpuBoardGroupID = attr(85);
    p->hostNativeAtomicSupported = attr(86);
    p->singleToDoublePrecisionPerfRatio = attr(87);
    p->pageableMemoryAccess = attr(88);
    p->concurrentManagedAccess = attr(89);
    p->computePreemptionSupported = attr(90);
    p->canUseHostPointerForRegisteredMem = attr(91);
    p->cooperativeLaunch = attr(95);
    p->sharedMemPerBlockOptin = (size_t)attr(97);
    p->pageableMemoryAccessUsesHostPageTables = attr(100);
    p->directManagedMemAccessFromHost = attr(101);
    p->maxBlocksPerMultiProcessor = attr(106);
    p->accessPolicyMaxWindowSize = attr(109);
    p->reservedSharedMemPerBlock = (size_t)attr(111);
    p->hostRegisterSupported = attr(99);
    return cudaSuccess;
}

cudaError_t cudaDeviceGetAttribute(int *value, enum cudaDeviceAttr a, int device)
{
    if (!value)
        return ret(cudaErrorInvalidValue);
    ENTER();
    if (device != 0)
        return ret(cudaErrorInvalidDevice);
    return ret(hw.DeviceGetAttribute(value, (int)a, hw_dev));
}

/* Only device 0 is used, so there is no peer. */
cudaError_t cudaDeviceCanAccessPeer(int *can, int device, int peer)
{
    if (!can)
        return ret(cudaErrorInvalidValue);
    *can = 0;
    ENTER();
    if (device != 0 || peer != 0)
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

cudaError_t cudaDeviceGetStreamPriorityRange(int *least, int *greatest)
{
    int a = 0, b = 0, e;
    ENTER();
    e = hw.CtxGetStreamPriorityRange(&a, &b);
    if (least)
        *least = a;
    if (greatest)
        *greatest = b;
    return ret(e);
}

cudaError_t cudaDriverGetVersion(int *v)
{
    if (!v)
        return ret(cudaErrorInvalidValue);
    ENTER();
    return ret(hw.DriverGetVersion(v));
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
    (void)version;
    if (!proc || !fn)
        return ret(cudaErrorInvalidValue);
    *fn = 0;
    ENTER();
    f = hw_entry(proc);
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

/* func is the host address that the compiler gives a kernel. Where the module record lists it, the kernel
   is found and the driver is asked; where not, a launch that needs more dynamic shared memory than the
   default raises the kernel's limit itself (__mcuda_launch), and so the call can be left undone. */
cudaError_t cudaFuncSetAttribute(const void *func, enum cudaFuncAttribute a, int value)
{
    CUhandle f;
    ENTER();
    f = hw_kernel_of(func);
    if (!f)
        return cudaSuccess;
    return ret(hw.FuncSetAttribute(f, (int)a, value));
}

cudaError_t cudaFuncSetCacheConfig(const void *func, enum cudaFuncCache c)
{
    (void)func;
    (void)c;
    return cudaSuccess;
}

cudaError_t cudaOccupancyMaxActiveBlocksPerMultiprocessorWithFlags(int *n, const void *func, int block_size,
                                                                   size_t smem, unsigned int flags)
{
    CUhandle f;
    unsigned long b, lim;
    if (!n || block_size <= 0)
        return ret(cudaErrorInvalidValue);
    ENTER();
    f = hw_kernel_of(func);
    if (f)
        return ret(hw.Occupancy(n, f, block_size, smem, flags));
    /* the kernel is not known: what fits by threads and by shared memory */
    b = (unsigned long)attr(39) / (unsigned long)block_size;
    lim = (unsigned long)attr(81);
    if (block_size > attr(1) || smem > (size_t)attr(97))
        b = 0;
    else if (smem && lim / smem < b)
        b = lim / smem;
    if (b > (unsigned long)attr(106))
        b = (unsigned long)attr(106);
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
    CUdeviceptr d = 0;
    int e;
    if (!p)
        return ret(cudaErrorInvalidValue);
    *p = 0;
    ENTER();
    if (!n)
        return cudaSuccess;
    e = hw.MemAlloc(&d, n);
    *p = (void *)(size_t)d;
    return ret(e);
}

cudaError_t cudaMallocManaged(void **p, size_t n, unsigned int flags)
{
    CUdeviceptr d = 0;
    int e;
    if (!p || !n)
        return ret(cudaErrorInvalidValue);
    *p = 0;
    ENTER();
    e = hw.MemAllocManaged(&d, n, flags);
    *p = (void *)(size_t)d;
    return ret(e);
}

cudaError_t cudaFree(void *p)
{
    if (!p)
        return cudaSuccess;
    ENTER();
    return ret(hw.MemFree(DP(p)));
}

cudaError_t cudaHostAlloc(void **p, size_t n, unsigned int flags)
{
    if (!p)
        return ret(cudaErrorInvalidValue);
    *p = 0;
    ENTER();
    if (!n)
        return cudaSuccess;
    return ret(hw.MemHostAlloc(p, n, flags));
}

cudaError_t cudaMallocHost(void **p, size_t n) { return cudaHostAlloc(p, n, 0); }

cudaError_t cudaFreeHost(void *p)
{
    if (!p)
        return cudaSuccess;
    ENTER();
    return ret(hw.MemFreeHost(p));
}

cudaError_t cudaHostGetDevicePointer(void **dev, void *host, unsigned int flags)
{
    CUdeviceptr d = 0;
    int e;
    if (!dev)
        return ret(cudaErrorInvalidValue);
    ENTER();
    e = hw.MemHostGetDevicePointer(&d, host, flags);
    *dev = (void *)(size_t)d;
    return ret(e);
}

cudaError_t cudaHostGetFlags(unsigned int *flags, void *host)
{
    if (!flags)
        return ret(cudaErrorInvalidValue);
    ENTER();
    return ret(hw.MemHostGetFlags(flags, host));
}

cudaError_t cudaHostRegister(void *p, size_t n, unsigned int flags)
{
    ENTER();
    return ret(hw.MemHostRegister(p, n, flags));
}

cudaError_t cudaHostUnregister(void *p)
{
    ENTER();
    return ret(hw.MemHostUnregister(p));
}

cudaError_t cudaMemGetInfo(size_t *fr, size_t *total)
{
    size_t a = 0, b = 0;
    int e;
    ENTER();
    e = hw.MemGetInfo(&a, &b);
    if (fr)
        *fr = a;
    if (total)
        *total = b;
    return ret(e);
}

/* The driver's kind of memory at p: 0 if it knows nothing of it (ordinary host memory), 1 host, 2 device. */
static int memtype(const void *p)
{
    unsigned t = 0;
    if (hw.PointerGetAttribute(&t, 2 /* MEMORY_TYPE */, DP(p)))
        return 0;
    return (int)t;
}

cudaError_t cudaPointerGetAttributes(struct cudaPointerAttributes *a, const void *p)
{
    int t, managed = 0;
    CUdeviceptr d = 0;
    void *h = 0;
    if (!a)
        return ret(cudaErrorInvalidValue);
    ENTER();
    memset(a, 0, sizeof *a);
    t = memtype(p);
    if (!t) {
        a->type = cudaMemoryTypeUnregistered;
        a->hostPointer = (void *)p;
        return cudaSuccess;
    }
    hw.PointerGetAttribute(&managed, 8 /* IS_MANAGED */, DP(p));
    hw.PointerGetAttribute(&d, 3 /* DEVICE_POINTER */, DP(p));
    hw.PointerGetAttribute(&h, 4 /* HOST_POINTER */, DP(p));
    a->type = managed ? cudaMemoryTypeManaged : t == 1 ? cudaMemoryTypeHost : cudaMemoryTypeDevice;
    a->device = 0;
    a->devicePointer = (void *)(size_t)d;
    a->hostPointer = h;
    return cudaSuccess;
}

/* 1: the device's side of a copy is at p, by the kind, or for the default kind by asking the driver */
static int on_device(const void *p, enum cudaMemcpyKind kind, int is_dst)
{
    switch (kind) {
    case cudaMemcpyHostToHost:
        return 0;
    case cudaMemcpyHostToDevice:
        return is_dst;
    case cudaMemcpyDeviceToHost:
        return !is_dst;
    case cudaMemcpyDeviceToDevice:
        return 1;
    default:
        return memtype(p) == 2;
    }
}

static int copy(void *dst, const void *src, size_t n, enum cudaMemcpyKind kind, int async, cudaStream_t s)
{
    int dd, sd;
    if ((unsigned)kind > (unsigned)cudaMemcpyDefault)
        return cudaErrorInvalidMemcpyDirection;
    if (!n)
        return 0;
    if (!dst || !src)
        return cudaErrorInvalidValue;
    dd = on_device(dst, kind, 1);
    sd = on_device(src, kind, 0);
    if (dd && sd)
        return async ? hw.MemcpyDtoDAsync(DP(dst), DP(src), n, s) : hw.MemcpyDtoD(DP(dst), DP(src), n);
    if (dd)
        return async ? hw.MemcpyHtoDAsync(DP(dst), src, n, s) : hw.MemcpyHtoD(DP(dst), src, n);
    if (sd)
        return async ? hw.MemcpyDtoHAsync(dst, DP(src), n, s) : hw.MemcpyDtoH(dst, DP(src), n);
    if (async) /* (in the order of the stream) */
        hw.StreamSynchronize(s);
    memmove(dst, src, n);
    return 0;
}

static int copy2d(void *dst, size_t dpitch, const void *src, size_t spitch, size_t w, size_t h, enum cudaMemcpyKind kind,
                  int async, cudaStream_t s)
{
    struct HwMemcpy2D c;
    if ((unsigned)kind > (unsigned)cudaMemcpyDefault)
        return cudaErrorInvalidMemcpyDirection;
    if (!w || !h)
        return 0;
    if (!dst || !src)
        return cudaErrorInvalidValue;
    if (w > dpitch || w > spitch)
        return cudaErrorInvalidPitchValue;
    memset(&c, 0, sizeof c);
    if (on_device(src, kind, 0)) {
        c.srcMemoryType = 2;
        c.srcDevice = DP(src);
    } else {
        c.srcMemoryType = 1;
        c.srcHost = src;
    }
    if (on_device(dst, kind, 1)) {
        c.dstMemoryType = 2;
        c.dstDevice = DP(dst);
    } else {
        c.dstMemoryType = 1;
        c.dstHost = dst;
    }
    c.srcPitch = spitch;
    c.dstPitch = dpitch;
    c.WidthInBytes = w;
    c.Height = h;
    return async ? hw.Memcpy2DAsync(&c, s) : hw.Memcpy2DUnaligned(&c);
}

cudaError_t cudaMemcpy(void *dst, const void *src, size_t n, enum cudaMemcpyKind kind)
{
    ENTER();
    return ret(copy(dst, src, n, kind, 0, 0));
}

cudaError_t cudaMemcpyAsync(void *dst, const void *src, size_t n, enum cudaMemcpyKind kind, cudaStream_t s)
{
    ENTER();
    return ret(copy(dst, src, n, kind, 1, s));
}

cudaError_t cudaMemcpyWithAttributesAsync(void *dst, const void *src, size_t n, struct cudaMemcpyAttributes *a, cudaStream_t s)
{
    if (!a || a->srcAccessOrder < cudaMemcpySrcAccessOrderStream || a->srcAccessOrder > cudaMemcpySrcAccessOrderAny)
        return ret(cudaErrorInvalidValue);
    ENTER();
    return ret(copy(dst, src, n, cudaMemcpyDefault, 1, s));
}

cudaError_t cudaMemcpyPeer(void *dst, int ddev, const void *src, int sdev, size_t n)
{
    if (ddev != 0 || sdev != 0)
        return ret(cudaErrorInvalidDevice);
    ENTER();
    return ret(copy(dst, src, n, cudaMemcpyDeviceToDevice, 0, 0));
}

cudaError_t cudaMemcpyPeerAsync(void *dst, int ddev, const void *src, int sdev, size_t n, cudaStream_t s)
{
    if (ddev != 0 || sdev != 0)
        return ret(cudaErrorInvalidDevice);
    ENTER();
    return ret(copy(dst, src, n, cudaMemcpyDeviceToDevice, 1, s));
}

cudaError_t cudaMemcpy2D(void *dst, size_t dpitch, const void *src, size_t spitch, size_t w, size_t h, enum cudaMemcpyKind kind)
{
    ENTER();
    return ret(copy2d(dst, dpitch, src, spitch, w, h, kind, 0, 0));
}

cudaError_t cudaMemcpy2DAsync(void *dst, size_t dpitch, const void *src, size_t spitch, size_t w, size_t h,
                              enum cudaMemcpyKind kind, cudaStream_t s)
{
    ENTER();
    return ret(copy2d(dst, dpitch, src, spitch, w, h, kind, 1, s));
}

/* Between two pitched pointers: extent.width and pos.x are in bytes; a slice is pitch * ysize bytes. */
static cudaError_t copy3d(const struct cudaMemcpy3DParms *p, int async, cudaStream_t st)
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
    ENTER();
    for (z = 0; z < p->extent.depth; z++) {
        s = (const u8 *)p->srcPtr.ptr + ((p->srcPos.z + z) * p->srcPtr.ysize + p->srcPos.y) * p->srcPtr.pitch + p->srcPos.x;
        d = (u8 *)p->dstPtr.ptr + ((p->dstPos.z + z) * p->dstPtr.ysize + p->dstPos.y) * p->dstPtr.pitch + p->dstPos.x;
        e = copy2d(d, p->dstPtr.pitch, s, p->srcPtr.pitch, p->extent.width, p->extent.height, p->kind, async, st);
        if (e)
            return ret(e);
    }
    return cudaSuccess;
}

cudaError_t cudaMemcpy3D(const struct cudaMemcpy3DParms *p) { return copy3d(p, 0, 0); }
cudaError_t cudaMemcpy3DAsync(const struct cudaMemcpy3DParms *p, cudaStream_t s) { return copy3d(p, 1, s); }

/* sym is the host address of the object that stands for the device global (or a place inside it). */
static cudaError_t symcopy(const void *sym, void *host, size_t n, size_t off, enum cudaMemcpyKind kind, int to, int async,
                           cudaStream_t s)
{
    CUdeviceptr d;
    size_t left = 0;
    if ((unsigned)kind > (unsigned)cudaMemcpyDefault)
        return ret(cudaErrorInvalidMemcpyDirection);
    ENTER();
    d = hw_symbol(sym, &left);
    if (!d)
        return ret(cudaErrorInvalidSymbol);
    if (off > left || n > left - off)
        return ret(cudaErrorInvalidValue);
    if (!n)
        return cudaSuccess;
    if (!host)
        return ret(cudaErrorInvalidValue);
    d += off;
    if (kind == cudaMemcpyDeviceToDevice || (kind == cudaMemcpyDefault && memtype(host) == 2)) {
        if (to)
            return ret(async ? hw.MemcpyDtoDAsync(d, DP(host), n, s) : hw.MemcpyDtoD(d, DP(host), n));
        return ret(async ? hw.MemcpyDtoDAsync(DP(host), d, n, s) : hw.MemcpyDtoD(DP(host), d, n));
    }
    if (to)
        return ret(async ? hw.MemcpyHtoDAsync(d, host, n, s) : hw.MemcpyHtoD(d, host, n));
    return ret(async ? hw.MemcpyDtoHAsync(host, d, n, s) : hw.MemcpyDtoH(host, d, n));
}

cudaError_t cudaMemcpyToSymbol(const void *sym, const void *src, size_t n, size_t off, enum cudaMemcpyKind kind)
{
    return symcopy(sym, (void *)src, n, off, kind, 1, 0, 0);
}

cudaError_t cudaMemcpyToSymbolAsync(const void *sym, const void *src, size_t n, size_t off, enum cudaMemcpyKind kind, cudaStream_t s)
{
    return symcopy(sym, (void *)src, n, off, kind, 1, 1, s);
}

cudaError_t cudaMemcpyFromSymbol(void *dst, const void *sym, size_t n, size_t off, enum cudaMemcpyKind kind)
{
    return symcopy(sym, dst, n, off, kind, 0, 0, 0);
}

cudaError_t cudaMemcpyFromSymbolAsync(void *dst, const void *sym, size_t n, size_t off, enum cudaMemcpyKind kind, cudaStream_t s)
{
    return symcopy(sym, dst, n, off, kind, 0, 1, s);
}

/* what the templates of the device header (cudaMemcpyToSymbol<T> ...) call */
int __mcuda_to_symbol(const void *sym, const void *src, unsigned long n, unsigned long off, int kind)
{
    return (int)symcopy(sym, (void *)src, n, off, (enum cudaMemcpyKind)kind, 1, 0, 0);
}

int __mcuda_from_symbol(void *dst, const void *sym, unsigned long n, unsigned long off, int kind)
{
    return (int)symcopy(sym, dst, n, off, (enum cudaMemcpyKind)kind, 0, 0, 0);
}

cudaError_t cudaMemset(void *p, int value, size_t n)
{
    ENTER();
    if (!n)
        return cudaSuccess;
    return ret(hw.MemsetD8(DP(p), (unsigned char)value, n));
}

cudaError_t cudaMemsetAsync(void *p, int value, size_t n, cudaStream_t s)
{
    ENTER();
    if (!n)
        return cudaSuccess;
    return ret(hw.MemsetD8Async(DP(p), (unsigned char)value, n, s));
}

cudaError_t cudaMemset2D(void *p, size_t pitch, int value, size_t w, size_t h)
{
    ENTER();
    if (!w || !h)
        return cudaSuccess;
    return ret(hw.MemsetD2D8(DP(p), pitch, (unsigned char)value, w, h));
}

cudaError_t cudaMemset2DAsync(void *p, size_t pitch, int value, size_t w, size_t h, cudaStream_t s)
{
    ENTER();
    if (!w || !h)
        return cudaSuccess;
    return ret(hw.MemsetD2D8Async(DP(p), pitch, (unsigned char)value, w, h, s));
}

/* ---- streams ---- */

cudaError_t cudaStreamCreateWithPriority(cudaStream_t *ps, unsigned int flags, int priority)
{
    CUhandle s = 0;
    int e;
    if (!ps)
        return ret(cudaErrorInvalidValue);
    ENTER();
    e = hw.StreamCreateWithPriority(&s, flags, priority);
    *ps = (cudaStream_t)s;
    return ret(e);
}

cudaError_t cudaStreamCreateWithFlags(cudaStream_t *ps, unsigned int flags) { return cudaStreamCreateWithPriority(ps, flags, 0); }
cudaError_t cudaStreamCreate(cudaStream_t *ps) { return cudaStreamCreateWithPriority(ps, 0, 0); }

cudaError_t cudaStreamDestroy(cudaStream_t s)
{
    ENTER();
    return ret(hw.StreamDestroy(s));
}

cudaError_t cudaStreamGetPriority(cudaStream_t s, int *priority)
{
    ENTER();
    return ret(hw.StreamGetPriority(s, priority));
}

cudaError_t cudaStreamGetFlags(cudaStream_t s, unsigned int *flags)
{
    ENTER();
    return ret(hw.StreamGetFlags(s, flags));
}

cudaError_t cudaStreamSynchronize(cudaStream_t s)
{
    ENTER();
    return ret(hw.StreamSynchronize(s));
}

cudaError_t cudaStreamQuery(cudaStream_t s)
{
    int e;
    ENTER();
    e = hw.StreamQuery(s);
    return e == cudaErrorNotReady ? cudaErrorNotReady : ret(e); /* (not ready is not recorded as an error) */
}

cudaError_t cudaStreamWaitEvent(cudaStream_t s, cudaEvent_t e, unsigned int flags)
{
    ENTER();
    return ret(hw.StreamWaitEvent(s, e, flags));
}

/* ---- events ---- */

cudaError_t cudaEventCreateWithFlags(cudaEvent_t *pe, unsigned int flags)
{
    CUhandle e = 0;
    int r;
    if (!pe)
        return ret(cudaErrorInvalidValue);
    ENTER();
    r = hw.EventCreate(&e, flags);
    *pe = (cudaEvent_t)e;
    return ret(r);
}

cudaError_t cudaEventCreate(cudaEvent_t *pe) { return cudaEventCreateWithFlags(pe, 0); }

cudaError_t cudaEventDestroy(cudaEvent_t e)
{
    ENTER();
    return ret(hw.EventDestroy(e));
}

cudaError_t cudaEventRecord(cudaEvent_t e, cudaStream_t s)
{
    ENTER();
    return ret(hw.EventRecordWithFlags(e, s, 0));
}

cudaError_t cudaEventQuery(cudaEvent_t e)
{
    int r;
    ENTER();
    r = hw.EventQuery(e);
    return r == cudaErrorNotReady ? cudaErrorNotReady : ret(r);
}

cudaError_t cudaEventSynchronize(cudaEvent_t e)
{
    ENTER();
    return ret(hw.EventSynchronize(e));
}

cudaError_t cudaEventElapsedTime(float *ms, cudaEvent_t a, cudaEvent_t b)
{
    ENTER();
    return ret(hw.EventElapsedTime(ms, a, b));
}

/* The profiler's capture range (nsys --capture-range=cudaProfilerApi): the driver's cuProfilerStart / Stop when it has them. */
static cudaError_t profiler_call(const char *name)
{
    int (*f)(void);
    ENTER();
    f = (int (*)(void))hw_entry(name);
    return f ? ret(f()) : cudaSuccess;
}
cudaError_t cudaProfilerStart(void) { return profiler_call("cuProfilerStart"); }
cudaError_t cudaProfilerStop(void) { return profiler_call("cuProfilerStop"); }
