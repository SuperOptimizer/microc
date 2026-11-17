/* Byte and short loops with comparisons, right shifts and (uint8_t) / (int8_t) conversions (2026-11-16): the
   vectoriser keeps them in lanes of one or two bytes where each value is known to be its lane zero- or
   sign-extended (volcomp's zigzag, mask builders), and must widen to lanes of four bytes where it is not: the
   average (a + b) >> 1, a byte against an int that may not fit a byte, an unsigned byte against a signed one.
   Trip counts around the vector widths, unaligned starts, in-place and neighbouring arrays. */
#include <stdio.h>
#include <string.h>

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef short s16;

#define N 700
static u8 a[N + 80], b[N + 80], d[N + 80];
static s8 sa[N + 80], sb[N + 80];
static u16 h[N + 80], hd[N + 80];
static s16 sh[N + 80];

#define NI __attribute__((noinline)) static

NI void unzz(u8 *u, unsigned n) { unsigned i; for (i = 0; i < n; i++) u[i] = (u8)((unsigned)(u[i] >> 1) ^ (unsigned)(u8)(0u - (u[i] & 1u))); }
NI void zz(u8 *o, const u8 *p, unsigned n)
{
    unsigned i;
    for (i = 1; i < n; i++) {
        u8 t = (u8)(p[i] - p[i - 1]);
        o[i] = (u8)((unsigned)(t << 1) ^ (unsigned)(u8)(0u - ((unsigned)t >> 7)));
    }
}
NI void mask2(u8 *o, const u8 *x, const u8 *y, int n) { int i; for (i = 0; i < n; i++) o[i] = x[i] != 0 && y[i] == 0; }
NI void thr(u8 *o, const u8 *x, int n) { int i; for (i = 0; i < n; i++) o[i] = x[i] > 200 ? 255 : 0; }
NI void thr_s(u8 *o, const s8 *x, int n) { int i; for (i = 0; i < n; i++) o[i] = x[i] > -3 ? 1 : 0; }
NI void lt_u(u8 *o, const u8 *x, const u8 *y, int n) { int i; for (i = 0; i < n; i++) o[i] = x[i] < y[i]; }
NI void lt_s(u8 *o, const s8 *x, const s8 *y, int n) { int i; for (i = 0; i < n; i++) o[i] = x[i] <= y[i]; }
NI void lt_us(u8 *o, const u8 *x, const s8 *y, int n) { int i; for (i = 0; i < n; i++) o[i] = x[i] < y[i]; }
NI void eq_int(u8 *o, const u8 *x, int t, int n) { int i; for (i = 0; i < n; i++) o[i] = x[i] == t; }
NI void eq_byte(u8 *o, const u8 *x, u8 t, int n) { int i; for (i = 0; i < n; i++) o[i] = x[i] == t ? x[i] : (u8)(x[i] >> 2); }
NI void avg(u8 *o, const u8 *x, const u8 *y, int n) { int i; for (i = 0; i < n; i++) o[i] = (u8)((x[i] + y[i] + 1) >> 1); }
NI void shl_shr(u8 *o, const u8 *x, int n) { int i; for (i = 0; i < n; i++) o[i] = (u8)((u8)(x[i] << 3) >> 5); }
NI void sum_shr(u8 *o, const u8 *x, int n) { int i; for (i = 0; i < n; i++) o[i] = (u8)((x[i] * 3) >> 2); }
NI void sar_s(u8 *o, const s8 *x, int n) { int i; for (i = 0; i < n; i++) o[i] = (u8)(x[i] >> 2); }
NI void sel(u8 *o, const u8 *x, const u8 *y, int n) { int i; for (i = 0; i < n; i++) o[i] = (x[i] & 15) >= (y[i] >> 4) ? x[i] : (u8)(y[i] + 7); }
NI void nib(u8 *o, const u8 *x, int n) { int i; for (i = 0; i < n; i++) o[i] = (u8)((x[i] >> 4) | (x[i] << 4)); }
NI void sx_cmp(u8 *o, const u8 *x, int n) { int i; for (i = 0; i < n; i++) o[i] = (s8)x[i] < 0 ? (u8)-x[i] : x[i]; }
NI void h_shr(u16 *o, const u16 *x, int n) { int i; for (i = 0; i < n; i++) o[i] = (u16)((x[i] >> 3) + (u8)x[i]); }
NI void h_sar(u16 *o, const s16 *x, int n) { int i; for (i = 0; i < n; i++) o[i] = (u16)((x[i] >> 2) ^ (s8)x[i]); }
NI void h_cmp(u16 *o, const u16 *x, const s16 *y, int n) { int i; for (i = 0; i < n; i++) o[i] = (u16)((x[i] > 40000) + (y[i] < -100) * 2 + (x[i] * 3 > 100) * 4); }
NI void h_zx16(u16 *o, const u16 *x, int n) { int i; for (i = 0; i < n; i++) o[i] = (u16)((u16)(x[i] * 5) >> 7); }
NI void fixed48(u8 *o, const u8 *x) { int i; for (i = 0; i < 48; i++) o[i] = x[i] >> 3; }
NI void fixed96(u8 *o, const u8 *x) { int i; for (i = 0; i < 96; i++) o[i] = x[i] >= 77; }

static unsigned long hh = 5381;
static void sum8(void) { unsigned k; for (k = 0; k < sizeof d; k++) hh = hh * 33 + d[k]; }
static void sum16(void) { unsigned k; for (k = 0; k < sizeof hd / sizeof hd[0]; k++) hh = hh * 33 + hd[k]; }

int main(void)
{
    static const int trips[] = { 0, 1, 2, 15, 16, 17, 31, 32, 33, 63, 64, 65, 66, 96, 127, 128, 129, 130, 191, 193, 255, 257, 700 };
    unsigned t, k;
    int off;
    for (t = 0; t < sizeof trips / sizeof trips[0]; t++)
        for (off = 0; off < 36; off += off < 4 ? 1 : 15) {
            int n = trips[t];
            for (k = 0; k < N + 80; k++) {
                unsigned r = (k + t * 7 + (unsigned)off) * 2654435761u;
                a[k] = (u8)(r >> 9);
                b[k] = (u8)((r >> 17) % 3 ? r >> 20 : 0);
                sa[k] = (s8)(r >> 13);
                sb[k] = (s8)(r >> 5);
                h[k] = (u16)(r >> 11);
                sh[k] = (s16)(r >> 7);
            }
#define RUN8(call) memset(d, 0xa5, sizeof d); call; sum8()
#define RUN16(call) memset(hd, 0x5a, sizeof hd); call; sum16()
            RUN8((memcpy(d + off, a, (size_t)n), unzz(d + off, (unsigned)n)));
            RUN8(zz(d + off, a + 1, (unsigned)n));
            RUN8(mask2(d + off, a + 2, b + 1, n));
            RUN8(thr(d + off, a + off, n));
            RUN8(thr_s(d + off, sa + 3, n));
            RUN8(lt_u(d + off, a + 1, b + 2, n));
            RUN8(lt_s(d + off, sa + 1, sb + 2, n));
            RUN8(lt_us(d + off, a + 1, sb + 2, n));
            RUN8(eq_int(d + off, a, a[5], n));
            RUN8(eq_int(d + off, a, 256 + a[5], n));
            RUN8(eq_int(d + off, a, -1, n));
            RUN8(eq_byte(d + off, a + 3, a[9], n));
            RUN8(avg(d + off, a + 1, b + 3, n));
            RUN8(shl_shr(d + off, a + 1, n));
            RUN8(sum_shr(d + off, a + 1, n));
            RUN8(sar_s(d + off, sa + 1, n));
            RUN8(sel(d + off, a + 1, b + 3, n));
            RUN8(nib(d + off, a + 2, n));
            RUN8(sx_cmp(d + off, a + 2, n));
            RUN16(h_shr(hd + off, h + 1, n));
            RUN16(h_sar(hd + off, sh + 1, n));
            RUN16(h_cmp(hd + off, h + 1, sh + 3, n));
            RUN16(h_zx16(hd + off, h + 1, n));
            RUN8(fixed48(d + off, a + 1));
            RUN8(fixed96(d + off, a + 1));
        }
    printf("%lu\n", hh);
    return 0;
}
