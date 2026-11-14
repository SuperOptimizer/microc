/* mcudahw: what the cub headers call (mcuda.h: prefix sums and the stable radix sort). The arrays are device
   memory here: they are brought to the host, worked on by the functions of the CPU run time (cuda/algo.c,
   included below under other names) and sent back. Everything queued on the device finishes first. */
#include "hw.h"
#include <stdlib.h>

#define __mcuda_exclusive_scan_sum_u32 host_exclusive_scan_sum_u32
#define __mcuda_exclusive_scan_sum_u64 host_exclusive_scan_sum_u64
#define __mcuda_exclusive_sum_u32 host_exclusive_sum_u32
#define __mcuda_exclusive_sum_u64 host_exclusive_sum_u64
#define __mcuda_inclusive_sum_u32 host_inclusive_sum_u32
#define __mcuda_inclusive_sum_u64 host_inclusive_sum_u64
#define __mcuda_sort_pairs_bits host_sort_pairs_bits
#define __mcuda_sort_pairs host_sort_pairs
#define __mcuda_sort_pairs_u32_u32 host_sort_pairs_u32_u32
#define __mcuda_sort_pairs_u32_u64 host_sort_pairs_u32_u64
#define __mcuda_sort_pairs_u64_u32 host_sort_pairs_u64_u32
#define __mcuda_sort_pairs_u64_u64 host_sort_pairs_u64_u64
#define __mcuda_sort_keys_u32 host_sort_keys_u32
#define __mcuda_sort_keys_u64 host_sort_keys_u64
#include "../algo.c"
#undef __mcuda_exclusive_scan_sum_u32
#undef __mcuda_exclusive_scan_sum_u64
#undef __mcuda_exclusive_sum_u32
#undef __mcuda_exclusive_sum_u64
#undef __mcuda_inclusive_sum_u32
#undef __mcuda_inclusive_sum_u64
#undef __mcuda_sort_pairs_bits
#undef __mcuda_sort_pairs
#undef __mcuda_sort_pairs_u32_u32
#undef __mcuda_sort_pairs_u32_u64
#undef __mcuda_sort_pairs_u64_u32
#undef __mcuda_sort_pairs_u64_u64
#undef __mcuda_sort_keys_u32
#undef __mcuda_sort_keys_u64

/* 1: p is memory of the device; 0: the host's (a caller that has fetched the operands itself, as the
   headers of the host-memory run time do, or memory of the host that the device sees too) */
static int on_device(const void *p)
{
    unsigned type = 0;
    if (!p || hw_enter())
        return 0;
    if (hw.PointerGetAttribute(&type, 2 /* CU_POINTER_ATTRIBUTE_MEMORY_TYPE */, (CUdeviceptr)(size_t)p))
        return 0;
    return type == 2; /* CU_MEMORYTYPE_DEVICE */
}

/* the n bytes at the device address p, on the host; 0 if that fails (or n is 0) */
static void *fetch(const void *p, u64 n)
{
    void *h;
    if (!p || !n)
        return 0;
    h = malloc((size_t)n);
    if (h && hw.MemcpyDtoH(h, (CUdeviceptr)(size_t)p, (size_t)n)) {
        free(h);
        h = 0;
    }
    return h;
}

static int send(void *p, const void *h, u64 n) { return hw.MemcpyHtoD((CUdeviceptr)(size_t)p, h, (size_t)n) != 0; }

static int begin(void)
{
    if (hw_enter())
        return 1;
    return hw.CtxSynchronize() != 0;
}

/* a prefix sum of n elements of w bytes: kind 0 exclusive from init, 1 inclusive */
static void scan(const void *in, void *out, unsigned long n, int w, int kind, ull init)
{
    void *h;
    if (!n)
        return;
    if (!on_device(in)) {
        if (w == 4) {
            if (kind)
                host_inclusive_sum_u32((const unsigned *)in, (unsigned *)out, n);
            else
                host_exclusive_scan_sum_u32((const unsigned *)in, (unsigned *)out, n, (unsigned)init);
        } else {
            if (kind)
                host_inclusive_sum_u64((const ull *)in, (ull *)out, n);
            else
                host_exclusive_scan_sum_u64((const ull *)in, (ull *)out, n, init);
        }
        return;
    }
    if (begin())
        return;
    h = fetch(in, (u64)n * (u64)w);
    if (!h) {
        hw_err_set(2);
        return;
    }
    if (w == 4) {
        if (kind)
            host_inclusive_sum_u32((const unsigned *)h, (unsigned *)h, n);
        else
            host_exclusive_scan_sum_u32((const unsigned *)h, (unsigned *)h, n, (unsigned)init);
    } else {
        if (kind)
            host_inclusive_sum_u64((const ull *)h, (ull *)h, n);
        else
            host_exclusive_scan_sum_u64((const ull *)h, (ull *)h, n, init);
    }
    if (send(out, h, (u64)n * (u64)w))
        hw_err_set(1);
    free(h);
}

void __mcuda_exclusive_scan_sum_u32(const unsigned *in, unsigned *out, unsigned long n, unsigned init) { scan(in, out, n, 4, 0, init); }
void __mcuda_exclusive_scan_sum_u64(const ull *in, ull *out, unsigned long n, ull init) { scan(in, out, n, 8, 0, init); }
void __mcuda_exclusive_sum_u32(const unsigned *in, unsigned *out, unsigned long n) { scan(in, out, n, 4, 0, 0); }
void __mcuda_exclusive_sum_u64(const ull *in, ull *out, unsigned long n) { scan(in, out, n, 8, 0, 0); }
void __mcuda_inclusive_sum_u32(const unsigned *in, unsigned *out, unsigned long n) { scan(in, out, n, 4, 1, 0); }
void __mcuda_inclusive_sum_u64(const ull *in, ull *out, unsigned long n) { scan(in, out, n, 8, 1, 0); }

int __mcuda_sort_pairs_bits(const void *keys_in, void *keys_out, int key_bytes, const void *vals_in, void *vals_out, int val_bytes,
                            unsigned long n, int begin_bit, int end_bit, int descending)
{
    u64 kn = (u64)n * (u64)key_bytes, vn = val_bytes ? (u64)n * (u64)val_bytes : 0;
    void *k, *v = 0;
    int r;
    if (!n)
        return 0;
    if (!on_device(keys_in))
        return host_sort_pairs_bits(keys_in, keys_out, key_bytes, vals_in, vals_out, val_bytes, n, begin_bit, end_bit, descending);
    if (begin())
        return 1;
    k = fetch(keys_in, kn);
    if (vn)
        v = fetch(vals_in, vn);
    if (!k || (vn && !v)) {
        free(k);
        free(v);
        hw_err_set(2);
        return 2;
    }
    /* (in place on the host copies: the caller's inputs stay as they are unless they are the outputs) */
    r = host_sort_pairs_bits(k, k, key_bytes, v, v, val_bytes, n, begin_bit, end_bit, descending);
    if (!r && (send(keys_out, k, kn) || (vn && send(vals_out, v, vn)))) {
        hw_err_set(1);
        r = 1;
    }
    free(k);
    free(v);
    return r;
}

int __mcuda_sort_pairs(void *keys, int key_bytes, void *vals, int val_bytes, unsigned long n, int descending)
{
    return __mcuda_sort_pairs_bits(keys, keys, key_bytes, vals, vals, val_bytes, n, 0, key_bytes * 8, descending);
}
void __mcuda_sort_pairs_u32_u32(unsigned *keys, unsigned *vals, unsigned long n) { __mcuda_sort_pairs(keys, 4, vals, 4, n, 0); }
void __mcuda_sort_pairs_u32_u64(unsigned *keys, ull *vals, unsigned long n) { __mcuda_sort_pairs(keys, 4, vals, 8, n, 0); }
void __mcuda_sort_pairs_u64_u32(ull *keys, unsigned *vals, unsigned long n) { __mcuda_sort_pairs(keys, 8, vals, 4, n, 0); }
void __mcuda_sort_pairs_u64_u64(ull *keys, ull *vals, unsigned long n) { __mcuda_sort_pairs(keys, 8, vals, 8, n, 0); }
void __mcuda_sort_keys_u32(unsigned *keys, unsigned long n) { __mcuda_sort_pairs(keys, 4, 0, 0, n, 0); }
void __mcuda_sort_keys_u64(ull *keys, unsigned long n) { __mcuda_sort_pairs(keys, 8, 0, 0, n, 0); }
