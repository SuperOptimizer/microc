/* Test of the mcuda run-time library (cuda/rt.c, driver.c, cublaslt.c, algo.c) from the host side.
   Every line of output is deterministic; rt_test.exp holds the output of a build with -DRT_TEST_ENGINE,
   which adds kernel launches through the PTX engine (the last section). */
#include <cuda_runtime.h>
#include <cuda.h>
#include <cublasLt.h>
#include <mcuda.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned long long ull;

/* The 16-bit conversions of the library (cuda/fpconv.c). */
unsigned mc_f32_to_f16(unsigned f32bits);
unsigned mc_f16_to_f32(unsigned f16bits);
unsigned mc_f32_to_bf16(unsigned f32bits);
unsigned mc_bf16_to_f32(unsigned bf16bits);

static int failures;

/* cudaMemGetInfo's free figure is this library's own bookkeeping where the host has no call for its free
   memory (then it moves by exactly what is allocated), and capped by the host's free memory on Linux,
   where it can only be checked loosely. */
#ifdef __linux__
#define EXACT_FREE 0
#else
#define EXACT_FREE 1
#endif
#define SLACK ((size_t)32 << 20)

static void check(const char *what, int ok)
{
    printf("%s", what);
    printf(": %s\n", ok ? "ok" : "FAIL");
    if (!ok)
        failures++;
}

static unsigned long seed = 12345;

static unsigned rnd(void)
{
    seed = (seed * 6364136223846793005ul + 1442695040888963407ul) & 0xfffffffffffffffful;
    return (unsigned)(seed >> 33);
}

/* ---- errors ---- */

static void test_errors(void)
{
    char buf[8];
    printf("-- errors\n");
    printf("%s", cudaGetErrorName(cudaSuccess));
    printf(" / %s\n", cudaGetErrorString(cudaSuccess));
    printf("%s", cudaGetErrorName(cudaErrorInvalidValue));
    printf(" / %s\n", cudaGetErrorString(cudaErrorInvalidValue));
    printf("%s", cudaGetErrorName(cudaErrorMemoryAllocation));
    printf(" / %s\n", cudaGetErrorString(cudaErrorMemoryAllocation));
    printf("%s", cudaGetErrorName(cudaErrorNotReady));
    printf(" / %s\n", cudaGetErrorString(cudaErrorNotReady));
    printf("%s", cudaGetErrorName(cudaErrorLaunchFailure));
    printf(" / %s\n", cudaGetErrorString(cudaErrorLaunchFailure));
    printf("%s", cudaGetErrorName(cudaErrorUnknown));
    printf(" / %s\n", cudaGetErrorString(cudaErrorUnknown));
    printf("%s\n", cudaGetErrorString((cudaError_t)12345));
    printf("values %d", (int)cudaErrorInvalidConfiguration);
    printf(" %d", (int)cudaErrorInvalidDeviceFunction);
    printf(" %d", (int)cudaErrorInvalidPtx);
    printf(" %d", (int)cudaErrorInvalidResourceHandle);
    printf(" %d", (int)cudaErrorNotReady);
    printf(" %d", (int)cudaErrorLaunchFailure);
    printf(" %d\n", (int)cudaErrorUnknown);
    printf("clean %d\n", (int)cudaGetLastError());
    printf("bad memcpy %d\n", (int)cudaMemcpy(buf, buf, 4, (enum cudaMemcpyKind)9));
    printf("peek %d\n", (int)cudaPeekAtLastError());
    printf("good call %d\n", (int)cudaDeviceSynchronize());
    printf("peek %d\n", (int)cudaPeekAtLastError()); /* a success does not clear it */
    printf("bad free %d\n", (int)cudaFree(buf));
    printf("get %d\n", (int)cudaGetLastError());     /* the last one wins */
    printf("get %d\n", (int)cudaGetLastError());
}

/* ---- the device ---- */

static void test_device(void)
{
    static struct cudaDeviceProp p;
    int n = -1, d = -1, v = 0, lo = 9, hi = 9, can = 9;
    unsigned fl = 9;
    printf("-- device\n");
    printf("count %d", (int)cudaGetDeviceCount(&n));
    printf(" %d\n", n);
    printf("get %d", (int)cudaGetDevice(&d));
    printf(" %d\n", d);
    printf("set 0: %d", (int)cudaSetDevice(0));
    printf(", set 1: %d\n", (int)cudaSetDevice(1));
    printf("last %s\n", cudaGetErrorName(cudaGetLastError()));
    printf("props %d\n", (int)cudaGetDeviceProperties(&p, 0));
    printf("name %s", p.name);
    printf(", sm_%d", p.major);
    printf("%d", p.minor);
    printf(", %d", p.multiProcessorCount);
    printf(" SMs, warp %d\n", p.warpSize);
    printf("threads %d", p.maxThreadsPerBlock);
    printf(" (%d", p.maxThreadsDim[0]);
    printf(" %d", p.maxThreadsDim[1]);
    printf(" %d", p.maxThreadsDim[2]);
    printf("), grid %d", p.maxGridSize[0]);
    printf(" %d", p.maxGridSize[1]);
    printf(" %d\n", p.maxGridSize[2]);
    printf("shared %lu", (unsigned long)p.sharedMemPerBlock);
    printf(" %lu", (unsigned long)p.sharedMemPerBlockOptin);
    printf(" %lu", (unsigned long)p.sharedMemPerMultiprocessor);
    printf(", uva %d\n", p.unifiedAddressing);
    check("total memory", p.totalGlobalMem >= ((size_t)1 << 30) && p.totalGlobalMem <= ((size_t)24 << 30));
    printf("props of device 1: %d\n", (int)cudaGetDeviceProperties(&p, 1));
    printf("attr SMs %d", (int)cudaDeviceGetAttribute(&v, cudaDevAttrMultiProcessorCount, 0));
    printf(" %d\n", v);
    printf("attr smem/SM %d", (int)cudaDeviceGetAttribute(&v, cudaDevAttrMaxSharedMemoryPerMultiprocessor, 0));
    printf(" %d\n", v);
    printf("attr major %d", (int)cudaDeviceGetAttribute(&v, cudaDevAttrComputeCapabilityMajor, 0));
    printf(" %d\n", v);
    printf("attr bad %d\n", (int)cudaDeviceGetAttribute(&v, (enum cudaDeviceAttr)9999, 0));
    printf("priorities %d", (int)cudaDeviceGetStreamPriorityRange(&lo, &hi));
    printf(" %d", lo);
    printf(" %d\n", hi);
    printf("peer %d", (int)cudaDeviceCanAccessPeer(&can, 0, 0));
    printf(" %d\n", can);
    printf("enable peer %d\n", (int)cudaDeviceEnablePeerAccess(0, 0));
    printf("flags %d", (int)cudaSetDeviceFlags(cudaDeviceScheduleBlockingSync));
    printf(" %d", (int)cudaGetDeviceFlags(&fl));
    printf(" %u\n", fl);
    printf("versions %d", (int)cudaDriverGetVersion(&v));
    printf(" %d", v);
    printf(" %d", (int)cudaRuntimeGetVersion(&v));
    printf(" %d\n", v);
    printf("occupancy %d", (int)cudaOccupancyMaxActiveBlocksPerMultiprocessor(&v, (const void *)test_device, 256, 0));
    printf(" %d", v);
    printf(" %d", (int)cudaOccupancyMaxActiveBlocksPerMultiprocessor(&v, (const void *)test_device, 32, 0));
    printf(" %d", v);
    printf(" %d", (int)cudaOccupancyMaxActiveBlocksPerMultiprocessor(&v, (const void *)test_device, 256, 40000));
    printf(" %d\n", v);
    printf("func attr %d\n", (int)cudaFuncSetAttribute((const void *)test_device, cudaFuncAttributeMaxDynamicSharedMemorySize, 96 * 1024));
    cudaGetLastError();
}

/* ---- memory ---- */

static void test_memory(void)
{
    unsigned char host[1000], back[1000], *d1 = 0, *d2 = 0, *h1 = 0, *h2 = 0, *mg = 0;
    void *p = (void *)1, *dp = 0;
    struct cudaPointerAttributes a;
    struct cudaMemcpy3DParms P;
    struct cudaMemcpyAttributes ma;
    size_t fr0 = 0, fr1 = 0, fr2 = 0, tot = 0, tot1 = 0;
    unsigned fl = 0;
    int i, ok, x, y, z;
    printf("-- memory\n");
    for (i = 0; i < 1000; i++)
        host[i] = (unsigned char)(i * 7 + 3);
    printf("malloc %d", (int)cudaMalloc((void **)&d1, 1000));
    printf(" %d\n", (int)cudaMalloc((void **)&d2, 1000));
    check("device pointers", d1 && d2 && d1 != d2 && ((unsigned long)d1 & 255) == 0 && ((unsigned long)d2 & 255) == 0);
    printf("malloc 0: %d", (int)cudaMalloc(&p, 0));
    printf(" %d\n", p == 0);
    printf("h2d %d", (int)cudaMemcpy(d1, host, 1000, cudaMemcpyHostToDevice));
    printf(", d2d %d", (int)cudaMemcpy(d2, d1, 1000, cudaMemcpyDeviceToDevice));
    printf(", d2h %d\n", (int)cudaMemcpy(back, d2, 1000, cudaMemcpyDeviceToHost));
    check("round trip", memcmp(host, back, 1000) == 0);
    printf("memset %d\n", (int)cudaMemset(d1 + 10, 0x1ab, 20));
    ok = d1[9] == host[9] && d1[30] == host[30];
    for (i = 10; i < 30; i++)
        ok = ok && d1[i] == 0xab;
    check("memset bytes", ok);
    printf("async %d", (int)cudaMemsetAsync(d2, 0, 1000, 0));
    printf(" %d", (int)cudaMemcpyAsync(d2 + 1, host, 999, cudaMemcpyDefault, 0));
    printf(" %d\n", (int)cudaMemcpyAsync(back, d2, 1000, cudaMemcpyHostToHost, cudaStreamLegacy));
    check("async bytes", back[0] == 0 && memcmp(back + 1, host, 999) == 0);
    ma.srcAccessOrder = cudaMemcpySrcAccessOrderStream;
    ma.srcLocHint.type = cudaMemLocationTypeInvalid;
    ma.srcLocHint.id = 0;
    ma.dstLocHint = ma.srcLocHint;
    ma.flags = cudaMemcpyFlagPreferOverlapWithCompute;
    printf("with attributes %d", (int)cudaMemcpyWithAttributesAsync(d2, host, 1000, &ma, 0));
    ma.srcAccessOrder = cudaMemcpySrcAccessOrderInvalid;
    printf(" %d\n", (int)cudaMemcpyWithAttributesAsync(d2, host, 1000, &ma, 0));
    check("attribute copy", memcmp(d2, host, 1000) == 0);
    printf("peer %d", (int)cudaMemcpyPeer(d1, 0, d2, 0, 1000));
    printf(" %d", (int)cudaMemcpyPeerAsync(d1, 0, d2, 0, 1000, 0));
    printf(" %d\n", (int)cudaMemcpyPeer(d1, 1, d2, 0, 1000));
    check("peer copy", memcmp(d1, host, 1000) == 0);
    printf("zero count with null %d\n", (int)cudaMemcpy(0, 0, 0, cudaMemcpyHostToDevice));

    /* 2D: a 10 x 6 window from a pitch of 25 into a pitch of 16 */
    memset(d2, 0xee, 1000);
    printf("2d %d\n", (int)cudaMemcpy2DAsync(d2, 16, host + 3, 25, 10, 6, cudaMemcpyHostToDevice, 0));
    ok = 1;
    for (y = 0; y < 6; y++)
        for (x = 0; x < 16; x++)
            ok = ok && d2[y * 16 + x] == (x < 10 ? host[3 + y * 25 + x] : 0xee);
    check("2d bytes", ok);
    printf("2d bad pitch %d\n", (int)cudaMemcpy2D(d2, 8, host, 25, 10, 6, cudaMemcpyHostToDevice));
    printf("memset2d %d\n", (int)cudaMemset2DAsync(d2, 16, 0x11, 4, 3, 0));
    ok = 1;
    for (y = 0; y < 4; y++)
        for (x = 0; x < 16; x++)
            ok = ok && d2[y * 16 + x] == (x < 4 && y < 3 ? 0x11 : x < 10 ? host[3 + y * 25 + x] : 0xee);
    check("memset2d bytes", ok);

    /* 3D, as ufsm does it: a 4 x 3 x 2 box at (x 2, y 1, z 3) of a 10 x 8 x 6 volume into a dense box */
    memset(&P, 0, sizeof P);
    P.srcPtr = make_cudaPitchedPtr(host, 10, 10, 8);
    P.srcPos = make_cudaPos(2, 1, 3);
    P.dstPtr = make_cudaPitchedPtr(d2, 4, 4, 3);
    P.extent = make_cudaExtent(4, 3, 2);
    P.kind = cudaMemcpyHostToDevice;
    printf("3d %d\n", (int)cudaMemcpy3DAsync(&P, 0));
    ok = 1;
    for (z = 0; z < 2; z++)
        for (y = 0; y < 3; y++)
            for (x = 0; x < 4; x++)
                ok = ok && d2[(z * 3 + y) * 4 + x] == host[((z + 3) * 8 + y + 1) * 10 + x + 2];
    check("3d bytes", ok);
    P.srcArray = (cudaArray_t)host;
    printf("3d array %d\n", (int)cudaMemcpy3D(&P));

    /* host memory */
    printf("host alloc %d", (int)cudaHostAlloc((void **)&h1, 5000, cudaHostAllocPortable));
    printf(" %d\n", (int)cudaMallocHost((void **)&h2, 100));
    check("host pointers", h1 && h2 && ((unsigned long)h1 & 4095) == 0);
    memset(h1, 1, 5000);
    printf("device pointer %d", (int)cudaHostGetDevicePointer(&dp, h1, 0));
    printf(" %d\n", dp == (void *)h1);
    printf("device pointer of pageable memory %d\n", (int)cudaHostGetDevicePointer(&dp, host, 0));
    printf("host flags %d", (int)cudaHostGetFlags(&fl, h1));
    printf(" %u\n", fl);
    printf("register %d", (int)cudaHostRegister(host, sizeof host, cudaHostRegisterDefault));
    printf(" %d", (int)cudaHostRegister(host + 10, 10, cudaHostRegisterDefault));
    printf(" %d\n", (int)cudaHostGetDevicePointer(&dp, host + 500, 0));
    printf("unregister %d", (int)cudaHostUnregister(host));
    printf(" %d\n", (int)cudaHostUnregister(host));
    printf("managed %d\n", (int)cudaMallocManaged((void **)&mg, 3000, cudaMemAttachGlobal));
    mg[2999] = 7;

    /* what is where */
    printf("attributes:");
    printf(" %d", (int)cudaPointerGetAttributes(&a, d1 + 999));
    printf(" %d", (int)a.type);
    printf(" %d", a.device);
    printf(" %d", a.devicePointer == (void *)(d1 + 999) && a.hostPointer == 0);
    printf(", %d", (int)cudaPointerGetAttributes(&a, d1 + 1000));
    printf(" %d", (int)a.type); /* one past the end */
    printf(", %d", (int)cudaPointerGetAttributes(&a, h1 + 4999));
    printf(" %d", (int)a.type);
    printf(" %d", a.devicePointer == (void *)(h1 + 4999) && a.hostPointer == (void *)(h1 + 4999));
    printf(", %d", (int)cudaPointerGetAttributes(&a, mg));
    printf(" %d", (int)a.type);
    printf(", %d", (int)cudaPointerGetAttributes(&a, host));
    printf(" %d", (int)a.type);
    printf(" %d\n", a.devicePointer == 0 && a.hostPointer == (void *)host);

    /* frees: only the start of a live allocation of the right kind */
    printf("free inside %d", (int)cudaFree(d1 + 256));
    printf(", free host as device %d", (int)cudaFree(h1));
    printf(", free device as host %d\n", (int)cudaFreeHost(d1));
    printf("free %d", (int)cudaFree(d1));
    printf(" %d", (int)cudaFree(d2));
    printf(" %d", (int)cudaFree(mg));
    printf(", again %d", (int)cudaFree(d1));
    printf(", null %d\n", (int)cudaFree(0));
    printf("free host %d", (int)cudaFreeHost(h1));
    printf(" %d", (int)cudaFreeHost(h2));
    printf(", again %d", (int)cudaFreeHost(h1));
    printf(", null %d\n", (int)cudaFreeHost(0));
    printf("after free %d", (int)cudaPointerGetAttributes(&a, d1));
    printf(" %d\n", (int)a.type);

    /* memory accounting */
    printf("meminfo %d\n", (int)cudaMemGetInfo(&fr0, &tot));
    check("meminfo totals", tot >= ((size_t)1 << 30) && tot <= ((size_t)24 << 30) && fr0 <= tot && fr0 > ((size_t)64 << 20));
    printf("malloc 64 MB %d\n", (int)cudaMalloc((void **)&d1, (size_t)64 << 20));
    cudaMemGetInfo(&fr1, &tot1);
    check("free went down", tot1 == tot && (EXACT_FREE ? fr0 - fr1 == ((size_t)64 << 20) : fr1 <= fr0 + SLACK));
    cudaFree(d1);
    cudaMemGetInfo(&fr2, &tot1);
    check("free came back", EXACT_FREE ? fr2 == fr0 : fr2 + SLACK >= fr1);
    printf("malloc 1 TB %d", (int)cudaMalloc(&p, (size_t)1 << 40));
    printf(" %d\n", p == 0);
    printf("last %s\n", cudaGetErrorName(cudaGetLastError()));
}

/* ---- streams and events ---- */

static void test_streams(void)
{
    cudaStream_t s1 = 0, s2 = 0, s3 = 0;
    cudaEvent_t e0 = 0, e1 = 0, e2 = 0, e3 = 0;
    volatile double sink = 0;
    float ms = -1;
    unsigned fl = 9;
    int pr = 9, i;
    printf("-- streams and events\n");
    printf("create %d", (int)cudaStreamCreate(&s1));
    printf(" %d", (int)cudaStreamCreateWithFlags(&s2, cudaStreamNonBlocking));
    printf(" %d\n", (int)cudaStreamCreateWithPriority(&s3, cudaStreamNonBlocking, -100));
    check("stream handles", s1 && s2 && s3 && s1 != s2 && s2 != s3);
    printf("flags %d", (int)cudaStreamGetFlags(s1, &fl));
    printf(" %u", fl);
    printf(" %d", (int)cudaStreamGetFlags(s2, &fl));
    printf(" %u,", fl);
    printf(" priority %d", (int)cudaStreamGetPriority(s2, &pr));
    printf(" %d", pr);
    printf(" %d", (int)cudaStreamGetPriority(s3, &pr));
    printf(" %d\n", pr);
    printf("bad flags %d\n", (int)cudaStreamCreateWithFlags(&s1 + 0, 64));
    cudaStreamCreate(&s1);
    printf("sync %d", (int)cudaStreamSynchronize(s1));
    printf(" %d", (int)cudaStreamSynchronize(0));
    printf(" %d", (int)cudaStreamSynchronize(cudaStreamPerThread));
    printf(", query %d", (int)cudaStreamQuery(s2));
    printf(" %d\n", (int)cudaStreamQuery(0));
    printf("events %d", (int)cudaEventCreate(&e0));
    printf(" %d", (int)cudaEventCreate(&e1));
    printf(" %d", (int)cudaEventCreateWithFlags(&e2, cudaEventDisableTiming));
    printf(" %d\n", (int)cudaEventCreateWithFlags(&e3, cudaEventBlockingSync | cudaEventDisableTiming));
    printf("elapsed before record %d\n", (int)cudaEventElapsedTime(&ms, e0, e1));
    printf("query unrecorded %d\n", (int)cudaEventQuery(e0));
    printf("record %d", (int)cudaEventRecord(e0, s1));
    for (i = 0; i < 2000000; i++)
        sink += (double)i * 0.5;
    printf(" %d", (int)cudaEventRecord(e1, s1));
    printf(" %d\n", (int)cudaEventRecord(e2, 0));
    printf("wait %d", (int)cudaStreamWaitEvent(s2, e1, 0));
    printf(" %d", (int)cudaStreamWaitEvent(0, e2, 0));
    printf(", sync %d", (int)cudaEventSynchronize(e1));
    printf(", query %d\n", (int)cudaEventQuery(e1));
    printf("elapsed %d", (int)cudaEventElapsedTime(&ms, e0, e1));
    printf(" >=0: %d", ms >= 0);
    printf(", <10s: %d\n", ms < 10000);
    printf("elapsed self %d", (int)cudaEventElapsedTime(&ms, e0, e0));
    printf(" %d\n", ms == 0);
    printf("elapsed without timing %d\n", (int)cudaEventElapsedTime(&ms, e0, e2));
    printf("copy on stream %d", (int)cudaMemcpyAsync(&pr, &i, sizeof pr, cudaMemcpyHostToHost, s2));
    printf(", on a non-stream %d\n", (int)cudaMemcpyAsync(&pr, &i, sizeof pr, cudaMemcpyHostToHost, (cudaStream_t)e0));
    printf("destroy %d", (int)cudaStreamDestroy(s1));
    printf(" %d", (int)cudaStreamDestroy(s2));
    printf(" %d", (int)cudaStreamDestroy(s3));
    printf(", again %d", (int)cudaStreamDestroy(s1));
    printf(", default %d\n", (int)cudaStreamDestroy(0));
    printf("use destroyed %d", (int)cudaStreamSynchronize(s2));
    printf(" %d\n", (int)cudaEventRecord(e0, s2));
    printf("destroy events %d", (int)cudaEventDestroy(e0));
    printf(" %d", (int)cudaEventDestroy(e1));
    printf(" %d", (int)cudaEventDestroy(e2));
    printf(" %d", (int)cudaEventDestroy(e3));
    printf(", again %d\n", (int)cudaEventDestroy(e0));
    printf("last %s\n", cudaGetErrorName(cudaGetLastError()));
}

/* ---- the driver API ---- */

typedef struct {
    CUresult (*gran)(size_t *, const CUmemAllocationProp *, CUmemAllocationGranularity_flags);
    CUresult (*reserve)(CUdeviceptr *, size_t, size_t, CUdeviceptr, unsigned long long);
    CUresult (*afree)(CUdeviceptr, size_t);
    CUresult (*create)(CUmemGenericAllocationHandle *, size_t, const CUmemAllocationProp *, unsigned long long);
    CUresult (*release)(CUmemGenericAllocationHandle);
    CUresult (*map)(CUdeviceptr, size_t, size_t, CUmemGenericAllocationHandle, unsigned long long);
    CUresult (*unmap)(CUdeviceptr, size_t);
    CUresult (*access)(CUdeviceptr, size_t, const CUmemAccessDesc *, size_t);
} Vm;

/* The sequence of ufsm's nn_ring_alloc: one virtual range of 5 slabs backed by a ring of 2 physical chunks. */
static void test_vm(void)
{
    Vm vm;
    cudaDriverEntryPointQueryResult q = (cudaDriverEntryPointQueryResult)9;
    CUmemAllocationProp pr;
    CUmemAccessDesc ad;
    CUmemGenericAllocationHandle h[2];
    CUdeviceptr va = 0;
    struct cudaPointerAttributes a;
    unsigned char *p;
    size_t G = 0, fr0 = 0, fr1 = 0, fr2 = 0, tot = 0;
    void *fn = 0;
    int s, ok;
    printf("-- virtual memory through driver entry points\n");
    printf("driver call before the run time hands one out: %d\n", (int)cuCtxSynchronize());
    printf("no such entry %d", (int)cudaGetDriverEntryPointByVersion("cuNoSuchThing", &fn, 12000, cudaEnableDefault, &q));
    printf(" %d", (int)q);
    printf(" %d\n", fn == 0);
    cudaGetLastError();
    ok = cudaGetDriverEntryPointByVersion("cuMemGetAllocationGranularity", (void **)&vm.gran, 12000, cudaEnableDefault, &q) == cudaSuccess && q == cudaDriverEntryPointSuccess;
    ok = ok && cudaGetDriverEntryPointByVersion("cuMemAddressReserve", (void **)&vm.reserve, 12000, cudaEnableDefault, &q) == cudaSuccess && q == cudaDriverEntryPointSuccess;
    ok = ok && cudaGetDriverEntryPointByVersion("cuMemAddressFree", (void **)&vm.afree, 12000, cudaEnableDefault, &q) == cudaSuccess && q == cudaDriverEntryPointSuccess;
    ok = ok && cudaGetDriverEntryPointByVersion("cuMemCreate", (void **)&vm.create, 12000, cudaEnableDefault, &q) == cudaSuccess && q == cudaDriverEntryPointSuccess;
    ok = ok && cudaGetDriverEntryPointByVersion("cuMemRelease", (void **)&vm.release, 12000, cudaEnableDefault, &q) == cudaSuccess && q == cudaDriverEntryPointSuccess;
    ok = ok && cudaGetDriverEntryPointByVersion("cuMemMap", (void **)&vm.map, 12000, cudaEnableDefault, &q) == cudaSuccess && q == cudaDriverEntryPointSuccess;
    ok = ok && cudaGetDriverEntryPointByVersion("cuMemUnmap", (void **)&vm.unmap, 12000, cudaEnableDefault, &q) == cudaSuccess && q == cudaDriverEntryPointSuccess;
    ok = ok && cudaGetDriverEntryPointByVersion("cuMemSetAccess", (void **)&vm.access, 12000, cudaEnableDefault, &q) == cudaSuccess && q == cudaDriverEntryPointSuccess;
    check("entry points", ok);
    if (!ok)
        return;
    memset(&pr, 0, sizeof pr);
    pr.type = CU_MEM_ALLOCATION_TYPE_PINNED;
    pr.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    pr.location.id = 0;
    printf("granularity %d", (int)vm.gran(&G, &pr, CU_MEM_ALLOC_GRANULARITY_MINIMUM));
    printf(" %lu\n", (unsigned long)G);
    cudaMemGetInfo(&fr0, &tot);
    printf("reserve %d", (int)vm.reserve(&va, 5 * G, G, 0, 0));
    printf(" %d\n", va != 0 && va % G == 0);
    printf("reserve off the granularity %d\n", (int)vm.reserve(&va + 0, G + 4096, G, 0, 0) != CUDA_SUCCESS);
    printf("create %d", (int)vm.create(&h[0], G, &pr, 0));
    printf(" %d\n", (int)vm.create(&h[1], G, &pr, 0));
    cudaMemGetInfo(&fr1, &tot);
    check("physical memory is charged", EXACT_FREE ? fr0 - fr1 == 2 * G : fr1 <= fr0 + SLACK);
    memset(&ad, 0, sizeof ad);
    ad.location.type = CU_MEM_LOCATION_TYPE_DEVICE;
    ad.location.id = 0;
    ad.flags = CU_MEM_ACCESS_FLAGS_PROT_READWRITE;
    printf("access before map %d\n", (int)vm.access(va, 5 * G, &ad, 1) != CUDA_SUCCESS);
    printf("map with an offset %d", (int)vm.map(va, G, 4096, h[0], 0));
    printf(", with the wrong size %d\n", (int)vm.map(va, 2 * G, 0, h[0], 0));
    printf("map");
    for (s = 0; s < 5; s++)
        printf(" %d", (int)vm.map(va + (CUdeviceptr)s * G, G, 0, h[s % 2], 0));
    printf(", twice %d\n", (int)vm.map(va, G, 0, h[1], 0));
    printf("access %d\n", (int)vm.access(va, 5 * G, &ad, 1));
    p = (unsigned char *)va;
    /* slabs 0, 2 and 4 are one chunk, slabs 1 and 3 the other */
    memset(p, 0x5a, G);
    memset(p + G, 0xa5, G);
    ok = p[2 * G] == 0x5a && p[3 * G - 1] == 0x5a && p[3 * G] == 0xa5 && p[4 * G + 12345] == 0x5a;
    p[4 * G + 7] = 1;
    p[3 * G + 9] = 2;
    ok = ok && p[7] == 1 && p[2 * G + 7] == 1 && p[G + 9] == 2 && p[G + 7] == 0xa5;
    check("the ring aliases", ok);
    printf("device memory to the run time: %d", (int)cudaPointerGetAttributes(&a, p + 3 * G + 5));
    printf(" %d\n", (int)a.type);
    printf("memset through cuda %d", (int)cudaMemset(p + G, 0, G));
    printf(" %d\n", p[3 * G + 9]);
    printf("cudaFree of a ring %d\n", (int)cudaFree(p));
    cudaGetLastError();
    printf("unmap half a mapping %d\n", (int)vm.unmap(va, G / 2) != CUDA_SUCCESS);
    printf("unmap %d\n", (int)vm.unmap(va, 5 * G));
    printf("release %d", (int)vm.release(h[0]));
    printf(" %d", (int)vm.release(h[1]));
    printf(", again %d\n", (int)vm.release(h[0]));
    printf("address free %d\n", (int)vm.afree(va, 5 * G));
    cudaMemGetInfo(&fr2, &tot);
    check("physical memory is given back", EXACT_FREE ? fr2 == fr0 : fr2 + SLACK >= fr1);
    printf("after: %d", (int)cudaPointerGetAttributes(&a, p));
    printf(" %d\n", (int)a.type);

    /* released while mapped: the mapping keeps the bytes until it goes */
    vm.reserve(&va, 2 * G, G, 0, 0);
    vm.create(&h[0], G, &pr, 0);
    vm.map(va, G, 0, h[0], 0);
    vm.map(va + G, G, 0, h[0], 0);
    vm.access(va, 2 * G, &ad, 1);
    printf("release while mapped %d", (int)vm.release(h[0]));
    p = (unsigned char *)va;
    p[100] = 77;
    printf(" %d", p[G + 100]);
    cudaMemGetInfo(&fr1, &tot);
    printf(" %d", EXACT_FREE ? fr0 - fr1 == G : fr1 <= fr0 + SLACK);
    printf(", free the range %d", (int)vm.afree(va, 2 * G));
    cudaMemGetInfo(&fr2, &tot);
    printf(" %d\n", EXACT_FREE ? fr2 == fr0 : fr2 + SLACK >= fr1);
}

static void test_driver(void)
{
    CUdevice dev = 9;
    CUcontext ctx = 0, cur = 0;
    CUdeviceptr d = 0, d2 = 0;
    char name[64];
    const char *str = 0;
    unsigned char buf[64], back[64];
    size_t total = 0, fr = 0;
    int n = 0, v = 0, i;
    printf("-- driver API\n");
    printf("init %d", (int)cuInit(0));
    printf(", count %d", (int)cuDeviceGetCount(&n));
    printf(" %d", n);
    printf(", get %d", (int)cuDeviceGet(&dev, 0));
    printf(" %d", dev);
    printf(", get 1: %d\n", (int)cuDeviceGet(&dev, 1));
    printf("name %d", (int)cuDeviceGetName(name, sizeof name, 0));
    printf(" %s\n", name);
    printf("short name %d", (int)cuDeviceGetName(name, 7, 0));
    printf(" %s\n", name);
    printf("total mem %d", (int)cuDeviceTotalMem(&total, 0));
    printf(" %d\n", total >= ((size_t)1 << 30));
    printf("attributes %d", (int)cuDeviceGetAttribute(&v, CU_DEVICE_ATTRIBUTE_MULTIPROCESSOR_COUNT, 0));
    printf(" %d", v);
    printf(" %d", (int)cuDeviceGetAttribute(&v, CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR, 0));
    printf(" %d", v);
    printf(" %d", (int)cuDeviceGetAttribute(&v, CU_DEVICE_ATTRIBUTE_MAX_THREADS_PER_BLOCK, 0));
    printf(" %d\n", v);
    printf("error string %d", (int)cuGetErrorString(CUDA_ERROR_INVALID_VALUE, &str));
    printf(" %s", str);
    printf(", name %d", (int)cuGetErrorName(CUDA_ERROR_LAUNCH_FAILED, &str));
    printf(" %s\n", str);
    printf("context %d", (int)cuCtxCreate(&ctx, 0, 0));
    printf(" %d", (int)cuCtxGetCurrent(&cur));
    printf(" %d", cur == ctx && ctx != 0);
    printf(" sync %d\n", (int)cuCtxSynchronize());
    for (i = 0; i < 64; i++)
        buf[i] = (unsigned char)(200 - i);
    printf("alloc %d", (int)cuMemAlloc(&d, 64));
    printf(" %d", (int)cuMemAlloc(&d2, 64));
    printf(", zero bytes %d\n", (int)cuMemAlloc(&d2 + 0, 0));
    cuMemAlloc(&d2, 64);
    printf("copies %d", (int)cuMemcpyHtoD(d, buf, 64));
    printf(" %d", (int)cuMemcpyDtoD(d2, d, 64));
    printf(" %d\n", (int)cuMemcpyDtoH(back, d2, 64));
    check("driver copies", memcmp(buf, back, 64) == 0);
    printf("memset %d", (int)cuMemsetD8(d, 9, 64));
    printf(" %d\n", (int)cuMemsetD32(d2, 0x01020304u, 16));
    check("driver memsets", ((unsigned char *)d)[63] == 9 && ((unsigned *)d2)[15] == 0x01020304u);
    printf("meminfo %d", (int)cuMemGetInfo(&fr, &total));
    printf(" %d\n", fr <= total && fr > 0);
    printf("free %d", (int)cuMemFree(d));
    printf(" %d", (int)cuMemFree(d2));
    printf(", again %d\n", (int)cuMemFree(d));
    printf("destroy %d", (int)cuCtxDestroy(ctx));
    printf(", then alloc %d", (int)cuMemAlloc(&d, 64));
    printf(", set primary %d", (int)(cuDevicePrimaryCtxRetain(&ctx, 0) || cuCtxSetCurrent(ctx)));
    printf(", then alloc %d\n", (int)cuMemAlloc(&d, 64));
    cuMemFree(d);
    printf("run-time error untouched %d\n", (int)cudaGetLastError());
}

/* ---- cublasLt ---- */

typedef union {
    float f;
    unsigned u;
} Bits;

static size_t esize(cudaDataType_t t) { return t == CUDA_R_32F ? 4 : 2; }

static float get(const void *p, cudaDataType_t t, size_t i)
{
    Bits b;
    if (t == CUDA_R_32F)
        return ((const float *)p)[i];
    b.u = t == CUDA_R_16F ? mc_f16_to_f32(((const unsigned short *)p)[i]) : mc_bf16_to_f32(((const unsigned short *)p)[i]);
    return b.f;
}

static void put(void *p, cudaDataType_t t, size_t i, float v)
{
    Bits b;
    b.f = v;
    if (t == CUDA_R_32F)
        ((float *)p)[i] = v;
    else
        ((unsigned short *)p)[i] = (unsigned short)(t == CUDA_R_16F ? mc_f32_to_f16(b.u) : mc_f32_to_bf16(b.u));
}

/* small integers when `ints` (every product and sum is then exact in every type), else multiples of 1/64 in (-2, 2) */
static float value(int ints) { return ints ? (float)((int)(rnd() % 7) - 3) : (float)((int)(rnd() % 255) - 127) / 64.0f; }

static const char *tname(cudaDataType_t t) { return t == CUDA_R_32F ? "f32" : t == CUDA_R_16F ? "f16" : "bf16"; }

typedef struct {
    const char *name;
    cublasComputeType_t compute;
    int ta, tb;
    cudaDataType_t at, bt, dt;
    int m, n, k;
    int pad;         /* extra leading dimension */
    int batch, gap;  /* gap: extra elements between the matrices of a batch */
    float alpha, beta;
    int cneqd;       /* C is its own array */
    cublasLtEpilogue_t epi;
    cudaDataType_t biast;
    int ints;
} Case;

static cublasLtHandle_t lt;

/* Runs one product and compares every element of D with a reference in double: equal when the case is
   exact, else within the rounding of the output type; elements outside the matrices must stay untouched. */
static void matmul_case(const Case *c)
{
    int ar = c->ta ? c->k : c->m, ac = c->ta ? c->m : c->k, br = c->tb ? c->n : c->k, bc = c->tb ? c->k : c->n;
    int lda = ar + c->pad, ldb = br + c->pad, ldd = c->m + c->pad;
    long long sa = (long long)lda * ac + c->gap, sb = (long long)ldb * bc + c->gap, sd = (long long)ldd * c->n + c->gap;
    size_t na = (size_t)sa * c->batch, nb = (size_t)sb * c->batch, nd = (size_t)sd * c->batch, i;
    void *A = malloc(na * 4), *B = malloc(nb * 4), *C = malloc(nd * 4), *D = malloc(nd * 4), *bias = malloc((size_t)c->m * 4);
    float *ref_in = (float *)malloc(nd * sizeof(float)), *d_in = (float *)malloc(nd * sizeof(float));
    cublasLtMatmulDesc_t op = 0;
    cublasLtMatrixLayout_t la = 0, lb = 0, ld = 0;
    cublasLtMatmulPreference_t pref = 0;
    cublasLtMatmulHeuristicResult_t h;
    cublasOperation_t oa = c->ta ? CUBLAS_OP_T : CUBLAS_OP_N, ob = c->tb ? CUBLAS_OP_T : CUBLAS_OP_N;
    size_t ws = 1 << 20;
    cublasStatus_t st;
    int nh = 0, b, r, q, l, bad = 0, touched = 0, hasbias = (c->epi & CUBLASLT_EPILOGUE_BIAS) != 0;
    double worst = 0;

    for (i = 0; i < na; i++)
        put(A, c->at, i, value(c->ints));
    for (i = 0; i < nb; i++)
        put(B, c->bt, i, value(c->ints));
    for (i = 0; i < nd; i++) {
        put(C, c->dt, i, value(c->ints));
        put(D, c->dt, i, value(c->ints));
        ref_in[i] = get(c->cneqd ? C : D, c->dt, i); /* what C holds before the call */
        d_in[i] = get(D, c->dt, i);
    }
    for (i = 0; i < (size_t)c->m; i++)
        put(bias, c->biast, i, value(c->ints));

    st = cublasLtMatmulDescCreate(&op, c->compute, CUDA_R_32F);
    if (!st) st = cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_TRANSA, &oa, sizeof oa);
    if (!st) st = cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_TRANSB, &ob, sizeof ob);
    if (!st && c->epi != CUBLASLT_EPILOGUE_DEFAULT) {
        st = cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_EPILOGUE, &c->epi, sizeof c->epi);
        if (!st && hasbias) st = cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_BIAS_DATA_TYPE, &c->biast, sizeof c->biast);
        if (!st && hasbias) st = cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_BIAS_POINTER, &bias, sizeof bias);
    }
    if (!st) st = cublasLtMatrixLayoutCreate(&la, c->at, (unsigned long long)ar, (unsigned long long)ac, lda);
    if (!st) st = cublasLtMatrixLayoutCreate(&lb, c->bt, (unsigned long long)br, (unsigned long long)bc, ldb);
    if (!st) st = cublasLtMatrixLayoutCreate(&ld, c->dt, (unsigned long long)c->m, (unsigned long long)c->n, ldd);
    if (!st && c->batch > 1) {
        cublasLtMatrixLayoutSetAttribute(la, CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT, &c->batch, sizeof c->batch);
        cublasLtMatrixLayoutSetAttribute(lb, CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT, &c->batch, sizeof c->batch);
        cublasLtMatrixLayoutSetAttribute(ld, CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT, &c->batch, sizeof c->batch);
        cublasLtMatrixLayoutSetAttribute(la, CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET, &sa, sizeof sa);
        cublasLtMatrixLayoutSetAttribute(lb, CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET, &sb, sizeof sb);
        st = cublasLtMatrixLayoutSetAttribute(ld, CUBLASLT_MATRIX_LAYOUT_STRIDED_BATCH_OFFSET, &sd, sizeof sd);
    }
    if (!st) st = cublasLtMatmulPreferenceCreate(&pref);
    if (!st) st = cublasLtMatmulPreferenceSetAttribute(pref, CUBLASLT_MATMUL_PREF_MAX_WORKSPACE_BYTES, &ws, sizeof ws);
    if (!st) st = cublasLtMatmulAlgoGetHeuristic(lt, op, la, lb, ld, ld, pref, 1, &h, &nh);
    if (!st && nh != 1) st = CUBLAS_STATUS_INTERNAL_ERROR;
    if (!st) st = cublasLtMatmul(lt, op, &c->alpha, A, la, B, lb, &c->beta, c->cneqd ? C : D, ld, D, ld, &h.algo, 0, 0, 0);
    if (st) {
        printf("matmul %s", c->name);
        printf(": status %d FAIL\n", (int)st);
        failures++;
        return;
    }
    for (b = 0; b < c->batch; b++) {
        size_t oa_ = (size_t)sa * b, ob_ = (size_t)sb * b, od = (size_t)sd * b;
        for (q = 0; q < c->n; q++)
            for (r = 0; r < c->m; r++) {
                double sum = 0, want, got, tol;
                for (l = 0; l < c->k; l++) {
                    double x = c->ta ? get(A, c->at, oa_ + (size_t)r * lda + l) : get(A, c->at, oa_ + (size_t)l * lda + r);
                    double y = c->tb ? get(B, c->bt, ob_ + (size_t)l * ldb + q) : get(B, c->bt, ob_ + (size_t)q * ldb + l);
                    sum += x * y;
                }
                want = (double)c->alpha * sum + (c->beta != 0 ? (double)c->beta * ref_in[od + (size_t)q * ldd + r] : 0.0);
                if (hasbias)
                    want += get(bias, c->biast, (size_t)r);
                if ((c->epi & CUBLASLT_EPILOGUE_RELU) && want < 0)
                    want = 0;
                got = get(D, c->dt, od + (size_t)q * ldd + r);
                /* half a unit in the last place of the output type, and the single-precision sums */
                tol = c->ints ? 0 : (want < 0 ? -want : want) * (c->dt == CUDA_R_32F ? 1e-6 : c->dt == CUDA_R_16F ? 1.0 / 2048 : 1.0 / 256) * 1.01 + 2e-5;
                if (got - want > tol || want - got > tol)
                    bad++;
                if (got - want > worst)
                    worst = got - want;
                if (want - got > worst)
                    worst = want - got;
            }
    }
    /* padding rows, batch gaps, and C when it is not D */
    for (i = 0; i < nd; i++) {
        size_t in = i % (size_t)sd;
        int inside = in < (size_t)ldd * c->n && (int)(in % (size_t)ldd) < c->m;
        if (!inside && get(D, c->dt, i) != d_in[i])
            touched++;
        if (c->cneqd && get(C, c->dt, i) != ref_in[i])
            touched++;
    }
    printf("matmul %s", c->name);
    printf(": %s", tname(c->at));
    printf("%s", c->ta ? "^T" : "");
    printf(" x %s", tname(c->bt));
    printf("%s", c->tb ? "^T" : "");
    printf(" -> %s", tname(c->dt));
    printf(", %d", c->m);
    printf("x%d", c->n);
    printf("x%d", c->k);
    if (c->batch > 1)
        printf(", batch %d", c->batch);
    printf(": %s", bad || touched ? "FAIL" : c->ints ? "PASS (exact)" : "PASS (within rounding)");
    if (bad || touched) {
        printf(" (%d", bad);
        printf(" wrong, worst %g", worst);
        printf(", %d outside)", touched);
        failures++;
    }
    printf("\n");
    cublasLtMatmulPreferenceDestroy(pref);
    cublasLtMatrixLayoutDestroy(la);
    cublasLtMatrixLayoutDestroy(lb);
    cublasLtMatrixLayoutDestroy(ld);
    cublasLtMatmulDescDestroy(op);
    free(A);
    free(B);
    free(C);
    free(D);
    free(bias);
    free(ref_in);
    free(d_in);
}

static const Case cases[] = {
    /* name, compute, ta, tb, A, B, D, m, n, k, pad, batch, gap, alpha, beta, C != D, epilogue, bias type, exact */
    {"f32 NN", CUBLAS_COMPUTE_32F, 0, 0, CUDA_R_32F, CUDA_R_32F, CUDA_R_32F, 5, 7, 6, 0, 1, 0, 1.0f, 0.0f, 0, CUBLASLT_EPILOGUE_DEFAULT, CUDA_R_32F, 1},
    {"f32 NN random", CUBLAS_COMPUTE_32F, 0, 0, CUDA_R_32F, CUDA_R_32F, CUDA_R_32F, 9, 4, 33, 2, 1, 0, 1.0f, 0.0f, 0, CUBLASLT_EPILOGUE_DEFAULT, CUDA_R_32F, 0},
    {"f32 TN beta", CUBLAS_COMPUTE_32F, 1, 0, CUDA_R_32F, CUDA_R_32F, CUDA_R_32F, 6, 5, 7, 3, 1, 0, 1.0f, 0.5f, 0, CUBLASLT_EPILOGUE_DEFAULT, CUDA_R_32F, 0},
    {"f32 NT alpha beta C", CUBLAS_COMPUTE_32F, 0, 1, CUDA_R_32F, CUDA_R_32F, CUDA_R_32F, 4, 8, 5, 1, 1, 0, -1.5f, 2.0f, 1, CUBLASLT_EPILOGUE_DEFAULT, CUDA_R_32F, 1},
    {"f32 TT tf32", CUBLAS_COMPUTE_32F_FAST_TF32, 1, 1, CUDA_R_32F, CUDA_R_32F, CUDA_R_32F, 7, 3, 9, 0, 1, 0, 1.0f, -1.0f, 0, CUBLASLT_EPILOGUE_DEFAULT, CUDA_R_32F, 0},
    {"f32 bias", CUBLAS_COMPUTE_32F, 0, 0, CUDA_R_32F, CUDA_R_32F, CUDA_R_32F, 6, 6, 6, 0, 1, 0, 1.0f, 0.0f, 0, CUBLASLT_EPILOGUE_BIAS, CUDA_R_32F, 1},
    {"f32 relu bias", CUBLAS_COMPUTE_32F, 0, 1, CUDA_R_32F, CUDA_R_32F, CUDA_R_32F, 8, 5, 4, 1, 1, 0, 1.0f, 1.0f, 1, CUBLASLT_EPILOGUE_RELU_BIAS, CUDA_R_32F, 1},
    {"f32 batched", CUBLAS_COMPUTE_32F, 1, 1, CUDA_R_32F, CUDA_R_32F, CUDA_R_32F, 3, 4, 5, 1, 4, 3, 1.0f, 1.0f, 0, CUBLASLT_EPILOGUE_DEFAULT, CUDA_R_32F, 1},
    /* nn_vwt_fast: bf16 col x bf16 gy^T accumulated into fp32 */
    {"bf16 NT into f32", CUBLAS_COMPUTE_32F, 0, 1, CUDA_R_16BF, CUDA_R_16BF, CUDA_R_32F, 12, 5, 9, 4, 1, 0, 1.0f, 1.0f, 0, CUBLASLT_EPILOGUE_DEFAULT, CUDA_R_32F, 1},
    {"bf16 NT into f32 random", CUBLAS_COMPUTE_32F, 0, 1, CUDA_R_16BF, CUDA_R_16BF, CUDA_R_32F, 12, 5, 9, 4, 1, 0, 1.0f, 1.0f, 0, CUBLASLT_EPILOGUE_DEFAULT, CUDA_R_32F, 0},
    /* nn_dinovol: f16 W^T x f16 A with a bias, into f16 or f32 */
    {"f16 TN bias f16", CUBLAS_COMPUTE_32F, 1, 0, CUDA_R_16F, CUDA_R_16F, CUDA_R_16F, 6, 10, 8, 2, 1, 0, 1.0f, 0.0f, 0, CUBLASLT_EPILOGUE_BIAS, CUDA_R_16F, 1},
    {"f16 TN bias f16 random", CUBLAS_COMPUTE_32F, 1, 0, CUDA_R_16F, CUDA_R_16F, CUDA_R_16F, 6, 10, 8, 2, 1, 0, 1.0f, 0.0f, 0, CUBLASLT_EPILOGUE_BIAS, CUDA_R_16F, 0},
    {"f16 TN bias f32 beta", CUBLAS_COMPUTE_32F, 1, 0, CUDA_R_16F, CUDA_R_16F, CUDA_R_32F, 6, 10, 8, 0, 1, 0, 1.0f, 1.0f, 0, CUBLASLT_EPILOGUE_BIAS, CUDA_R_32F, 0},
    /* nn_attn_h16: bf16 operands, bf16 or f32 result, C given or in place */
    {"bf16 TT bias bf16 C", CUBLAS_COMPUTE_32F, 1, 1, CUDA_R_16BF, CUDA_R_16BF, CUDA_R_16BF, 5, 6, 4, 0, 1, 0, 1.0f, 1.0f, 1, CUBLASLT_EPILOGUE_BIAS, CUDA_R_16BF, 1},
    {"bf16 NN bf16 random", CUBLAS_COMPUTE_32F, 0, 0, CUDA_R_16BF, CUDA_R_16BF, CUDA_R_16BF, 5, 6, 16, 0, 1, 0, 1.0f, 1.0f, 0, CUBLASLT_EPILOGUE_DEFAULT, CUDA_R_16BF, 0},
    {"bf16 TN bias f32", CUBLAS_COMPUTE_32F, 1, 0, CUDA_R_16BF, CUDA_R_16BF, CUDA_R_32F, 7, 3, 5, 0, 1, 0, 1.0f, 0.0f, 0, CUBLASLT_EPILOGUE_BIAS, CUDA_R_32F, 1},
    /* nn_villawind: batched f16, strided, into f32 or f16 */
    {"f16 batched NT f32", CUBLAS_COMPUTE_32F, 0, 1, CUDA_R_16F, CUDA_R_16F, CUDA_R_32F, 4, 5, 6, 2, 3, 5, 1.0f, 0.0f, 0, CUBLASLT_EPILOGUE_DEFAULT, CUDA_R_32F, 1},
    {"f16 batched TN f16", CUBLAS_COMPUTE_32F, 1, 0, CUDA_R_16F, CUDA_R_16F, CUDA_R_16F, 5, 4, 7, 1, 5, 0, 1.0f, 0.0f, 0, CUBLASLT_EPILOGUE_DEFAULT, CUDA_R_16F, 0},
    {"mixed f16 x bf16", CUBLAS_COMPUTE_32F_FAST_16BF, 0, 0, CUDA_R_16F, CUDA_R_16BF, CUDA_R_32F, 3, 3, 3, 0, 1, 0, 2.0f, 0.0f, 0, CUBLASLT_EPILOGUE_DEFAULT, CUDA_R_32F, 1}
};

static void test_cublaslt(void)
{
    cublasLtMatmulDesc_t op = 0;
    cublasLtMatrixLayout_t a = 0, b = 0, d = 0, d64 = 0;
    cublasLtMatmulPreference_t pref = 0;
    cublasLtMatmulHeuristicResult_t h;
    cublasLtMatmulDescOpaque_t own;
    cublasLtEpilogue_t gelu = CUBLASLT_EPILOGUE_GELU, epi = (cublasLtEpilogue_t)0;
    cublasOperation_t t = CUBLAS_OP_T;
    float one = 1, x[4], y[4];
    long long ld = 0;
    size_t wr = 0;
    int n = 9, bc = 0;
    unsigned i;
    printf("-- cublasLt\n");
    printf("create %d\n", (int)cublasLtCreate(&lt));
    for (i = 0; i < sizeof cases / sizeof cases[0]; i++)
        matmul_case(&cases[i]);

    printf("layout %d", (int)cublasLtMatrixLayoutCreate(&a, CUDA_R_32F, 2, 2, 2));
    printf(" %d", (int)cublasLtMatrixLayoutCreate(&b, CUDA_R_32F, 2, 2, 2));
    printf(" %d", (int)cublasLtMatrixLayoutCreate(&d, CUDA_R_32F, 2, 2, 2));
    printf(" %d\n", (int)cublasLtMatrixLayoutCreate(&d64, CUDA_R_64F, 2, 2, 2));
    printf("layout attributes %d", (int)cublasLtMatrixLayoutGetAttribute(a, CUBLASLT_MATRIX_LAYOUT_LD, &ld, sizeof ld, &wr));
    printf(" %d", (int)ld);
    printf(" %d", (int)wr);
    printf(" %d", (int)cublasLtMatrixLayoutGetAttribute(a, CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT, &bc, sizeof bc, 0));
    printf(" %d", bc);
    printf(", wrong size %d\n", (int)cublasLtMatrixLayoutSetAttribute(a, CUBLASLT_MATRIX_LAYOUT_BATCH_COUNT, &ld, sizeof ld));
    printf("desc %d", (int)cublasLtMatmulDescCreate(&op, CUBLAS_COMPUTE_32F, CUDA_R_32F));
    printf(", pref %d\n", (int)cublasLtMatmulPreferenceCreate(&pref));
    printf("desc epilogue %d", (int)cublasLtMatmulDescGetAttribute(op, CUBLASLT_MATMUL_DESC_EPILOGUE, &epi, sizeof epi, 0));
    printf(" %d\n", (int)epi);
    printf("heuristic %d", (int)cublasLtMatmulAlgoGetHeuristic(lt, op, a, b, d, d, pref, 4, &h, &n));
    printf(" %d", n);
    printf(" %d\n", (int)h.state);
    printf("f64 layout: %d", (int)cublasLtMatmulAlgoGetHeuristic(lt, op, a, b, d64, d64, pref, 1, &h, &n));
    printf(" %d\n", n);
    printf("f64 matmul: %d\n", (int)cublasLtMatmul(lt, op, &one, x, a, x, b, &one, y, d64, y, d64, 0, 0, 0, 0));
    cublasLtMatmulDescSetAttribute(op, CUBLASLT_MATMUL_DESC_EPILOGUE, &gelu, sizeof gelu);
    printf("gelu: %d", (int)cublasLtMatmulAlgoGetHeuristic(lt, op, a, b, d, d, pref, 1, &h, &n));
    printf(" %d\n", n);
    cublasLtMatmulDescDestroy(op);
    printf("f64 compute: %d", (int)cublasLtMatmulDescCreate(&op, CUBLAS_COMPUTE_64F, CUDA_R_64F));
    printf(" %d\n", (int)cublasLtMatmulAlgoGetHeuristic(lt, op, a, b, d, d, pref, 1, &h, &n));
    cublasLtMatmulDescDestroy(op);
    printf("unknown attribute: %d\n", (int)cublasLtMatmulDescSetAttribute((cublasLtMatmulDesc_t)&own, CUBLASLT_MATMUL_DESC_A_SCALE_POINTER, &one, sizeof(void *)));
    /* a descriptor in the caller's storage, a shape that does not fit, an algorithm pointer of 0 */
    printf("init %d", (int)cublasLtMatmulDescInit(&own, CUBLAS_COMPUTE_32F, CUDA_R_32F));
    cublasLtMatrixLayoutDestroy(b);
    cublasLtMatrixLayoutCreate(&b, CUDA_R_32F, 3, 2, 3);
    printf(", 2x2 times 3x2: %d", (int)cublasLtMatmulAlgoGetHeuristic(lt, &own, a, b, d, d, pref, 1, &h, &n));
    cublasLtMatmulDescSetAttribute(&own, CUBLASLT_MATMUL_DESC_TRANSB, &t, sizeof t);
    cublasLtMatrixLayoutDestroy(d);
    cublasLtMatrixLayoutCreate(&d, CUDA_R_32F, 2, 3, 2);
    printf(", 2x2 times (3x2)^T: %d", (int)cublasLtMatmulAlgoGetHeuristic(lt, &own, a, b, d, d, pref, 1, &h, &n));
    {
        float A2[4], B2[6], D2[6];
        A2[0] = 1; A2[1] = 2; A2[2] = 3; A2[3] = 4;              /* [1 3; 2 4] */
        B2[0] = 1; B2[1] = 0; B2[2] = -1; B2[3] = 2; B2[4] = 1; B2[5] = 0; /* [1 2; 0 1; -1 0], transposed: [1 0 -1; 2 1 0] */
        x[0] = 0;
        printf(", %d:", (int)cublasLtMatmul(lt, &own, &one, A2, a, B2, b, x, 0, d, D2, d, 0, 0, 0, 0));
        for (i = 0; i < 6; i++)
            printf(" %g", (double)D2[i]);
        printf("\n");
    }
    cublasLtMatmulPreferenceDestroy(pref);
    cublasLtMatrixLayoutDestroy(a);
    cublasLtMatrixLayoutDestroy(b);
    cublasLtMatrixLayoutDestroy(d);
    cublasLtMatrixLayoutDestroy(d64);
    printf("destroy %d", (int)cublasLtDestroy(lt));
    printf(", again %d\n", (int)cublasLtMatmulAlgoGetHeuristic(0, 0, 0, 0, 0, 0, 0, 1, &h, &n));
}

/* ---- prefix sums and sorts ---- */

#define NS 5000

/* keys[i] (masked) in order, equal keys in their original order, every original element present once */
static int sorted_ok(const ull *okeys, const ull *keys, const ull *vals, int n, ull mask, int desc)
{
    static unsigned char seen[NS];
    int i;
    memset(seen, 0, sizeof seen);
    for (i = 0; i < n; i++) {
        if (vals[i] >= (ull)n || seen[vals[i]] || okeys[vals[i]] != keys[i])
            return 0;
        seen[vals[i]] = 1;
        if (i) {
            ull a = keys[i - 1] & mask, b = keys[i] & mask;
            if (desc ? a < b : a > b)
                return 0;
            if (a == b && vals[i - 1] > vals[i])
                return 0;
        }
    }
    return 1;
}

static void test_algo(void)
{
    static unsigned in32[NS], out32[NS], k32[NS], v32[NS];
    static ull in64[NS], out64[NS], k64[NS], v64[NS], ok64[NS], kk[NS], vv[NS], ko[NS], vo[NS];
    unsigned s32;
    ull s64;
    int i, ok, w;
    printf("-- scans and sorts\n");
    for (i = 0; i < NS; i++) {
        in32[i] = rnd();
        in64[i] = ((ull)rnd() << 32) | rnd();
    }
    __mcuda_exclusive_sum_u32(in32, out32, NS);
    for (ok = 1, s32 = 0, i = 0; i < NS; i++) {
        ok = ok && out32[i] == s32;
        s32 += in32[i];
    }
    check("exclusive sum u32", ok);
    __mcuda_inclusive_sum_u32(in32, out32, NS);
    for (ok = 1, s32 = 0, i = 0; i < NS; i++) {
        s32 += in32[i];
        ok = ok && out32[i] == s32;
    }
    check("inclusive sum u32", ok);
    memcpy(out32, in32, sizeof in32);
    __mcuda_exclusive_sum_u32(out32, out32, NS);
    for (ok = 1, s32 = 0, i = 0; i < NS; i++) {
        ok = ok && out32[i] == s32;
        s32 += in32[i];
    }
    check("exclusive sum u32 in place", ok);
    __mcuda_exclusive_scan_sum_u32(in32, out32, 3, 100);
    check("exclusive scan with a start", out32[0] == 100 && out32[1] == 100 + in32[0] && out32[2] == 100 + in32[0] + in32[1]);
    __mcuda_exclusive_sum_u64(in64, out64, NS);
    for (ok = 1, s64 = 0, i = 0; i < NS; i++) {
        ok = ok && out64[i] == s64;
        s64 += in64[i];
    }
    check("exclusive sum u64", ok);
    memcpy(out64, in64, sizeof in64);
    __mcuda_inclusive_sum_u64(out64, out64, NS);
    for (ok = 1, s64 = 0, i = 0; i < NS; i++) {
        s64 += in64[i];
        ok = ok && out64[i] == s64;
    }
    check("inclusive sum u64 in place", ok);
    __mcuda_exclusive_sum_u32(in32, out32, 0);
    __mcuda_sort_keys_u32(k32, 0);
    check("empty inputs", 1);

    /* keys with many duplicates in every digit; w: the key bits in use */
    for (w = 0; w < 4; w++) {
        static const int bits[4] = {5, 13, 32, 64};
        static const char *const names[4] = {"5-bit keys", "13-bit keys", "32-bit keys", "64-bit keys"};
        ull mask = bits[w] == 64 ? ~(ull)0 : (((ull)1 << bits[w]) - 1);
        for (i = 0; i < NS; i++)
            ok64[i] = (((ull)rnd() << 32) | rnd()) & mask;
        printf("%s:", names[w]);
        if (bits[w] <= 32) {
            for (i = 0; i < NS; i++) {
                k32[i] = (unsigned)ok64[i];
                v32[i] = (unsigned)i;
            }
            __mcuda_sort_pairs_u32_u32(k32, v32, NS);
            for (i = 0; i < NS; i++) {
                kk[i] = k32[i];
                vv[i] = v32[i];
            }
            printf(" u32/u32 %s", sorted_ok(ok64, kk, vv, NS, ~(ull)0, 0) ? "ok" : "FAIL");
            for (i = 0; i < NS; i++) {
                k32[i] = (unsigned)ok64[i];
                v64[i] = (ull)i;
            }
            __mcuda_sort_pairs_u32_u64(k32, v64, NS);
            for (i = 0; i < NS; i++)
                kk[i] = k32[i];
            printf(" u32/u64 %s", sorted_ok(ok64, kk, v64, NS, ~(ull)0, 0) ? "ok" : "FAIL");
            for (i = 0; i < NS; i++)
                k32[i] = (unsigned)ok64[i];
            __mcuda_sort_keys_u32(k32, NS);
            for (ok = 1, i = 0; i < NS; i++)
                ok = ok && k32[i] == (unsigned)kk[i];
            printf(" keys u32 %s", ok ? "ok" : "FAIL");
        }
        for (i = 0; i < NS; i++) {
            k64[i] = ok64[i];
            v32[i] = (unsigned)i;
        }
        __mcuda_sort_pairs_u64_u32(k64, v32, NS);
        for (i = 0; i < NS; i++)
            vv[i] = v32[i];
        printf(" u64/u32 %s", sorted_ok(ok64, k64, vv, NS, ~(ull)0, 0) ? "ok" : "FAIL");
        for (i = 0; i < NS; i++) {
            k64[i] = ok64[i];
            v64[i] = (ull)i;
        }
        __mcuda_sort_pairs_u64_u64(k64, v64, NS);
        printf(" u64/u64 %s", sorted_ok(ok64, k64, v64, NS, ~(ull)0, 0) ? "ok" : "FAIL");
        memcpy(kk, ok64, sizeof kk);
        __mcuda_sort_keys_u64(kk, NS);
        printf(" keys u64 %s", memcmp(kk, k64, sizeof kk) == 0 ? "ok" : "FAIL");
        for (i = 0; i < NS; i++) {
            k64[i] = ok64[i];
            v64[i] = (ull)i;
        }
        printf(" descending %d", __mcuda_sort_pairs(k64, 8, v64, 8, NS, 1));
        printf(" %s\n", sorted_ok(ok64, k64, v64, NS, ~(ull)0, 1) ? "ok" : "FAIL");
    }

    /* as cub's SortPairs(keys_in, keys_out, values_in, values_out, n, begin_bit, end_bit): out of place, a bit range */
    for (i = 0; i < NS; i++) {
        ok64[i] = ((ull)rnd() << 32) | rnd();
        k64[i] = ok64[i];
        v64[i] = (ull)i;
    }
    printf("bits [0, 37) out of place %d", __mcuda_sort_pairs_bits(k64, ko, 8, v64, vo, 8, NS, 0, 37, 0));
    for (ok = 1, i = 0; i < NS; i++)
        ok = ok && k64[i] == ok64[i] && v64[i] == (ull)i;
    printf(" %s", sorted_ok(ok64, ko, vo, NS, ((ull)1 << 37) - 1, 0) ? "ok" : "FAIL");
    printf(", inputs %s\n", ok ? "intact" : "CHANGED");
    printf("bits [8, 16) one pass %d", __mcuda_sort_pairs_bits(k64, ko, 8, v64, vo, 8, NS, 8, 16, 0));
    printf(" %s\n", sorted_ok(ok64, ko, vo, NS, 0xff00, 0) ? "ok" : "FAIL");
    printf("bits [3, 30) descending in place %d", __mcuda_sort_pairs_bits(k64, k64, 8, v64, v64, 8, NS, 3, 30, 1));
    printf(" %s\n", sorted_ok(ok64, k64, v64, NS, (((ull)1 << 30) - 1) & ~(ull)7, 1) ? "ok" : "FAIL");
    for (i = 0; i < NS; i++) {
        k64[i] = ok64[i];
        v64[i] = (ull)i;
    }
    printf("keys in place, values out of place %d", __mcuda_sort_pairs_bits(k64, k64, 8, v64, vo, 8, NS, 0, 64, 0));
    printf(" %s\n", sorted_ok(ok64, k64, vo, NS, ~(ull)0, 0) ? "ok" : "FAIL");
    printf("empty bit range %d", __mcuda_sort_pairs_bits(ok64, ko, 8, v64, vo, 8, NS, 5, 5, 0));
    printf(" %s\n", memcmp(ko, ok64, sizeof ko) == 0 && memcmp(vo, v64, sizeof vo) == 0 ? "copied" : "FAIL");
    printf("bad arguments %d", __mcuda_sort_pairs(k64, 3, v64, 8, NS, 0));
    printf(" %d", __mcuda_sort_pairs(k64, 8, v64, 2, NS, 0));
    printf(" %d\n", __mcuda_sort_pairs_bits(k64, k64, 4, v64, v64, 8, NS, 0, 33, 0));
}

/* ---- kernel launches through the PTX engine ---- */

#ifdef RT_TEST_ENGINE
/* y[i] = a * x[i] + y[i] for i = blockIdx.x * blockDim.x + threadIdx.x < n, as nvcc writes it; and a kernel
   that adds the device global gtab[threadIdx.x & 3] to y[threadIdx.x]. */
/* (in pieces: a C89 compiler need not take a string literal of more than 509 characters) */
static const char *const ptx_parts[] = {
    "//\n// Generated by hand in the style of NVIDIA NVVM Compiler\n//\n\n"
    ".version 8.5\n.target sm_86\n.address_size 64\n\n"
    ".visible .global .align 4 .f32 gtab[4];\n\n",

    ".visible .entry saxpy(\n"
    "\t.param .u32 saxpy_param_0,\n"
    "\t.param .f32 saxpy_param_1,\n"
    "\t.param .u64 saxpy_param_2,\n"
    "\t.param .u64 saxpy_param_3\n"
    ")\n{\n"
    "\t.reg .pred \t%p<2>;\n"
    "\t.reg .f32 \t%f<5>;\n"
    "\t.reg .b32 \t%r<6>;\n"
    "\t.reg .b64 \t%rd<7>;\n\n",

    "\tld.param.u32 \t%r2, [saxpy_param_0];\n"
    "\tld.param.f32 \t%f1, [saxpy_param_1];\n"
    "\tld.param.u64 \t%rd1, [saxpy_param_2];\n"
    "\tld.param.u64 \t%rd2, [saxpy_param_3];\n"
    "\tmov.u32 \t%r3, %ctaid.x;\n"
    "\tmov.u32 \t%r4, %ntid.x;\n"
    "\tmov.u32 \t%r5, %tid.x;\n"
    "\tmad.lo.s32 \t%r1, %r3, %r4, %r5;\n"
    "\tsetp.ge.s32 \t%p1, %r1, %r2;\n"
    "\t@%p1 bra \t$L__BB0_2;\n\n",

    "\tcvt.s64.s32 \t%rd3, %r1;\n"
    "\tshl.b64 \t%rd4, %rd3, 2;\n"
    "\tadd.s64 \t%rd5, %rd1, %rd4;\n"
    "\tadd.s64 \t%rd6, %rd2, %rd4;\n"
    "\tld.global.f32 \t%f2, [%rd5];\n"
    "\tld.global.f32 \t%f3, [%rd6];\n"
    "\tfma.rn.f32 \t%f4, %f1, %f2, %f3;\n"
    "\tst.global.f32 \t[%rd6], %f4;\n\n"
    "$L__BB0_2:\n"
    "\tret;\n\n}\n\n",

    ".visible .entry addg(\n"
    "\t.param .u64 addg_param_0\n"
    ")\n{\n"
    "\t.reg .f32 \t%f<4>;\n"
    "\t.reg .b32 \t%r<3>;\n"
    "\t.reg .b64 \t%rd<8>;\n\n"
    "\tld.param.u64 \t%rd1, [addg_param_0];\n"
    "\tmov.u32 \t%r1, %tid.x;\n"
    "\tand.b32 \t%r2, %r1, 3;\n"
    "\tcvt.u64.u32 \t%rd2, %r1;\n"
    "\tshl.b64 \t%rd3, %rd2, 2;\n"
    "\tadd.s64 \t%rd4, %rd1, %rd3;\n",

    "\tcvt.u64.u32 \t%rd5, %r2;\n"
    "\tshl.b64 \t%rd6, %rd5, 2;\n"
    "\tmov.u64 \t%rd7, gtab;\n"
    "\tadd.s64 \t%rd7, %rd7, %rd6;\n"
    "\tld.global.f32 \t%f1, [%rd7];\n"
    "\tld.global.f32 \t%f2, [%rd4];\n"
    "\tadd.f32 \t%f3, %f2, %f1;\n"
    "\tst.global.f32 \t[%rd4], %f3;\n"
    "\tret;\n\n}\n"
};

static char ptx[4096];
static struct __mcuda_module module; /* what the compiler emits once per translation unit */

#define NX 1000

static void test_engine(void)
{
    static const char bad_ptx[] = ".version 8.5\n.target sm_86\n.address_size 64\n\nthis is not PTX at all;\n.visible .entry broken(\n{ nor this\n";
    static struct __mcuda_module bad = {bad_ptx, 0, "bad.cu"};
    float *x = 0, *y = 0, a = 2.5f, hx[NX], hy[NX], g[4], *sym;
    unsigned grid[3], block[3], n = NX;
    void *args[4];
    cudaStream_t st = 0;
    CUmodule mod = 0, mod2 = 0;
    CUfunction fn = 0, fg = 0, fnone = 0;
    CUdeviceptr gp = 0, cx = 0, cy = 0;
    size_t gbytes = 0;
    char log[200];
    CUjit_option opt[2];
    void *optv[2];
    struct {
        unsigned n;
        float a;
        float *x, *y;
    } pbuf;
    size_t pbytes = sizeof pbuf;
    void *extra[5];
    int i, ok;
    printf("-- launches\n");
    for (i = 0; i < (int)(sizeof ptx_parts / sizeof ptx_parts[0]); i++)
        strcat(ptx, ptx_parts[i]);
    module.ptx = ptx;
    module.handle = 0;
    module.name = "rt_test.cu";
    cudaMalloc((void **)&x, NX * sizeof(float));
    cudaMalloc((void **)&y, NX * sizeof(float));
    cudaStreamCreateWithFlags(&st, cudaStreamNonBlocking);
    for (i = 0; i < NX; i++) {
        hx[i] = (float)i;
        hy[i] = 1.0f;
    }
    cudaMemcpy(x, hx, sizeof hx, cudaMemcpyHostToDevice);
    cudaMemcpy(y, hy, sizeof hy, cudaMemcpyHostToDevice);
    grid[0] = (NX + 255) / 256;
    grid[1] = 1;
    grid[2] = 1;
    block[0] = 256;
    block[1] = 1;
    block[2] = 1;
    args[0] = &n;
    args[1] = &a;
    args[2] = &x;
    args[3] = &y;
    printf("saxpy %d\n", __mcuda_launch(&module, "saxpy", grid, block, 0, st, args));
    cudaMemcpy(hy, y, sizeof hy, cudaMemcpyDeviceToHost);
    for (ok = 1, i = 0; i < NX; i++)
        ok = ok && hy[i] == 2.5f * (float)i + 1.0f;
    check("saxpy result", ok);
    n = 10; /* only the first 10 elements: the bounds check in the kernel */
    printf("saxpy again %d\n", __mcuda_launch(&module, "saxpy", grid, block, 0, 0, args));
    cudaMemcpy(hy, y, sizeof hy, cudaMemcpyDeviceToHost);
    for (ok = 1, i = 0; i < NX; i++)
        ok = ok && hy[i] == (i < 10 ? 5.0f : 2.5f) * (float)i + 1.0f;
    check("saxpy bounds", ok);

    /* a device global: found by name, written with cudaMemcpyToSymbol, read by a kernel */
    sym = (float *)__mcuda_symbol(&module, "gtab");
    printf("symbol %d", sym != 0);
    printf(", unknown %d\n", __mcuda_symbol(&module, "nothing") == 0);
    g[0] = 10;
    g[1] = 20;
    g[2] = 30;
    g[3] = 40;
    printf("to symbol %d", (int)cudaMemcpyToSymbol(sym, g, sizeof g, 0, cudaMemcpyHostToDevice));
    printf(" %d\n", (int)cudaMemcpyToSymbol(sym, &a, sizeof a, 2 * sizeof(float), cudaMemcpyHostToDevice));
    memset(g, 0, sizeof g);
    printf("from symbol %d", (int)cudaMemcpyFromSymbol(g, sym, sizeof g, 0, cudaMemcpyDeviceToHost));
    printf(": %g", (double)g[0]);
    printf(" %g", (double)g[1]);
    printf(" %g", (double)g[2]);
    printf(" %g\n", (double)g[3]);
    cudaMemset(y, 0, 8 * sizeof(float));
    grid[0] = 1;
    block[0] = 8;
    args[0] = &y;
    printf("addg %d:", __mcuda_launch(&module, "addg", grid, block, 0, st, args));
    cudaMemcpy(hy, y, 8 * sizeof(float), cudaMemcpyDeviceToHost);
    for (i = 0; i < 8; i++)
        printf(" %g", (double)hy[i]);
    printf("\n");

    /* a kernel that another translation unit defines: found through that unit's registered module, which is
       loaded on demand, and so is the device function the kernel calls, defined by this unit's module */
    {
        static const char other_ptx[] =
            ".version 8.5\n.target sm_86\n.address_size 64\n"
            ".visible .entry elsewhere(.param .u64 out)\n{\n"
            "  .reg .u64 %rd1;\n  .reg .f32 %f1;\n"
            "  ld.param.u64 %rd1, [out];\n  mov.f32 %f1, 0f42F60000;\n  st.global.f32 [%rd1], %f1;\n  ret;\n}\n";
        static struct __mcuda_module other = {other_ptx, 0, "other.cu", 0};
        void *oa[1];
        __mcuda_register(&other);
        oa[0] = &y;
        grid[0] = block[0] = 1;
        printf("kernel of another unit %d:", __mcuda_launch(&module, "elsewhere", grid, block, 0, 0, oa));
        cudaMemcpy(hy, y, sizeof(float), cudaMemcpyDeviceToHost);
        printf(" %g\n", (double)hy[0]);
    }

    /* launches that must fail, and what they leave as the last error */
    args[0] = &n;
    printf("clean %d\n", (int)cudaGetLastError());
    printf("no such kernel %d", __mcuda_launch(&module, "nokernel", grid, block, 0, 0, args));
    printf(" (%s)\n", cudaGetErrorName(cudaGetLastError()));
    grid[0] = 0;
    printf("zero grid %d", __mcuda_launch(&module, "saxpy", grid, block, 0, 0, args));
    grid[0] = 1;
    block[0] = 1025;
    printf(", block of 1025 %d", __mcuda_launch(&module, "saxpy", grid, block, 0, 0, args));
    block[0] = 64;
    block[1] = 32;
    printf(", block of 64x32 %d", __mcuda_launch(&module, "saxpy", grid, block, 0, 0, args));
    block[1] = 1;
    printf(", too much shared memory %d", __mcuda_launch(&module, "saxpy", grid, block, 200000, 0, args));
    printf(", bad stream %d", __mcuda_launch(&module, "saxpy", grid, block, 0, (void *)x, args));
    printf(" (%s)\n", cudaGetErrorName(cudaGetLastError()));
    printf("unparsable module %d", __mcuda_launch(&bad, "broken", grid, block, 0, 0, args));
    printf(" (%s", cudaGetErrorName(cudaGetLastError()));
    printf("), symbol %d\n", __mcuda_symbol(&bad, "x") == 0);
    {
        float *nullp = 0;
        n = 4;
        args[2] = &nullp; /* x = 0: the kernel faults */
        block[0] = 4;
        printf("faulting kernel %d", __mcuda_launch(&module, "saxpy", grid, block, 0, 0, args));
        printf(" (%s)\n", cudaGetErrorName(cudaGetLastError()));
        args[2] = &x;
    }

    /* the same kernels through the driver API */
    printf("load %d", (int)cuModuleLoadData(&mod, ptx));
    printf(", functions %d", (int)cuModuleGetFunction(&fn, mod, "saxpy"));
    printf(" %d", (int)cuModuleGetFunction(&fg, mod, "addg"));
    printf(", missing %d\n", (int)cuModuleGetFunction(&fnone, mod, "nokernel"));
    printf("global %d", (int)cuModuleGetGlobal(&gp, &gbytes, mod, "gtab"));
    printf(" %lu", (unsigned long)gbytes);
    printf(", missing %d\n", (int)cuModuleGetGlobal(&cx, &gbytes, mod, "nothing"));
    printf("a second copy of the global %d\n", (void *)gp != (void *)sym);
    cuMemAlloc(&cx, NX * sizeof(float));
    cuMemAlloc(&cy, NX * sizeof(float));
    for (i = 0; i < NX; i++) {
        hx[i] = (float)(i % 17);
        hy[i] = -1.0f;
    }
    cuMemcpyHtoD(cx, hx, sizeof hx);
    cuMemcpyHtoD(cy, hy, sizeof hy);
    n = NX;
    a = 3.0f;
    args[0] = &n;
    args[1] = &a;
    args[2] = &cx;
    args[3] = &cy;
    printf("cuLaunchKernel %d\n", (int)cuLaunchKernel(fn, (NX + 127) / 128, 1, 1, 128, 1, 1, 0, 0, args, 0));
    cuMemcpyDtoH(hy, cy, sizeof hy);
    for (ok = 1, i = 0; i < NX; i++)
        ok = ok && hy[i] == 3.0f * (float)(i % 17) - 1.0f;
    check("driver saxpy result", ok);
    pbuf.n = NX;
    pbuf.a = -3.0f;
    pbuf.x = (float *)cx;
    pbuf.y = (float *)cy;
    extra[0] = CU_LAUNCH_PARAM_BUFFER_POINTER;
    extra[1] = &pbuf;
    extra[2] = CU_LAUNCH_PARAM_BUFFER_SIZE;
    extra[3] = &pbytes;
    extra[4] = CU_LAUNCH_PARAM_END;
    printf("cuLaunchKernel with a parameter buffer %d\n", (int)cuLaunchKernel(fn, 4, 1, 1, 250, 1, 1, 0, st, 0, extra));
    cuMemcpyDtoH(hy, cy, sizeof hy);
    for (ok = 1, i = 0; i < NX; i++)
        ok = ok && hy[i] == -1.0f;
    check("parameter buffer result", ok);
    g[0] = 1;
    g[1] = 2;
    g[2] = 3;
    g[3] = 4;
    cuMemcpyHtoD(gp, g, sizeof g);
    args[0] = &cy;
    printf("driver addg %d:", (int)cuLaunchKernel(fg, 1, 1, 1, 6, 1, 1, 0, 0, args, 0));
    cuMemcpyDtoH(hy, cy, 6 * sizeof(float));
    for (i = 0; i < 6; i++)
        printf(" %g", (double)hy[i]);
    printf("\n");
    printf("bad launches %d", (int)cuLaunchKernel(fn, 0, 1, 1, 128, 1, 1, 0, 0, args, 0));
    printf(" %d\n", (int)cuLaunchKernel(fn, 1, 1, 1, 2048, 1, 1, 0, 0, args, 0));
    opt[0] = CU_JIT_ERROR_LOG_BUFFER;
    optv[0] = log;
    opt[1] = CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES;
    optv[1] = (void *)sizeof log;
    log[0] = 0;
    printf("load bad PTX %d", (int)cuModuleLoadDataEx(&mod2, bad_ptx, 2, opt, optv));
    printf(", a log %d\n", log[0] != 0 && mod2 == 0);
    printf("unload %d\n", (int)cuModuleUnload(mod));
    cuMemFree(cx);
    cuMemFree(cy);
    cudaFree(x);
    cudaFree(y);
    cudaStreamDestroy(st);
    printf("run-time error left %d\n", (int)cudaGetLastError());
}
#endif

int main(void)
{
    test_errors();
    test_device();
    test_memory();
    test_streams();
    test_vm();
    test_driver();
    test_cublaslt();
    test_algo();
#ifdef RT_TEST_ENGINE
    test_engine();
#endif
    printf("%s\n", failures ? "FAILED" : "all ok");
    return failures != 0;
}
