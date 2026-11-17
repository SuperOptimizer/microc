/* Loops whose element is cut out of 64-bit arithmetic on the counter and on values fixed for the loop, stored
   through an unsigned index that is a sum (2026-11-16): ufsm's test_volcomp_fast fill_sub,
   chunk[off + i] = (uint8_t) ((key * 31 + s * 7 + z * 13 + i * 5 + (i >> 7)) & 255).
   The vectoriser does the tree 32 bits wide and guards the index sum against wrapping; trip counts around the
   vector widths, offsets that leave the stores unaligned, and trees that must stay 64 bits wide (a right shift,
   a division, a value used twice). */
#include <stdio.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef long long i64;

#define N 600
static u8 bb[N + 96];
static u16 hh[N + 96];
static u32 ww[N + 96];
static int src[N + 96];

__attribute__((noinline)) static void fill_sub(u8 *chunk, u64 key, u32 s, u32 z, u32 off, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++)
        chunk[off + i] = (u8)((key * 31 + s * 7 + z * 13 + i * 5 + (i >> 7)) & 255);
}
__attribute__((noinline)) static void fill_from(u8 *chunk, u64 key, u32 off, u32 from, u32 n)
{
    u32 i;
    for (i = from; i < n; i++)
        chunk[off + i] = (u8)(key + (u64)i * 3);
}
__attribute__((noinline)) static void fill16(u16 *p, u64 key, i64 m, u32 off, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++)
        p[off + i] = (u16)(key * i + (m ^ ((u64)i << 3)) - 7);
}
__attribute__((noinline)) static void fill32(u32 *p, const int *a, u64 key, u32 off, int n)
{
    int i;
    for (i = 0; i < n; i++)
        p[off + (u32)i] = (u32)((i64)a[i] * 5 + key - (u64)(i64)i);
}
/* stays 64 bits wide: the right shift and the division need the high bits */
__attribute__((noinline)) static void fill_shr(u8 *p, u64 key, u32 off, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++)
        p[off + i] = (u8)((key + (u64)i * 0x01010101u) >> 28);
}
__attribute__((noinline)) static void fill_div(u32 *p, u64 key, u32 n)
{
    u32 i;
    for (i = 0; i < n; i++)
        p[i] = (u32)((key + i) / 3);
}
/* the 64-bit sum is used twice: it is not cut down */
__attribute__((noinline)) static u64 fill_twice(u32 *p, u64 key, u32 n)
{
    u32 i;
    u64 last = 0;
    for (i = 0; i < n; i++) {
        u64 t = key + (u64)i * 9;
        p[i] = (u32)t;
        last = t;
    }
    return last;
}
/* a signed index sum next to it */
__attribute__((noinline)) static void fill_s(u8 *p, i64 key, int off, int n)
{
    int i;
    for (i = 0; i < n; i++)
        p[off + i] = (u8)(key - (i64)i * i);
}

static unsigned long h = 5381;
static void put(unsigned long v) { h = h * 33 + v; }
static void sum8(void) { u32 k; for (k = 0; k < sizeof bb; k++) put(bb[k]); }
static void sum16(void) { u32 k; for (k = 0; k < sizeof hh / sizeof hh[0]; k++) put(hh[k]); }
static void sum32(void) { u32 k; for (k = 0; k < sizeof ww / sizeof ww[0]; k++) put(ww[k]); }

int main(void)
{
    static const u32 trips[] = { 0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 47, 48, 63, 64, 65, 66, 95, 127, 128, 129, 130, 255, 257, 511, 600 };
    u32 t, off, k;
    for (k = 0; k < N + 96; k++)
        src[k] = (int)(k * 2654435761u) >> 7;
    for (t = 0; t < sizeof trips / sizeof trips[0]; t++)
        for (off = 0; off < 40; off += off < 5 ? 1 : 11) {
            u32 n = trips[t];
            u64 key = 0x123456789abcdef1ull * (t + 1) + off;
            memset(bb, 0xa5, sizeof bb);
            fill_sub(bb, key, t * 3 + 1, off, off, n);
            sum8();
            memset(bb, 0x5a, sizeof bb);
            fill_from(bb, key, off, n / 3, n);
            sum8();
            memset(hh, 0x33, sizeof hh);
            fill16(hh, key, (i64)(key >> 5) - 99, off, n);
            sum16();
            memset(ww, 0x77, sizeof ww);
            fill32(ww, src + (off & 3), key, off, (int)n);
            sum32();
            memset(bb, 0x11, sizeof bb);
            fill_shr(bb, key, off, n);
            sum8();
            memset(ww, 0x22, sizeof ww);
            fill_div(ww + off, key, n);
            sum32();
            put((unsigned long)fill_twice(ww + off, key, n));
            sum32();
            memset(bb, 0x44, sizeof bb);
            fill_s(bb, (i64)key, (int)off, (int)n);
            sum8();
        }
    printf("%lu\n", h);
    return 0;
}
