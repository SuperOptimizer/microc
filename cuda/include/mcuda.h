/* mcuda: the interface between code compiled from CUDA sources and the run-time library.
   Plain C; also valid C++. Nothing here exists in NVIDIA's CUDA: these are the entry points that the
   compiler emits calls to (kernel launches, device globals) and the helpers that the cub headers call. */
#ifndef MCUDA_H
#define MCUDA_H
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- compiler -> run time ----
   One __mcuda_module per translation unit. ptx is the NUL-terminated PTX text of the unit (it must stay
   valid for the life of the program: it is not copied). handle starts as 0; the run time parses the text on
   first use and keeps the parsed module there. name is for diagnostics (the source file) and may be 0.
   syms (may be 0) lists the host objects the device code names: the PTX declares each ".extern .global" and
   its address is the one given here; the list ends with a name of 0. */
struct __mcuda_sym {
    const char *name;
    void *addr;
};
struct __mcuda_module {
    const char *ptx;
    void *handle;
    const char *name;
    const struct __mcuda_sym *syms;
};

/* Run kernel `kernel` (its PTX .entry name) of module m over grid[0..2] blocks of block[0..2] threads with
   shared_bytes of dynamic shared memory, synchronously. args[i] points at the value of parameter i, in
   declaration order. stream is a cudaStream_t (0: the default stream); since everything is synchronous it
   only gets checked. Returns a cudaError_t value: 0, or
     1   cudaErrorInvalidValue           m, kernel or args is null; too much shared memory
                                         (dynamic + static > 102400 bytes)
     9   cudaErrorInvalidConfiguration   a zero grid or block dimension, a block of more than 1024 threads or
                                         beyond 1024 x 1024 x 64, a grid beyond 2147483647 x 65535 x 65535,
                                         a block that breaks the kernel's .maxntid / .reqntid
     98  cudaErrorInvalidDeviceFunction  the module has no such entry
     218 cudaErrorInvalidPtx             the PTX does not parse or the kernel body does not decode
     400 cudaErrorInvalidResourceHandle  stream is not a stream
     719 cudaErrorLaunchFailure          the kernel faulted
   A non-zero result is also recorded as the calling thread's last error, which cudaGetLastError() returns
   and clears, as after a failed <<< >>> launch with NVIDIA's run time. */
/* Announces a translation unit's module before its first launch, so that names it defines (kernels, device
   functions, device variables) are found when another unit uses them. May be called from a constructor. */
void __mcuda_register(struct __mcuda_module *m);
int __mcuda_launch(struct __mcuda_module *m, const char *kernel, const unsigned grid[3], const unsigned block[3],
                   unsigned long shared_bytes, void *stream, void **args);

/* Host address of the device global (a PTX .global or .const) `name` of module m, which is also its device
   address; 0 if the module has none (or does not parse). cudaMemcpyToSymbol and friends take this address. */
void *__mcuda_symbol(struct __mcuda_module *m, const char *name);

/* ---- helpers for the cub headers (algo.c) ----
   Device arrays are host arrays; n counts elements. Nothing here needs temporary storage from the caller
   (a cub call with d_temp_storage == nullptr only reports a size: the header answers that itself, e.g.
   with 1, and does not call the helper). Sums wrap modulo 2^32 / 2^64, so the unsigned helpers serve
   signed integers too. in == out is allowed everywhere. */
void __mcuda_exclusive_sum_u32(const unsigned *in, unsigned *out, unsigned long n); /* out[i] = in[0] + ... + in[i-1]; out[0] = 0 */
void __mcuda_exclusive_sum_u64(const unsigned long long *in, unsigned long long *out, unsigned long n);
void __mcuda_inclusive_sum_u32(const unsigned *in, unsigned *out, unsigned long n); /* out[i] = in[0] + ... + in[i] */
void __mcuda_inclusive_sum_u64(const unsigned long long *in, unsigned long long *out, unsigned long n);
/* The same with a start value added to every output (cub's ExclusiveScan with an initial value and Sum). */
void __mcuda_exclusive_scan_sum_u32(const unsigned *in, unsigned *out, unsigned long n, unsigned init);
void __mcuda_exclusive_scan_sum_u64(const unsigned long long *in, unsigned long long *out, unsigned long n, unsigned long long init);

/* Stable least-significant-digit radix sort of unsigned keys of key_bytes (4 or 8) bytes, carrying one
   value of val_bytes (0: keys only, 4 or 8; any bit pattern, e.g. float) per key.
   Only key bits [begin_bit, end_bit) are compared (the whole key: 0, 8 * key_bytes), as in cub's
   SortPairs(..., begin_bit, end_bit); elements whose keys agree in those bits keep their order.
   Reads keys_in / vals_in and writes keys_out / vals_out; the inputs are left untouched unless an output is
   the same array as its input, which is allowed (keys_in == keys_out and/or vals_in == vals_out: in place).
   An output must not overlap anything else. Returns 0, or -1 on a bad argument or when memory runs out
   (then the outputs are untouched). */
int __mcuda_sort_pairs_bits(const void *keys_in, void *keys_out, int key_bytes, const void *vals_in, void *vals_out,
                            int val_bytes, unsigned long n, int begin_bit, int end_bit, int descending);
/* In place, whole keys. */
int __mcuda_sort_pairs(void *keys, int key_bytes, void *vals, int val_bytes, unsigned long n, int descending);
void __mcuda_sort_pairs_u32_u32(unsigned *keys, unsigned *vals, unsigned long n);
void __mcuda_sort_pairs_u32_u64(unsigned *keys, unsigned long long *vals, unsigned long n);
void __mcuda_sort_pairs_u64_u32(unsigned long long *keys, unsigned *vals, unsigned long n);
void __mcuda_sort_pairs_u64_u64(unsigned long long *keys, unsigned long long *vals, unsigned long n);
void __mcuda_sort_keys_u32(unsigned *keys, unsigned long n);
void __mcuda_sort_keys_u64(unsigned long long *keys, unsigned long n);

#ifdef MCUDA_INTERNAL
/* ---- shared by rt.c, driver.c and cublaslt.c; not for user programs ---- */
#define MCUDA_MEM_HOST 1   /* the cudaMemoryType numbers */
#define MCUDA_MEM_DEVICE 2
#define MCUDA_MEM_MANAGED 3
/* Allocate n bytes (n > 0) aligned for the kind and enter them in the allocation table. Returns a
   cudaError_t / CUresult number (0, 2 out of memory). */
int mcuda_mem_alloc(void **out, unsigned long n, int type);
/* Free what mcuda_mem_alloc gave: 0, or -1 if p is not the start of a live allocation whose type is in
   the mask `types` (bit 1 << MCUDA_MEM_x). */
int mcuda_mem_free(void *p, int types);
/* Enter / remove a range that somebody else owns (the reserved ranges of cuMemAddressReserve, registered
   host memory). mcuda_mem_track returns -2 if the range overlaps a live one, -1 when memory runs out. */
int mcuda_mem_track(void *p, unsigned long n, int type);
int mcuda_mem_untrack(void *p);
/* Device bytes that are backed outside the table (cuMemCreate): delta may be negative. Returns 0, or 2 when
   the device is full. */
int mcuda_mem_charge(long delta);
/* The allocation containing p: its type (0 if none), start and size. */
int mcuda_mem_find(const void *p, void **base, unsigned long *size);
void mcuda_mem_info(unsigned long *free_bytes, unsigned long *total_bytes);
int mcuda_dev_count(void); /* 1, or 0 when CUDA_VISIBLE_DEVICES hides the device */
/* A device attribute by its cudaDeviceAttr / CUdevice_attribute number: 0, or -1 if unknown. */
int mcuda_dev_attr(int attr, int *value);
/* The checks of a launch that do not need the stream: 0, 1 or 9 as __mcuda_launch documents. */
int mcuda_launch_check(void *func, const unsigned grid[3], const unsigned block[3], unsigned long shared_bytes);
/* 1 if s is 0, one of the two special default streams, or a live stream. */
int mcuda_stream_ok(void *s);
/* driver.c: a cu* function by name, 0 if there is none. */
void *mcuda_driver_entry(const char *name);
#endif

#ifdef __cplusplus
}
#endif
#endif
