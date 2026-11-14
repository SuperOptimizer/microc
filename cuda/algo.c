/* What the cub headers need on the host: prefix sums and a stable radix sort over device arrays, which are
   host arrays. mcuda.h documents the entry points. Nothing here keeps state. */
#include "mc.h"
#include <mcuda.h>
#include <stdlib.h>
#include <string.h>

/* 64 bits, as u64. The public prototypes use this spelling and so do the loops over the callers' arrays:
   the compiler's aliasing rules go by type name. */
typedef unsigned long long ull;

/* ---- prefix sums ---- */

void __mcuda_exclusive_scan_sum_u32(const unsigned *in, unsigned *out, unsigned long n, unsigned init)
{
    unsigned long i;
    u32 sum = init, v;
    for (i = 0; i < n; i++) {
        v = in[i];
        out[i] = sum;
        sum += v;
    }
}

void __mcuda_exclusive_scan_sum_u64(const ull *in, ull *out, unsigned long n, ull init)
{
    unsigned long i;
    ull sum = init, v;
    for (i = 0; i < n; i++) {
        v = in[i];
        out[i] = sum;
        sum += v;
    }
}

void __mcuda_exclusive_sum_u32(const unsigned *in, unsigned *out, unsigned long n) { __mcuda_exclusive_scan_sum_u32(in, out, n, 0); }
void __mcuda_exclusive_sum_u64(const ull *in, ull *out, unsigned long n) { __mcuda_exclusive_scan_sum_u64(in, out, n, 0); }

void __mcuda_inclusive_sum_u32(const unsigned *in, unsigned *out, unsigned long n)
{
    unsigned long i;
    u32 sum = 0;
    for (i = 0; i < n; i++) {
        sum += in[i];
        out[i] = sum;
    }
}

void __mcuda_inclusive_sum_u64(const ull *in, ull *out, unsigned long n)
{
    unsigned long i;
    ull sum = 0;
    for (i = 0; i < n; i++) {
        sum += in[i];
        out[i] = sum;
    }
}

/* ---- radix sort ----
   Least significant digit first, 8 bits a pass (the last pass of a bit range may be narrower). The
   histograms of all passes come from one scan of the keys; a pass in which every key has the same digit
   moves nothing and is skipped. Each pass is a stable counting sort from one pair of arrays to another.
   For a descending sort the digit is complemented, which keeps equal keys in their order. */

#define RADIX 8
#define NBUCKET (1 << RADIX)
#define MAXPASS 8

typedef struct {
    int shift;
    u32 mask;
} Pass;

/* One scatter pass for keys of type KT that carry a value of type VT. start[d] is where digit d begins. */
#define DEF_PASS_KV(NAME, KT, VT)                                                                               \
    static void NAME(const void *ksrc, void *kdst, const void *vsrc, void *vdst, u64 n, Pass p, int desc, u64 *start) \
    {                                                                                                           \
        const KT *ks = (const KT *)ksrc;                                                                        \
        KT *kd = (KT *)kdst;                                                                                    \
        const VT *vs = (const VT *)vsrc;                                                                        \
        VT *vd = (VT *)vdst;                                                                                    \
        u32 flip = desc ? p.mask : 0;                                                                           \
        u64 i, at;                                                                                              \
        for (i = 0; i < n; i++) {                                                                               \
            KT key = ks[i];                                                                                     \
            at = start[((u32)(key >> p.shift) & p.mask) ^ flip]++;                                              \
            kd[at] = key;                                                                                       \
            vd[at] = vs[i];                                                                                     \
        }                                                                                                       \
    }

#define DEF_PASS_K(NAME, KT)                                                                                    \
    static void NAME(const void *ksrc, void *kdst, const void *vsrc, void *vdst, u64 n, Pass p, int desc, u64 *start) \
    {                                                                                                           \
        const KT *ks = (const KT *)ksrc;                                                                        \
        KT *kd = (KT *)kdst;                                                                                    \
        u32 flip = desc ? p.mask : 0;                                                                           \
        u64 i;                                                                                                  \
        (void)vsrc;                                                                                             \
        (void)vdst;                                                                                             \
        for (i = 0; i < n; i++) {                                                                               \
            KT key = ks[i];                                                                                     \
            kd[start[((u32)(key >> p.shift) & p.mask) ^ flip]++] = key;                                         \
        }                                                                                                       \
    }

DEF_PASS_K(pass_k32, u32)
DEF_PASS_K(pass_k64, ull)
DEF_PASS_KV(pass_k32_v32, u32, u32)
DEF_PASS_KV(pass_k32_v64, u32, ull)
DEF_PASS_KV(pass_k64_v32, ull, u32)
DEF_PASS_KV(pass_k64_v64, ull, ull)

typedef void (*PassFn)(const void *, void *, const void *, void *, u64, Pass, int, u64 *);

/* count[p][d]: how many keys have digit d in pass p. */
static void histograms(const void *keys, int key_bytes, u64 n, const Pass *pass, int npass, int desc, u64 (*count)[NBUCKET])
{
    u64 i;
    int p;
    memset(count, 0, (size_t)npass * sizeof count[0]);
    if (key_bytes == 4) {
        const u32 *k = (const u32 *)keys;
        for (i = 0; i < n; i++) {
            u32 key = k[i];
            for (p = 0; p < npass; p++)
                count[p][((key >> pass[p].shift) & pass[p].mask) ^ (desc ? pass[p].mask : 0)]++;
        }
    } else {
        const ull *k = (const ull *)keys;
        for (i = 0; i < n; i++) {
            ull key = k[i];
            for (p = 0; p < npass; p++)
                count[p][((u32)(key >> pass[p].shift) & pass[p].mask) ^ (desc ? pass[p].mask : 0)]++;
        }
    }
}

int __mcuda_sort_pairs_bits(const void *keys_in, void *keys_out, int key_bytes, const void *vals_in, void *vals_out,
                            int val_bytes, unsigned long n, int begin_bit, int end_bit, int descending)
{
    Pass pass[MAXPASS];
    u64 (*count)[NBUCKET];
    u64 kn = (u64)n * (u64)key_bytes, vn = (u64)n * (u64)val_bytes;
    const void *ksrc, *vsrc;
    void *ktmp = 0, *vtmp = 0, *kdst, *vdst;
    PassFn fn;
    int npass = 0, todo = 0, p, d, bit, inplace, vals_inplace;
    u8 live[MAXPASS];

    if ((key_bytes != 4 && key_bytes != 8) || (val_bytes != 0 && val_bytes != 4 && val_bytes != 8))
        return -1;
    if (begin_bit < 0 || end_bit < begin_bit || end_bit > 8 * key_bytes)
        return -1;
    if (n && (!keys_in || !keys_out || (val_bytes && (!vals_in || !vals_out))))
        return -1;
    if (key_bytes == 4)
        fn = val_bytes == 0 ? pass_k32 : val_bytes == 4 ? pass_k32_v32 : pass_k32_v64;
    else
        fn = val_bytes == 0 ? pass_k64 : val_bytes == 4 ? pass_k64_v32 : pass_k64_v64;
    for (bit = begin_bit; bit < end_bit; bit += RADIX) {
        int width = end_bit - bit < RADIX ? end_bit - bit : RADIX;
        pass[npass].shift = bit;
        pass[npass].mask = ((u32)1 << width) - 1;
        npass++;
    }
    count = (u64 (*)[NBUCKET])malloc((size_t)(npass ? npass : 1) * sizeof count[0]);
    if (!count)
        return -1;
    histograms(keys_in, key_bytes, n, pass, npass, descending, count);
    for (p = 0; p < npass; p++) {
        u64 sum = 0, c;
        live[p] = 1;
        for (d = 0; d < NBUCKET; d++) {
            c = count[p][d];
            if (c == n)
                live[p] = 0; /* one digit for all (or nothing to sort): the pass would be the identity */
            count[p][d] = sum; /* from counts to starting positions */
            sum += c;
        }
        todo += live[p];
    }
    /* One array in place and the other not: bring the other to its output first, then sort in place. */
    vals_inplace = val_bytes ? vals_in == vals_out : keys_in == keys_out;
    if ((keys_in == keys_out) != vals_inplace && todo) {
        if (keys_in != keys_out) {
            memcpy(keys_out, keys_in, kn);
            keys_in = keys_out;
        } else {
            memcpy(vals_out, vals_in, vn);
            vals_in = vals_out;
        }
    }
    inplace = keys_in == keys_out;
    /* The passes go in -> a -> b -> a ... where a and b are the outputs and a temporary pair, arranged so
       that the last pass writes the outputs; in place the first pass must write the temporaries, and after
       an odd number of passes the result is copied back. A single pass out of place needs no temporary. */
    if (todo > 1 || (todo == 1 && inplace)) {
        ktmp = malloc(kn ? kn : 1);
        vtmp = malloc(vn ? vn : 1);
        if (!ktmp || !vtmp) {
            free(ktmp);
            free(vtmp);
            free(count);
            return -1;
        }
    }
    ksrc = keys_in;
    vsrc = vals_in;
    for (p = 0; p < npass; p++) {
        int to_out;
        if (!live[p])
            continue;
        todo--; /* passes left after this one */
        to_out = inplace ? ksrc != keys_out : todo % 2 == 0;
        kdst = to_out ? keys_out : ktmp;
        vdst = to_out ? vals_out : vtmp;
        fn(ksrc, kdst, vsrc, vdst, n, pass[p], descending, count[p]);
        ksrc = kdst;
        vsrc = vdst;
    }
    if (ksrc != keys_out) { /* nothing moved (out of place), or in place and the result is in the temporaries */
        memcpy(keys_out, ksrc, kn);
        if (val_bytes)
            memcpy(vals_out, vsrc, vn);
    }
    free(ktmp);
    free(vtmp);
    free(count);
    return 0;
}

int __mcuda_sort_pairs(void *keys, int key_bytes, void *vals, int val_bytes, unsigned long n, int descending)
{
    return __mcuda_sort_pairs_bits(keys, keys, key_bytes, vals, vals, val_bytes, n, 0, 8 * key_bytes, descending);
}

void __mcuda_sort_pairs_u32_u32(unsigned *keys, unsigned *vals, unsigned long n) { __mcuda_sort_pairs(keys, 4, vals, 4, n, 0); }
void __mcuda_sort_pairs_u32_u64(unsigned *keys, ull *vals, unsigned long n) { __mcuda_sort_pairs(keys, 4, vals, 8, n, 0); }
void __mcuda_sort_pairs_u64_u32(ull *keys, unsigned *vals, unsigned long n) { __mcuda_sort_pairs(keys, 8, vals, 4, n, 0); }
void __mcuda_sort_pairs_u64_u64(ull *keys, ull *vals, unsigned long n) { __mcuda_sort_pairs(keys, 8, vals, 8, n, 0); }
void __mcuda_sort_keys_u32(unsigned *keys, unsigned long n) { __mcuda_sort_pairs(keys, 4, 0, 0, n, 0); }
void __mcuda_sort_keys_u64(ull *keys, unsigned long n) { __mcuda_sort_pairs(keys, 8, 0, 0, n, 0); }
