/* mcudahw: the same CUDA run-time API as mcuda, on a real GPU through NVIDIA's driver (libcuda.so.1), which is
   opened at run time: nothing of NVIDIA's is needed to compile or link. Internal declarations. */
#ifndef MCUDA_HW_H
#define MCUDA_HW_H
#include <stddef.h>

typedef int CUresult;
typedef int CUdevice;
typedef unsigned long long CUdeviceptr;
typedef void *CUhandle; /* a context, module, function, stream, event or link state */

struct HwMemcpy2D { /* CUDA_MEMCPY2D */
    size_t srcXInBytes, srcY;
    int srcMemoryType;
    const void *srcHost;
    CUdeviceptr srcDevice;
    void *srcArray;
    size_t srcPitch;
    size_t dstXInBytes, dstY;
    int dstMemoryType;
    void *dstHost;
    CUdeviceptr dstDevice;
    void *dstArray;
    size_t dstPitch;
    size_t WidthInBytes, Height;
};

/* The driver's functions that are used: member, the driver's name, parameters. */
#define HW_DRIVER(X) \
    X(Init, "cuInit", (unsigned)) \
    X(DriverGetVersion, "cuDriverGetVersion", (int *)) \
    X(DeviceGetCount, "cuDeviceGetCount", (int *)) \
    X(DeviceGet, "cuDeviceGet", (CUdevice *, int)) \
    X(DeviceGetName, "cuDeviceGetName", (char *, int, CUdevice)) \
    X(DeviceGetUuid, "cuDeviceGetUuid_v2", (void *, CUdevice)) \
    X(DeviceTotalMem, "cuDeviceTotalMem_v2", (size_t *, CUdevice)) \
    X(DeviceGetAttribute, "cuDeviceGetAttribute", (int *, int, CUdevice)) \
    X(PrimaryCtxRetain, "cuDevicePrimaryCtxRetain", (CUhandle *, CUdevice)) \
    X(PrimaryCtxSetFlags, "cuDevicePrimaryCtxSetFlags_v2", (CUdevice, unsigned)) \
    X(PrimaryCtxGetState, "cuDevicePrimaryCtxGetState", (CUdevice, unsigned *, int *)) \
    X(CtxSetCurrent, "cuCtxSetCurrent", (CUhandle)) \
    X(CtxSynchronize, "cuCtxSynchronize", (void)) \
    X(CtxGetStreamPriorityRange, "cuCtxGetStreamPriorityRange", (int *, int *)) \
    X(ModuleLoadData, "cuModuleLoadData", (CUhandle *, const void *)) \
    X(ModuleUnload, "cuModuleUnload", (CUhandle)) \
    X(ModuleLoadDataEx, "cuModuleLoadDataEx", (CUhandle *, const void *, unsigned, int *, void **)) \
    X(ModuleGetFunction, "cuModuleGetFunction", (CUhandle *, CUhandle, const char *)) \
    X(ModuleGetGlobal, "cuModuleGetGlobal_v2", (CUdeviceptr *, size_t *, CUhandle, const char *)) \
    X(LinkCreate, "cuLinkCreate_v2", (unsigned, int *, void **, CUhandle *)) \
    X(LinkAddData, "cuLinkAddData_v2", (CUhandle, int, void *, size_t, const char *, unsigned, int *, void **)) \
    X(LinkComplete, "cuLinkComplete", (CUhandle, void **, size_t *)) \
    X(LinkDestroy, "cuLinkDestroy", (CUhandle)) \
    X(LaunchKernel, "cuLaunchKernel", (CUhandle, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, CUhandle, void **, void **)) \
    X(FuncSetAttribute, "cuFuncSetAttribute", (CUhandle, int, int)) \
    X(FuncGetAttribute, "cuFuncGetAttribute", (int *, int, CUhandle)) \
    X(Occupancy, "cuOccupancyMaxActiveBlocksPerMultiprocessorWithFlags", (int *, CUhandle, int, size_t, unsigned)) \
    X(MemAlloc, "cuMemAlloc_v2", (CUdeviceptr *, size_t)) \
    X(MemFree, "cuMemFree_v2", (CUdeviceptr)) \
    X(MemHostAlloc, "cuMemHostAlloc", (void **, size_t, unsigned)) \
    X(MemFreeHost, "cuMemFreeHost", (void *)) \
    X(MemAllocManaged, "cuMemAllocManaged", (CUdeviceptr *, size_t, unsigned)) \
    X(MemGetInfo, "cuMemGetInfo_v2", (size_t *, size_t *)) \
    X(MemHostRegister, "cuMemHostRegister_v2", (void *, size_t, unsigned)) \
    X(MemHostUnregister, "cuMemHostUnregister", (void *)) \
    X(MemHostGetDevicePointer, "cuMemHostGetDevicePointer_v2", (CUdeviceptr *, void *, unsigned)) \
    X(MemHostGetFlags, "cuMemHostGetFlags", (unsigned *, void *)) \
    X(PointerGetAttribute, "cuPointerGetAttribute", (void *, int, CUdeviceptr)) \
    X(MemcpyHtoD, "cuMemcpyHtoD_v2", (CUdeviceptr, const void *, size_t)) \
    X(MemcpyDtoH, "cuMemcpyDtoH_v2", (void *, CUdeviceptr, size_t)) \
    X(MemcpyDtoD, "cuMemcpyDtoD_v2", (CUdeviceptr, CUdeviceptr, size_t)) \
    X(MemcpyHtoDAsync, "cuMemcpyHtoDAsync_v2", (CUdeviceptr, const void *, size_t, CUhandle)) \
    X(MemcpyDtoHAsync, "cuMemcpyDtoHAsync_v2", (void *, CUdeviceptr, size_t, CUhandle)) \
    X(MemcpyDtoDAsync, "cuMemcpyDtoDAsync_v2", (CUdeviceptr, CUdeviceptr, size_t, CUhandle)) \
    X(Memcpy2DUnaligned, "cuMemcpy2DUnaligned_v2", (const struct HwMemcpy2D *)) \
    X(Memcpy2DAsync, "cuMemcpy2DAsync_v2", (const struct HwMemcpy2D *, CUhandle)) \
    X(MemsetD8, "cuMemsetD8_v2", (CUdeviceptr, unsigned char, size_t)) \
    X(MemsetD8Async, "cuMemsetD8Async", (CUdeviceptr, unsigned char, size_t, CUhandle)) \
    X(MemsetD2D8, "cuMemsetD2D8_v2", (CUdeviceptr, size_t, unsigned char, size_t, size_t)) \
    X(MemsetD2D8Async, "cuMemsetD2D8Async", (CUdeviceptr, size_t, unsigned char, size_t, size_t, CUhandle)) \
    X(StreamCreateWithPriority, "cuStreamCreateWithPriority", (CUhandle *, unsigned, int)) \
    X(StreamGetPriority, "cuStreamGetPriority", (CUhandle, int *)) \
    X(StreamGetFlags, "cuStreamGetFlags", (CUhandle, unsigned *)) \
    X(StreamDestroy, "cuStreamDestroy_v2", (CUhandle)) \
    X(StreamSynchronize, "cuStreamSynchronize", (CUhandle)) \
    X(StreamQuery, "cuStreamQuery", (CUhandle)) \
    X(StreamWaitEvent, "cuStreamWaitEvent", (CUhandle, CUhandle, unsigned)) \
    X(EventCreate, "cuEventCreate", (CUhandle *, unsigned)) \
    X(EventDestroy, "cuEventDestroy_v2", (CUhandle)) \
    X(EventRecordWithFlags, "cuEventRecordWithFlags", (CUhandle, CUhandle, unsigned)) \
    X(EventQuery, "cuEventQuery", (CUhandle)) \
    X(EventSynchronize, "cuEventSynchronize", (CUhandle)) \
    X(EventElapsedTime, "cuEventElapsedTime_v2", (float *, CUhandle, CUhandle))

#define HW_MEMBER(m, name, args) CUresult (*m) args;
struct HwDriver {
    HW_DRIVER(HW_MEMBER)
    void *lib; /* the open library, for cudaGetDriverEntryPointByVersion */
};
extern struct HwDriver hw;
extern CUdevice hw_dev;
extern CUhandle hw_ctx;

/* Before anything that needs the device: opens the driver and the device's primary context once, and makes
   that context the calling thread's. Returns a cudaError_t number (0, 100 no device, 3 no driver ...). */
int hw_enter(void);
/* A function of the driver by name, 0 if there is none (the driver must be open). */
void *hw_entry(const char *name);

/* The lock of the module tables (mod.c). */
void hw_lock(void);
void hw_unlock(void);

/* The last error of the calling thread. */
void hw_err_set(int e);
int hw_err_get(int clear);

/* mod.c */
struct __mcuda_module;
/* The device address of the device global whose host object is at p (or inside it): 0 if p is no such
   object. Loads the modules that use it. *left: the bytes from p to the end of the object. */
CUdeviceptr hw_symbol(const void *p, size_t *left);
/* The function of the kernel whose host stub is at p, 0 if unknown. */
CUhandle hw_kernel_of(const void *p);
void hw_precompile(int n); /* MCUDA_PRECOMPILE: mod.c */

/* lib.c: the device library, pieces of PTX by the name of the function each defines; made by mklib.py */
struct HwLibPiece {
    const char *name; /* __mcuda_... */
    const char *text; /* PTX: the function, without a header */
    const char *deps; /* names it calls, separated by spaces */
};
extern const struct HwLibPiece hw_lib[];
extern const int hw_nlib;

#endif
