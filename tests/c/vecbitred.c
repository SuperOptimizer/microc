/* or / and / xor carried round a loop (2026-11-16): ufsm's emb_at asks `any |= s[k]` over signed bytes before it
   uses a cell. Every count from 0 to 150, starts that are not zero, elements of one, two, four and eight bytes,
   signed and unsigned, and the sum next to them. */
#include <stdio.h>

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned short u16;
typedef unsigned long long u64;

#define N 400
static s8 sb[N + 8];
static u8 ub[N + 8];
static u16 uh[N + 8];
static int ai[N + 8];
static u64 aq[N + 8];

#define NI __attribute__((noinline)) static

NI int any_s8(const s8 *s, int n) { int k, any = 0; for (k = 0; k < n; k++) any |= s[k]; return any; }
NI int any_u8(const u8 *s, int n, int start) { int k, any = start; for (k = 0; k < n; k++) any |= s[k]; return any; }
NI int all_u8(const u8 *s, int n) { int k, all = -1; for (k = 0; k < n; k++) all &= s[k]; return all; }
NI int all_s8(const s8 *s, int n, int start) { int k, all = start; for (k = 0; k < n; k++) all &= s[k]; return all; }
NI unsigned par_u16(const u16 *s, int n) { int k; unsigned p = 0x1234; for (k = 0; k < n; k++) p ^= s[k]; return p; }
NI int or_int(const int *s, int n) { int k, o = 0; for (k = 0; k < n; k++) o |= s[k] & 0x00ffff00; return o; }
NI int and_int(const int *s, int n) { int k, o = 0x7fffffff; for (k = 0; k < n; k++) o &= s[k] | 0x11; return o; }
NI u64 xor_q(const u64 *s, int n) { int k; u64 o = 7; for (k = 0; k < n; k++) o ^= s[k]; return o; }
NI u64 or_q(const u64 *s, int n) { int k; u64 o = 0; for (k = 0; k < n; k++) o |= s[k]; return o; }
NI int or_and_sum(const u8 *s, int n, int *sum) { int k, o = 0, t = 0; for (k = 0; k < n; k++) { o |= s[k]; t += s[k]; } *sum = t; return o; }
NI int nz(const s8 *s, int n) { int k, any = 0; for (k = 0; k < n; k++) any |= s[k] != 0; return any; }

static unsigned long h = 5381;
static void put(unsigned long v) { h = h * 33 + v; }

int main(void)
{
    int n, k, off, t;
    for (off = 0; off < 3; off++) {
        for (k = 0; k < N + 8; k++) {
            unsigned r = (unsigned)(k + off * 1000) * 2654435761u;
            /* mostly zeros (ones for the ands), so that which element is set matters */
            sb[k] = (s8)(r % 37 == 0 ? r >> 9 : 0);
            ub[k] = (u8)(r % 41 == 0 ? r >> 11 : off == 2 ? 0xff : 0);
            uh[k] = (u16)(r >> 7);
            ai[k] = (int)(r % 29 == 0 ? r : off == 1 ? -1 : 0);
            aq[k] = r % 31 == 0 ? (u64)r << 27 | r : 0;
        }
        for (n = 0; n <= 150; n++) {
            put((unsigned long)any_s8(sb + off, n));
            put((unsigned long)any_s8(sb + 200, n));
            put((unsigned long)any_u8(ub + off, n, n & 0x300));
            put((unsigned long)all_u8(ub + off, n));
            put((unsigned long)all_s8(sb + off, n, -1 - (n & 4)));
            put(par_u16(uh + off, n));
            put((unsigned long)or_int(ai + off, n));
            put((unsigned long)and_int(ai + off, n));
            put((unsigned long)xor_q(aq + off, n));
            put((unsigned long)or_q(aq + off, n));
            put((unsigned long)or_and_sum(ub + off, n, &t));
            put((unsigned long)t);
            put((unsigned long)nz(sb + off, n));
        }
    }
    printf("%lu\n", h);
    return 0;
}
