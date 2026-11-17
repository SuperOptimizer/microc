/* Loops whose count is a whole number of vectors (2026-11-16): the vectoriser then runs no scalar iteration at all,
   and what the code after the loop reads of it (the counter, a pointer stepped through the array, a sum, a
   maximum) comes from the vector loop's end. Every count from 0 to 200 and a few larger ones, so that both ways
   out are taken at every vector width; loops whose last element or last sum-before-the-end is read afterwards
   (those keep a scalar iteration); a loop that runs straight into another one. */
#include <stdio.h>
#include <string.h>

typedef unsigned char u8;
typedef unsigned short u16;

#define N 4200
static u8 a8[N + 64], b8[N + 64];
static int ai[N + 64], bi[N + 64];
static float af[N + 64], bf[N + 64];
static u16 ah[N + 64];

#define NI __attribute__((noinline)) static

NI int cnt_after(u8 *d, const u8 *s, int n) { int i; for (i = 0; i < n; i++) d[i] = (u8)(s[i] + 3); return i * 7 + 1; }
NI long ptr_after(float *d, const float *s, const float *e) { const float *s0 = s; float *d0 = d; while (s < e) *d++ = *s++ * 0.5f + 1.0f; return (long)(s - s0) * 3 + (long)(d - d0); }
NI int sum_after(const int *s, int n, int *ip) { int i, t = 5; for (i = 0; i < n; i++) t += s[i] ^ 3; *ip = i; return t; }
NI int bsum_after(const u8 *s, int n) { int i, t = 0; for (i = 0; i < n; i++) t += s[i]; return t + i; }
NI int max_after(const int *s, int n) { int i, m = -1000000; for (i = 0; i < n; i++) if (s[i] > m) m = s[i]; return m - i; }
NI unsigned down_after(int *d, unsigned n) { while (n--) d[n] = (int)(n * 3u); return n; }
NI int last_elem(int *d, const int *s, int n) { int i, last = -1; for (i = 0; i < n; i++) { last = s[i] * 3; d[i] = last; } return last; }
NI int two_loops(u8 *d, const u8 *s, int n, int m)
{
    int i = 0, j = 0, t = 0;
    if (n > 0)
        do { d[i] = (u8)(s[i] ^ 0x5a); } while (++i < n);
    do { t += d[j] + i; } while (++j < m); /* reads the first loop's counter in every round */
    return t + i + j;
}
NI int both(int *d, const int *s, int n, int flag)
{
    int i = 3;
    if (flag)
        for (i = 0; i < n; i++)
            d[i] = s[i] + flag;
    return i; /* a join of the loop's exit with a way round the loop */
}
NI int step2(u16 *d, const u16 *s, int n) { int i, k = 10; for (i = 0; i < n; i++, k += 3) d[i] = (u16)(s[i] * 3 + 1); return i + k; }
NI int fixed(u8 *d, const u8 *s) { int i; for (i = 0; i < 4096; i++) d[i] = (u8)(s[i] >> 1); return i; }
NI int fixed16(float *d, const float *s, float w) { int k; for (k = 0; k < 16; k++) d[k] += w * s[k]; return k; }

static unsigned long h = 5381;
static void put(long v) { h = h * 33 + (unsigned long)v; }

int main(void)
{
    int n, k, ip;
    for (k = 0; k < N + 64; k++) {
        unsigned r = (unsigned)k * 2654435761u;
        a8[k] = (u8)(r >> 8);
        ai[k] = (int)(r >> 3) % 100000 - 50000;
        af[k] = (float)(int)(r >> 20);
        ah[k] = (u16)(r >> 5);
    }
    for (n = 0; n <= 4104; n += n < 200 ? 1 : n < 4000 ? 317 : 8) {
        int off = n % 5;
        memset(b8, 1, sizeof b8);
        put(cnt_after(b8 + off, a8 + 1, n));
        for (k = 0; k < n + 8; k++) put(b8[k]);
        memset(bf, 0, sizeof bf);
        put(ptr_after(bf + off, af + 2, af + 2 + n));
        for (k = 0; k < n + 8; k++) put((long)bf[k]);
        put(sum_after(ai + off, n, &ip));
        put(ip);
        put(bsum_after(a8 + off, n));
        put(max_after(ai + off, n));
        memset(bi, 0, sizeof bi);
        put((long)down_after(bi + off, (unsigned)n));
        for (k = 0; k < n + 8; k++) put(bi[k]);
        put(last_elem(bi + off, ai + 1, n));
        for (k = 0; k < n + 8; k++) put(bi[k]);
        put(two_loops(b8 + off, a8 + 3, n, n / 2 + 1));
        put(both(bi + off, ai + 2, n, n & 1));
        put(both(bi + off, ai + 2, n, 2));
        for (k = 0; k < n + 8; k++) put(bi[k]);
        put(step2(ah + 2000, ah + off, n < 2000 ? n : 2000));
        for (k = 0; k < 40; k++) put(ah[2000 + k * 50]);
    }
    put(fixed(b8, a8 + 1));
    for (k = 0; k < 4100; k++) put(b8[k]);
    for (k = 0; k < 32; k++) bf[k] = (float)k;
    put(fixed16(bf + 1, af + 5, 0.25f));
    for (k = 0; k < 32; k++) put((long)(bf[k] * 4));
    printf("%lu\n", h);
    return 0;
}
